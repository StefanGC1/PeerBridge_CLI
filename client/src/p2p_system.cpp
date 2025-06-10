#include "utils.hpp"
#include "p2p_system.hpp"
#include "logger.hpp"
#include <iostream>
#include <vector>
#include <sstream>

namespace {
// REMOVE LATER
inline void dumpMinecraftLan(const std::vector<uint8_t>& buf,
                             const std::string& textPrefix)
{
    if (buf.size() < 34) return;                  // IPv4 + UDP header minimal
    const uint8_t  ihl   = (buf[0] & 0x0F) * 4;   // usually 20
    const uint16_t dport = (buf[ihl+2] << 8) | buf[ihl+3];

    // Filter: UDP 4445 or 4446 AND dest 224.0.2.60
    // if (dport != 4445 && dport != 4446) return;
    // if (buf[16] != 224 || buf[17] != 0 || buf[18] != 2 || buf[19] != 60) return;

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

// Helper structure for IP packet parsing
struct IPPacket {
    uint8_t version_ihl;
    uint8_t dscp_ecn;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment_offset;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t header_checksum;
    uint32_t source_ip;
    uint32_t dest_ip;
    // Options and data follow
};

P2PSystem::P2PSystem() 
    : running_(false), public_port_(0), peer_port_(0), is_host_(false)
{
    // Initialize state managers
    state_manager_ = std::make_shared<SystemStateManager>();
    peer_connection_ = std::make_shared<PeerConnectionInfo>();
}

P2PSystem::~P2PSystem() {
    shutdown();
}

bool P2PSystem::initialize(const std::string& server_url, const std::string& username, int local_port) {
    username_ = username;
    running_ = true;
    state_manager_->setState(SystemState::IDLE);

    /*
    *   STUN PROCEDURE SETUP
    */

    // Discover public address for NAT traversal
    if (!discoverPublicAddress()) {
        SYSTEM_LOG_ERROR("[System] Failed to do STUN and discover public address.");
        return false;
    }

    /*
    *   SIGNALING SERVER CONNECTION SETUP
    */

    // Set up callbacks for signaling
    signaling_.setConnectCallback([this](bool connected) {
        if (connected) {
            this->signaling_.sendGreeting();
        }
    });

    signaling_.setChatRequestCallback([this](const std::string& from) {
        this->handleConnectionRequest(from);
    });
    
    signaling_.setPeerInfoCallback([this](const std::string& username, const std::string& ip, int port) {
        this->handlePeerInfo(username, ip, port);
    });
    
    signaling_.setChatInitCallback([this](const std::string& username, const std::string& ip, int port) {
        this->handleConnectionInit(username, ip, port);
    });

    // Connect to signaling server
    if (!signaling_.connect(server_url)) {
        SYSTEM_LOG_ERROR("[System] Failed to connect to signaling server");
        return false;
    }

    // Register with the signaling server
    signaling_.registerUser(username_, public_ip_, public_port_);

    /*
    *   TUN INTERFACE SETUP
    */

    // Initialize TUN interface
    tun_ = std::make_unique<TunInterface>();
    if (!tun_->initialize("PeerBridge")) {
        SYSTEM_LOG_ERROR("[System] Failed to initialize TUN interface");
        return false;
    }
    
    // Register packet callback from TUN interface
    tun_->setPacketCallback([this](const std::vector<uint8_t>& packet) {
        this->handlePacketFromTun(packet);
    });

    networkConfigManager.setNarrowAlias(tun_->getNarrowAlias());

    /*
    *   UDP NETWORK SERVICE SETUP
    */

    // Create networking class, using the socket from STUN to preserve NAT binding
    network_ = std::make_unique<UDPNetwork>(
        std::move(stun_.getSocket()),
        stun_.getContext(),
        state_manager_,
        peer_connection_);
    
    // Set up network callbacks for P2P connection
    network_->setMessageCallback([this](std::vector<uint8_t> packet) {
        // Convert message to binary data
        this->handleNetworkData(std::move(packet));
    });
    
    // Start UDP network
    if (!network_->startListening(local_port)) {
        SYSTEM_LOG_ERROR("[System] Failed to start UDP network");
        return false;
    }
    
    // Start monitoring loop
    monitor_thread_ = std::thread([this]() {
        while (running_ && !state_manager_->isInState(SystemState::SHUTTING_DOWN)) {
            this->monitorLoop();
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    });
    monitor_thread_.detach();

    SYSTEM_LOG_INFO("[System] P2P System initialized successfully");
    
    return true;
}

void P2PSystem::monitorLoop() {
    // Check connection state
    if (state_manager_->isInState(SystemState::CONNECTED) && !peer_connection_->isConnected()) {
        // Connection lost, update system state
        SYSTEM_LOG_WARNING("[System] Connection lost detected by monitor loop");
        stopConnection();
    }
    
    // Removed problematic TUN interface check that was causing unnecessary network reconfigurations
}

bool P2PSystem::discoverPublicAddress() {
    auto public_addr = stun_.discoverPublicAddress();
    if (!public_addr) {
        return false;
    }
    
    public_ip_ = public_addr->ip;
    public_port_ = public_addr->port;

    SYSTEM_LOG_INFO("[System] Public address: {}:{}", public_ip_, std::to_string(public_port_));
    
    return true;
}

bool P2PSystem::connectToPeer(const std::string& peer_username) {
    if (peer_connection_->isConnected()) {
        SYSTEM_LOG_WARNING("[System] Attempted to connect to peer while already connected to a peer");
        return false;
    }
    
    peer_username_ = peer_username;
    is_host_ = false;
    
    // Update system state
    state_manager_->setState(SystemState::CONNECTING);
    
    // Request peer info from signaling server
    signaling_.requestPeerInfo(peer_username);
    
    // Request connection
    signaling_.sendChatRequest(peer_username);

    SYSTEM_LOG_INFO("[System] Sent connection request to {}", peer_username);
    
    return true;
}

// Stop current connection but keep system running
void P2PSystem::stopConnection() {
    // Stop the network connection
    if (network_) {
        network_->stopConnection();
    }
    
    // Stop the network interface
    stopNetworkInterface();
    
    // Reset peer info
    peer_username_ = "";
    peer_ip_ = "";
    peer_port_ = 0;
    
    // Update system state
    state_manager_->setState(SystemState::IDLE);
    
    SYSTEM_LOG_INFO("[System] Connection stopped, system ready for new connections");
}

// Full system shutdown
void P2PSystem::shutdown() {
    // First stop any active connections
    if (peer_connection_->isConnected()) {
        stopConnection();
    }
    
    // Update system state
    state_manager_->setState(SystemState::SHUTTING_DOWN);
    running_ = false;
    
    // Stop the network interface
    stopNetworkInterface();
    tun_->close();
    
    // Stop the network
    if (network_) {
        network_->shutdown();
    }
    
    // Stop packet handling
    if (packet_handling_thread_.joinable()) {
        packet_handling_thread_.join();
    }
    
    // Close the TUN interface
    if (tun_) {
        tun_->close();
    }
    
    // Clean up signaling connection
    signaling_.disconnect();
    
    SYSTEM_LOG_INFO("[System] System shut down successfully");
}

bool P2PSystem::isConnected() const {
    return peer_connection_->isConnected();
}

bool P2PSystem::isRunning() const {
    return running_;
}

void P2PSystem::setRunning() {
    running_ = false;
}

// !! SCHEDULED FOR REMOVAL WHEN INTEGRATING
bool P2PSystem::isHost() const {
    return is_host_;
}

// !! SCHEDULED FOR REMOVAL WHEN INTEGRATING
void P2PSystem::acceptIncomingRequest() {
    if (pending_request_from_.empty()) {
        SYSTEM_LOG_INFO("[System] No pending connection request");
        return;
    }
    
    // We are the host
    is_host_ = true;
    
    signaling_.acceptChatRequest();
    SYSTEM_LOG_INFO("[System] Accepted connection request from {}", pending_request_from_);
    
    peer_username_ = pending_request_from_;
    pending_request_from_ = "";
}

// !! SCHEDULED FOR REMOVAL WHEN INTEGRATING
void P2PSystem::rejectIncomingRequest() {
    if (pending_request_from_.empty()) {
        SYSTEM_LOG_INFO("[System] No pending connection request");
        return;
    }
    
    signaling_.declineChatRequest();
    SYSTEM_LOG_INFO("[System] Rejected connection request from {}", pending_request_from_);
    
    pending_request_from_ = "";
}

bool P2PSystem::startNetworkInterface() {
    if (!peer_connection_->isConnected()) {
        SYSTEM_LOG_WARNING("[System] Cannot configure interface, not connected to a peer");
        return false;
    }
    
    // Assign IP addresses based on host/client role
    assignIPAddresses();
    uint8_t selfIndex = is_host_ ? 1 : 2;
    
    NetworkConfigManager::ConnectionConfig cfg{
        selfIndex,
        peer_virtual_ip_
    };
    // Set up virtual interface
    if (!networkConfigManager.configureInterface(cfg)) {
        SYSTEM_LOG_ERROR("[System] Failed to set up virtual interface");
        return false;
    }
    
    // Start packet processing
    if (!tun_->startPacketProcessing()) {
        SYSTEM_LOG_ERROR("[System] Failed to start packet processing");
        return false;
    }
    
    SYSTEM_LOG_INFO("[System] Network interface started with IP {}", local_virtual_ip_);
    SYSTEM_LOG_INFO("[System] Peer has IP {}", peer_virtual_ip_);

    clog.setLoggingEnabled(false);
    
    return true;
}

void P2PSystem::stopNetworkInterface()
{
    if (tun_ && tun_->isRunning())
    {
        tun_->stopPacketProcessing();
        networkConfigManager.resetInterfaceConfiguration(peer_virtual_ip_);
        SYSTEM_LOG_INFO("[System] Network interface stopped and configuration reset");
    }
}

// TODO: TO BE MODIFIED FOR *1, this can be removed as peer info parsing will be done elsewhere
void P2PSystem::assignIPAddresses() {
    if (is_host_) {
        local_virtual_ip_ = HOST_IP;
        peer_virtual_ip_ = CLIENT_IP;
    } else {
        local_virtual_ip_ = CLIENT_IP;
        peer_virtual_ip_ = HOST_IP;
    }
}

void P2PSystem::handleConnectionRequest(const std::string& from) {
    pending_request_from_ = from;
}

void P2PSystem::handlePeerInfo(const std::string& username, const std::string& ip, int port) {
    if (username != peer_username_) {
        return;
    }

    peer_ip_ = ip;
    peer_port_ = port;

    SYSTEM_LOG_INFO("[System] Got peer info: {} at {}:{}", username, ip, std::to_string(port));

}

void P2PSystem::handleConnectionInit(const std::string& username, const std::string& ip, int port) {
    peer_username_ = username;
    peer_ip_ = ip;
    peer_port_ = port;

    SYSTEM_LOG_INFO("[System] Connection initialized with {}, connecting...", username);

    // Start UDP hole punching process
    if (!network_->connectToPeer(ip, port)) {
        SYSTEM_LOG_ERROR("[System] Failed to initiate UDP hole punching, will retry...");
    }
    
    // Set state to CONNECTED when the connection is established
    state_manager_->setState(SystemState::CONNECTED);
    
    // Start the virtual network interface now that we're connected
    if (!startNetworkInterface()) {
        SYSTEM_LOG_WARNING("[System] Failed to start network interface, connection will be limited");
        return;
    }
    
    SYSTEM_LOG_INFO("[System] Virtual network is now active with {}. You can use standard networking tools (ping, etc.)", username);
}

void P2PSystem::handlePacketFromTun(const std::vector<uint8_t>& packet) {
    // We received a packet from our TUN interface, forward to peer
    // Minimum IPv4 header size and version check
    if (packet.size() >= sizeof(IPPacket) && (packet[0] >> 4) == 4) {
        forwardPacketToPeer(packet);
    }
}

bool P2PSystem::forwardPacketToPeer(const std::vector<uint8_t>& packet) {
    // Extract source and destination IPs for filtering
    uint32_t srcIp = (packet[12] << 24) | (packet[13] << 16) | (packet[14] << 8) | packet[15];
    uint32_t dstIp = (packet[16] << 24) | (packet[17] << 16) | (packet[18] << 8) | packet[19];

    std::string srcIpStr = utils::uint32ToIp(srcIp);
    std::string dstIpStr = utils::uint32ToIp(dstIp);

    // Forward packets that are meant for the peer OR are broadcast/multicast packets
    bool isForPeer = (dstIpStr == peer_virtual_ip_);
    bool isBroadcast = (dstIpStr == "10.0.0.255" || dstIpStr == "255.255.255.255");
    bool isMulticast = (dstIp >> 28) == 14; // 224.0.0.0/4 (first octet 224-239)

    if (!isForPeer && !isBroadcast && !isMulticast) {
        // Drop packet not meant for peer
        return false;
    }

    if (isMulticast) dumpMinecraftLan(packet, "[TX] Sending");
    
    // Send the packet to the peer
    return network_->sendMessage(packet);
}

void P2PSystem::handleNetworkData(std::vector<uint8_t> data) {
    // We received a packet from peer, forward to TUN
    // Minimum IPv4 header size and version check
    if (data.size() >= sizeof(IPPacket) && (data[0] >> 4) == 4) {
        deliverPacketToTun(std::move(data));
    }
}

bool P2PSystem::deliverPacketToTun(std::vector<uint8_t> packet) {
    // Basic check for TUN interface availability
    if (!tun_ || !tun_->isRunning()) {
        return false;
    }
    
    // Extract source and destination IPs for filtering
    uint32_t srcIp = (packet[12] << 24) | (packet[13] << 16) | (packet[14] << 8) | packet[15];
    uint32_t dstIp = (packet[16] << 24) | (packet[17] << 16) | (packet[18] << 8) | packet[19];
    
    std::string srcIpStr = utils::uint32ToIp(srcIp);
    std::string dstIpStr = utils::uint32ToIp(dstIp);
    
    // Only deliver packets that are meant for us, are broadcast packets, or multicast packets
    bool isForUs = (dstIpStr == local_virtual_ip_);
    bool isBroadcast = (dstIpStr == "10.0.0.255" || dstIpStr == "255.255.255.255");
    bool isMulticast = (dstIp >> 28) == 14; // 224.0.0.0/4 (first octet 224-239)
    
    if (!isForUs && !isBroadcast && !isMulticast) {
        // Drop packet not meant for us
        return false;
    }

    if (isMulticast) dumpMinecraftLan(packet, "[RX] Receiving");
    
    // Send the packet to the TUN interface
    return tun_->sendPacket(std::move(packet));
}