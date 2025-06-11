#include "Utils.hpp"
#include "P2PSystem.hpp"
#include "Logger.hpp"
#include <iostream>
#include <vector>
#include <sstream>

namespace {
// REMOVE LATER
inline void dumpMulticastPacket(const std::vector<uint8_t>& buf,
                                const std::string& textPrefix)
{
    if (buf.size() < 34) return;                  // IPv4 + UDP header minimal
    const uint8_t  ihl   = (buf[0] & 0x0F) * 4;   // usually 20
    const uint16_t dport = (buf[ihl+2] << 8) | buf[ihl+3];

    std::ostringstream out;
    size_t payloadStart = ihl + 8;
    size_t payloadLen   = buf.size() - payloadStart;
    size_t printLen     = std::min(payloadLen, static_cast<size_t>(50));

     out << textPrefix
        << " MC-LAN • printed size " << payloadLen << " B  | ";

    // payload starts after UDP header (ihl + 8)
    for (size_t i = 0; i < printLen; ++i) {
        uint8_t c = buf[payloadStart + i];
        out << (std::isprint(c) ? static_cast<char>(c) : '.');
    }

    if (payloadLen > printLen) {
        out << "...";  // indicate truncation
    }

    // NETWORK_TRAFFIC_LOG("[Network] Multicast packet: {}", out.str());
}
}

// Helper struct for IP header
struct IPPacket
{
    uint8_t version4__ihl4;
    uint8_t typeOfService;
    uint16_t totalLength;
    uint16_t identification;
    uint16_t flags2__fragmentOffset14;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t headerChecksum;
    uint32_t sourceIp;
    uint32_t destIp;
    // uint32_t Options
};

P2PSystem::P2PSystem() 
    : running(false)
    , publicPort(0)
    , peerPort(0)
    , isHost(false)
{
    stateManager = std::make_shared<SystemStateManager>();
}

P2PSystem::~P2PSystem()
{
    shutdown();
}

bool P2PSystem::initialize(const std::string& serverUrl, const std::string& selfUsername, int localPort)
{
    username = selfUsername;
    running = true;
    stateManager->setState(SystemState::IDLE);

    /*
    *   STUN PROCEDURE SETUP
    */

    // Discover public address for NAT traversal
    if (!discoverPublicAddress())
    {
        SYSTEM_LOG_ERROR("[System] Failed to do STUN and discover public address.");
        return false;
    }

    /*
    *   SIGNALING SERVER CONNECTION SETUP
    */

    // Set up callbacks for signaling
    signalingClient.setConnectCallback([this](bool connected)
    {
        if (connected) {
            this->signalingClient.sendGreeting();
        }
    });

    signalingClient.setChatRequestCallback([this](const std::string& from)
    {
        this->handleConnectionRequest(from);
    });
    
    signalingClient.setPeerInfoCallback([this](const std::string& username, const std::string& ip, int port)
    {
        this->handlePeerInfo(username, ip, port);
    });
    
    signalingClient.setChatInitCallback([this](const std::string& username, const std::string& ip, int port)
    {
        this->handleConnectionInit(username, ip, port);
    });

    // Connect to signaling server
    if (!signalingClient.connect(serverUrl)) {
        SYSTEM_LOG_ERROR("[System] Failed to connect to signaling server");
        return false;
    }

    // Register with the signaling server
    signalingClient.registerUser(username, publicIp, publicPort);

    /*
    *   TUN INTERFACE SETUP
    */

    // Initialize TUN interface
    tunInterface = std::make_unique<TunInterface>();
    if (!tunInterface->initialize("PeerBridge"))
    {
        SYSTEM_LOG_ERROR("[System] Failed to initialize TUN interface");
        return false;
    }
    
    // Register packet callback from TUN interface
    tunInterface->setPacketCallback([this](const std::vector<uint8_t>& packet) {
        this->handlePacketFromTun(packet);
    });

    networkConfigManager.setNarrowAlias(tunInterface->getNarrowAlias());

    /*
    *   UDP NETWORK SERVICE SETUP
    */

    // Create networking class, using the socket from STUN to preserve NAT binding
    networkModule = std::make_unique<UDPNetwork>(
        std::move(stunService.getSocket()),
        stunService.getContext(),
        stateManager);
    
    // Set up network callbacks for P2P connection
    networkModule->setMessageCallback([this](std::vector<uint8_t> packet)
    {
        // Convert message to binary data
        this->handleNetworkData(std::move(packet));
    });
    
    // Start UDP network
    if (!networkModule->startListening(localPort))
    {
        SYSTEM_LOG_ERROR("[System] Failed to start UDP network");
        return false;
    }
    
    // Start monitoring loop
    monitorThread = std::thread([this]()
    {
        while (running && !stateManager->isInState(SystemState::SHUTTING_DOWN))
        {
            this->monitorLoop();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });

    SYSTEM_LOG_INFO("[System] P2P System initialized successfully");
    
    return true;
}

void P2PSystem::monitorLoop()
{
    // Process all pending events
    while (auto event = stateManager->getNextEvent())
    {
        handleNetworkEvent(*event);
    }
    
    // Add a additional health checks if necessary
}

void P2PSystem::handleNetworkEvent(const NetworkEventData& event)
{
    SystemState currentState = stateManager->getState();
    
    switch (event.event)
    {
        case NetworkEvent::PEER_CONNECTED:
            if (currentState == SystemState::CONNECTING) 
            {
                if (!startNetworkInterface()) {
                    SYSTEM_LOG_ERROR("[System] Failed to start network interface");\
                    stopConnection();
                    break;
                }
                stateManager->setState(SystemState::CONNECTED);
                SYSTEM_LOG_INFO("[System] Peer connected successfully");
            }
            break;
            
        case NetworkEvent::ALL_PEERS_DISCONNECTED:
            if (currentState == SystemState::CONNECTED)
            {
                SYSTEM_LOG_WARNING("[System] All peers disconnected");
                stopConnection();
            }
            break;
            
        case NetworkEvent::SHUTDOWN_REQUESTED:
            SYSTEM_LOG_INFO("[System] Shutdown requested via event");
            shutdown();
            break;
    }
}

bool P2PSystem::discoverPublicAddress()
{
    auto publicAddr = stunService.discoverPublicAddress();
    if (!publicAddr)
    {
        SYSTEM_LOG_ERROR("[System] Failed to discover public address via STUN");
        return false;
    }
    
    publicIp = publicAddr->ip;
    publicPort = publicAddr->port;

    SYSTEM_LOG_INFO("[System] Public address: {}:{}", publicIp, std::to_string(publicPort));
    
    return true;
}

bool P2PSystem::startNetworkInterface()
{
    if (!isConnected() || stateManager->getState() != SystemState::CONNECTING)
    {
        SYSTEM_LOG_WARNING("[System] Cannot configure interface, not connected to a peer");
        return false;
    }
    
    // Start packet processing
    if (!tunInterface->startPacketProcessing()) {
        SYSTEM_LOG_ERROR("[System] Failed to start packet processing");
        return false;
    }
    
    SYSTEM_LOG_INFO("[System] Network interface started with IP {}", localVirtualIp);
    SYSTEM_LOG_INFO("[System] Peer has IP {}", peerVirtualIp);

    clog.setLoggingEnabled(false);
    
    return true;
}

// TODO: TO BE MODIFIED FOR *1, this can be removed as peer info parsing will be done elsewhere
void P2PSystem::assignIPAddresses()
{
    if (isHost) {
        localVirtualIp = HOST_IP;
        peerVirtualIp = CLIENT_IP;
    } else {
        localVirtualIp = CLIENT_IP;
        peerVirtualIp = HOST_IP;
    }
}

bool P2PSystem::isConnected() const
{
    return networkModule ? networkModule->isConnected() : false;
}

bool P2PSystem::isRunning() const
{
    return running;
}

void P2PSystem::setRunning()
{
    running = false;
}

// !! *1 SCHEDULED FOR REMOVAL WHEN INTEGRATING
bool P2PSystem::getIsHost() const
{
    return isHost;
}

bool P2PSystem::connectToPeer(const std::string& peerUsername_)
{
    if (isConnected())
    {
        SYSTEM_LOG_WARNING("[System] Attempted to connect to peer while already connected to a peer");
        return false;
    }
    
    peerUsername = peerUsername_;
    isHost = false;
    
    // Update system state
    stateManager->setState(SystemState::CONNECTING);
    
    // Request peer info from signaling server
    signalingClient.requestPeerInfo(peerUsername);
    
    // Request connection
    signalingClient.sendChatRequest(peerUsername);

    SYSTEM_LOG_INFO("[System] Sent connection request to {}", peerUsername);
    
    return true;
}

// !! SCHEDULED FOR REMOVAL WHEN INTEGRATING
void P2PSystem::acceptIncomingRequest()
{
    if (pendingRequestFrom.empty())
    {
        SYSTEM_LOG_INFO("[System] No pending connection request");
        return;
    }
    
    // We are the host
    isHost = true;
    
    signalingClient.acceptChatRequest();
    SYSTEM_LOG_INFO("[System] Accepted connection request from {}", pendingRequestFrom);
    
    peerUsername = pendingRequestFrom;
    pendingRequestFrom = "";
}

// !! SCHEDULED FOR REMOVAL WHEN INTEGRATING
void P2PSystem::rejectIncomingRequest()
{
    if (pendingRequestFrom.empty()) {
        SYSTEM_LOG_INFO("[System] No pending connection request");
        return;
    }
    
    signalingClient.declineChatRequest();
    SYSTEM_LOG_INFO("[System] Rejected connection request from {}", pendingRequestFrom);
    
    pendingRequestFrom = "";
}

void P2PSystem::handleConnectionRequest(const std::string& from)
{
    pendingRequestFrom = from;
}

void P2PSystem::handlePeerInfo(const std::string& username, const std::string& ip, int port)
{
    if (username != peerUsername)
        return;

    peerIp = ip;
    peerPort = port;

    SYSTEM_LOG_INFO("[System] Got peer info: {} at {}:{}", username, ip, std::to_string(port));

}

void P2PSystem::handleConnectionInit(const std::string& username, const std::string& ip, int port)
{
    peerUsername = username;
    peerIp = ip;
    peerPort = port;

    SYSTEM_LOG_INFO("[System] Connection initialized with {}, connecting...", username);

    // Set state to CONNECTING (actual connection will be confirmed via events)
    stateManager->setState(SystemState::CONNECTING);

    // Assign IP addresses based on host/client role
    assignIPAddresses();
    uint8_t selfIndex = isHost ? 1 : 2;
    
    NetworkConfigManager::ConnectionConfig cfg{selfIndex, peerVirtualIp};
    // Set up virtual interface
    if (!networkConfigManager.configureInterface(cfg))
    {
        SYSTEM_LOG_ERROR("[System] Failed to set up virtual interface");
        return;
    }
    
    // Start UDP hole punching process
    if (!networkModule->connectToPeer(ip, port))
    {
        SYSTEM_LOG_ERROR("[System] Failed to initiate UDP hole punching");
        stateManager->setState(SystemState::IDLE);
        return;
    }
}

/*
* Network flow
*/

void P2PSystem::handlePacketFromTun(const std::vector<uint8_t>& packet)
{
    // We received a packet from our TUN interface, forward to peer
    // Minimum IPv4 header size and version check
    if (packet.size() >= sizeof(IPPacket) && (packet[0] >> 4) == 4)
    {
        forwardPacketToPeer(packet);
    }
}

bool P2PSystem::forwardPacketToPeer(const std::vector<uint8_t>& packet)
{
    // Extract source and destination IPs for filtering
    uint32_t srcIp = (packet[12] << 24) | (packet[13] << 16) | (packet[14] << 8) | packet[15];
    uint32_t dstIp = (packet[16] << 24) | (packet[17] << 16) | (packet[18] << 8) | packet[19];

    std::string srcIpStr = utils::uint32ToIp(srcIp);
    std::string dstIpStr = utils::uint32ToIp(dstIp);

    // Forward packets that are meant for  peer OR are broadcast/multicast packets
    bool isForPeer = (dstIpStr == peerVirtualIp);
    bool isBroadcast = (dstIpStr == "10.0.0.255" || dstIpStr == "255.255.255.255");
    bool isMulticast = (dstIp >> 28) == 14; // 224.0.0.0/4 (first octet 224-239)

    if (!isForPeer && !isBroadcast && !isMulticast)
    {
        // Drop packet not meant for peer
        return false;
    }

    // if (isMulticast) dumpMulticastPacket(packet, "[TX] Sending");

    // Send the packet to the peer
    return networkModule->sendMessage(packet);
}

void P2PSystem::handleNetworkData(std::vector<uint8_t> data)
{
    // We received a packet from peer, forward to TUN
    // Minimum IPv4 header size and version check
    if (data.size() >= sizeof(IPPacket) && (data[0] >> 4) == 4) 
    {
        deliverPacketToTun(std::move(data));
    }
}

bool P2PSystem::deliverPacketToTun(std::vector<uint8_t> packet) {
    // Basic check for TUN interface availability
    if (!tunInterface || !tunInterface->isRunning())
    {
        return false;
    }

    // Extract source and destination IPs for filtering
    uint32_t srcIp = (packet[12] << 24) | (packet[13] << 16) | (packet[14] << 8) | packet[15];
    uint32_t dstIp = (packet[16] << 24) | (packet[17] << 16) | (packet[18] << 8) | packet[19];

    std::string srcIpStr = utils::uint32ToIp(srcIp);
    std::string dstIpStr = utils::uint32ToIp(dstIp);

    // Only deliver packets that are meant for us OR are broadcast/multicast packets
    bool isForUs = (dstIpStr == localVirtualIp);
    bool isBroadcast = (dstIpStr == "10.0.0.255" || dstIpStr == "255.255.255.255");
    bool isMulticast = (dstIp >> 28) == 14; // 224.0.0.0/4 (first octet 224-239)

    if (!isForUs && !isBroadcast && !isMulticast)
    {
        // Drop packet not meant for us
        return false;
    }

    // if (isMulticast) dumpMulticastPacket(packet, "[RX] Receiving");

    // Send the packet to the TUN interface
    return tunInterface->sendPacket(std::move(packet));
}

/*
* Disconnect and shutdown
*/

void P2PSystem::stopNetworkInterface()
{
    if (tunInterface && tunInterface->isRunning())
    {
        tunInterface->stopPacketProcessing();
        networkConfigManager.resetInterfaceConfiguration(peerVirtualIp);
        SYSTEM_LOG_INFO("[System] Network interface stopped and configuration reset");
    }
}

// Stop current connection but keep system running
void P2PSystem::stopConnection()
{
    // Stop the network connection
    if (networkModule)
        networkModule->stopConnection();
    
    // Stop the network interface
    stopNetworkInterface();
    
    // Reset peer info
    peerUsername = "";
    peerIp = "";
    peerPort = 0;
    
    // Update system state
    stateManager->setState(SystemState::IDLE);
    
    SYSTEM_LOG_INFO("[System] Connection stopped, system ready for new connections");
}

// Full system shutdown
void P2PSystem::shutdown()
{
    // First stop any active connections
    if (isConnected())
    {
        if (networkModule)
            networkModule->stopConnection();
    
        // Stop the network interface
        stopNetworkInterface();
    }
    // Update system state
    running = false;
    stateManager->setState(SystemState::SHUTTING_DOWN);
    
    // Stop the network interface
    stopNetworkInterface();
    tunInterface->close();
    
    // Stop the network
    if (networkModule)
    {
        networkModule->shutdown();
    }
    
    // Close the TUN interface
    if (tunInterface)
    {
        tunInterface->close();
    }
    
    // Clean up signaling connection
    signalingClient.disconnect();

    if (monitorThread.joinable())
    {
        monitorThread.join();
    }
    
    SYSTEM_LOG_INFO("[System] System shut down successfully");
}