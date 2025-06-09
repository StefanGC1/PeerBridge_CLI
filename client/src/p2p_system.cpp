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
}

P2PSystem::~P2PSystem() {
    disconnect();
}

bool P2PSystem::initialize(const std::string& server_url, const std::string& username, int local_port) {
    username_ = username;
    running_ = true;

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

    /*
    *   UDP NETWORK SERVICE SETUP
    */

    // Create networking class, using the socket from STUN to preserve NAT binding
    network_ = std::make_unique<UDPNetwork>(
        std::move(stun_.getSocket()),
        stun_.getContext());
    
    // Set up network callbacks for P2P connection
    network_->setMessageCallback([this](const std::string& message) {
        // Convert message to binary data
        std::vector<uint8_t> data(message.begin(), message.end());
        this->handleNetworkData(data);
    });
    
    network_->setConnectionCallback([this](bool connected) {
        this->handleConnectionChange(connected);
    });
    
    // Start UDP network
    if (!network_->startListening(local_port)) {
        SYSTEM_LOG_ERROR("[System] Failed to start UDP network");
        return false;
    }

    SYSTEM_LOG_INFO("[System] P2P System initialized successfully");
    
    return true;
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

// !! SCHEDULED FOR REMOVAL WHEN INTEGRATING
bool P2PSystem::connectToPeer(const std::string& peer_username) {
    if (network_->isConnected()) {
        SYSTEM_LOG_WARNING("[System] Attempted to connect to peer while already connected to a peer");
        return false;
    }
    
    peer_username_ = peer_username;
    is_host_ = false;
    
    // Request peer info from signaling server
    signaling_.requestPeerInfo(peer_username);
    
    // Request connection
    signaling_.sendChatRequest(peer_username);

    SYSTEM_LOG_INFO("[System] Sent connection request to {}", peer_username);
    
    return true;
}

void P2PSystem::disconnect() {
    running_ = false;
    
    // Stop packet handling
    if (packet_handling_thread_.joinable()) {
        packet_handling_thread_.join();
    }
    
    // Stop the network interface
    stopNetworkInterface();
    
    // Disconnect from peer
    network_->disconnect();
    signaling_.disconnect();
    
    peer_username_ = "";
    pending_request_from_ = "";

    SYSTEM_LOG_INFO("[System] Disconnected");
}

bool P2PSystem::isConnected() const {
    return network_->isConnected();
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
    if (!network_->isConnected()) {
        SYSTEM_LOG_WARNING("[System] Cannot configure interface, not connected to a peer");
        return false;
    }
    
    // Assign IP addresses based on host/client role
    assignIPAddresses();
    
    // Set up virtual interface
    if (!setupVirtualInterface()) {
        SYSTEM_LOG_ERROR("[System] Failed to set up virtual interface");
        return false;
    }
    
    // Start packet processing
    if (!tun_->startPacketProcessing()) {
        SYSTEM_LOG_ERROR("[System] Failed to start packet processing");
        return false;
    }
    
    SYSTEM_LOG_INFO("[System] Network interface started with IP ", local_virtual_ip_);
    SYSTEM_LOG_INFO("[System] Peer has IP ", peer_virtual_ip_);

    clog.setLoggingEnabled(false);
    
    return true;
}

void P2PSystem::stopNetworkInterface() {
    if (tun_ && tun_->isRunning()) {
        tun_->stopPacketProcessing();
        tun_->close();
        
        SYSTEM_LOG_INFO("[System] Network interface stopped");
    }
}

void P2PSystem::assignIPAddresses() {
    if (is_host_) {
        local_virtual_ip_ = HOST_IP;
        peer_virtual_ip_ = CLIENT_IP;
    } else {
        local_virtual_ip_ = CLIENT_IP;
        peer_virtual_ip_ = HOST_IP;
    }
}

bool P2PSystem::setupVirtualInterface() {
    // Configure the interface with IP and netmask
    if (!tun_->configureInterface(local_virtual_ip_, VIRTUAL_NETMASK)) {
        SYSTEM_LOG_ERROR("[System] Failed to configure interface with IP {}", local_virtual_ip_);
        return false;
    }
    
    // Set up routing
    if (!tun_->setupRouting()) {
        SYSTEM_LOG_ERROR("[System] Failed to set up routing");
        return false;
    }
    
    // Add firewall rule to allow traffic on the virtual network
    std::ostringstream firewallCmd;
    firewallCmd << "netsh advfirewall firewall add rule "
                << "name=\"P2P Network\" "
                << "dir=in "
                << "action=allow "
                << "protocol=any "
                << "remoteip=10.0.0.0/24";
    
    // Execute firewall command (using our TUN interface's command helper)
    if (!tun_->executeNetshCommand(firewallCmd.str())) {
        SYSTEM_LOG_WARNING("[System] Failed to add inbound firewall rule. Connectivity may be limited.");
    }
    
    // Add outbound rule too
    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall add rule "
                << "name=\"P2P Network (out)\" "
                << "dir=out "
                << "action=allow "
                << "protocol=any "
                << "remoteip=10.0.0.0/24";
    
    if (!tun_->executeNetshCommand(firewallCmd.str())) {
        SYSTEM_LOG_WARNING("[System] Failed to add outbound firewall rule. Connectivity may be limited.");
    }
    
    // Add specific rule for ICMP (ping)
    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall add rule "
                << "name=\"P2P Network ICMP\" "
                << "dir=in "
                << "action=allow "
                << "protocol=icmpv4 "
                << "remoteip=10.0.0.0/24";
    
    if (!tun_->executeNetshCommand(firewallCmd.str())) {
        SYSTEM_LOG_WARNING("[System] Failed to add ICMP firewall rule. Ping may not work.");
    }
    
    // Enable File and Printer Sharing (needed for some network discovery protocols)
    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall set rule "
                << "group=\"File and Printer Sharing\" "
                << "new enable=Yes";
    
    if (!tun_->executeNetshCommand(firewallCmd.str())) {
        SYSTEM_LOG_WARNING("[System] Failed to enable File and Printer Sharing. Network discovery may be limited.");
    }

    // Add specific rule for IGMP (inbound)
    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall add rule "
                << "name=\"P2P Network IGMP in\" "
                << "dir=in "
                << "action=allow "
                << "protocol=2 "
                << "remoteip=10.0.0.0/24";
    if (!tun_->executeNetshCommand(firewallCmd.str())) {
        SYSTEM_LOG_WARNING("[System] Failed to add outbound IGMP firewall rule. Multicast may not work.");
    }

    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall add rule "
                << "name=\"P2P Network IGMP out\" "
                << "dir=out "
                << "action=allow "
                << "protocol=2 "
                << "remoteip=10.0.0.0/24";
    if (!tun_->executeNetshCommand(firewallCmd.str())) {
        SYSTEM_LOG_WARNING("[System] Failed to add outbound IGMP firewall rule. Multicast may not work.");
    }

    std::ostringstream netCategoryCmd;
    netCategoryCmd << "powershell -Command \"Set-NetConnectionProfile -InterfaceAlias 'PeerBridge' -NetworkCategory Private\"";

    if (!tun_->executeNetshCommand(netCategoryCmd.str())) {
        SYSTEM_LOG_WARNING("[System] Failed to set network category to Private, LAN functionality may be limited");
    }

    return true;
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
}

void P2PSystem::handlePacketFromTun(const std::vector<uint8_t>& packet) {
    // We received a packet from our TUN interface, forward to peer
    // Minimum IPv4 header size and version check
    if (packet.size() >= sizeof(IPPacket) && network_->isConnected() && (packet[0] >> 4) == 4) {
        forwardPacketToPeer(packet);
    }
}

bool P2PSystem::forwardPacketToPeer(const std::vector<uint8_t>& packet) {
    // Check if the packet is an IPv4 packet
    // UNCOMMENT IN CASE THIS BREAKS
    // if (packet.empty() || (packet[0] >> 4) != 4) {
    //     return false;  // Not an IPv4 packet
    // }

    // Extract source and destination IPs for filtering
    uint32_t srcIp = (packet[12] << 24) | (packet[13] << 16) | (packet[14] << 8) | packet[15];
    uint32_t dstIp = (packet[16] << 24) | (packet[17] << 16) | (packet[18] << 8) | packet[19];

    std::string srcIpStr = uint32ToIp(srcIp);
    std::string dstIpStr = uint32ToIp(dstIp);

    // Forward packets that are meant for the peer OR are broadcast/multicast packets
    bool isForPeer = (dstIpStr == peer_virtual_ip_);
    bool isBroadcast = (dstIpStr == "10.0.0.255" || dstIpStr == "255.255.255.255");
    bool isMulticast = (dstIp >> 28) == 14; // 224.0.0.0/4 (first octet 224-239)

    if (!isForPeer && !isBroadcast && !isMulticast) {
        // Drop packet not meant for peer
        return false;
    }

    if (isMulticast) dumpMinecraftLan(packet, "[TX] Sending");
    
    // Convert packet to string for UDP transmission
    std::string packet_str(packet.begin(), packet.end());
    
    // Send the packet to the peer
    // TODO: Improve logging
    // NETWORK_TRAFFIC_LOG("[Network] Extracted packet from TUN, sending to peer: {} -> {} (Size: {})",
    //                 srcIpStr, dstIpStr, packet.size());
    return network_->sendMessage(packet_str);
}

void P2PSystem::handleNetworkData(const std::vector<uint8_t>& data) {
    // We received a packet from peer, forward to TUN
    // Minimum IPv4 header size and version check
    if (data.size() >= sizeof(IPPacket) && (data[0] >> 4) == 4)
    {
        deliverPacketToTun(data);
    }
}

bool P2PSystem::deliverPacketToTun(const std::vector<uint8_t>& packet) {
    // Check if the packet is an IPv4 packet
    // UNCOMMENT IN CASE THIS BREAKS
    // if (packet.empty() || (packet[0] >> 4) != 4) {
    //     return false;  // Not an IPv4 packet
    // }
    
    // Extract source and destination IPs for filtering
    uint32_t srcIp = (packet[12] << 24) | (packet[13] << 16) | (packet[14] << 8) | packet[15];
    uint32_t dstIp = (packet[16] << 24) | (packet[17] << 16) | (packet[18] << 8) | packet[19];
    
    std::string srcIpStr = uint32ToIp(srcIp);
    std::string dstIpStr = uint32ToIp(dstIp);
    
    // Only deliver packets that are meant for us, are broadcast packets, or multicast packets
    bool isForUs = (dstIpStr == local_virtual_ip_);
    bool isBroadcast = (dstIpStr == "10.0.0.255" || dstIpStr == "255.255.255.255");
    bool isMulticast = (dstIp >> 28) == 14; // 224.0.0.0/4 (first octet 224-239)
    
    if (!isForUs && !isBroadcast && !isMulticast) {
        // Drop packet not meant for us
        return false;
    }

    if (isMulticast) dumpMinecraftLan(packet, "[TX] Receiving");
    
    // Send the packet to the TUN interface
    // TODO: Improve logging
    // NETWORK_TRAFFIC_LOG("[Network]  Received packet from peer, delivering to TUN: {} <- {} (Size: {})",
    //                 dstIpStr, srcIpStr, packet.size());
    return tun_->sendPacket(packet);
}

std::string P2PSystem::uint32ToIp(uint32_t ipAddress) {
    std::ostringstream result;
    
    for (int i = 0; i < 4; i++) {
        uint8_t octet = (ipAddress >> (8 * (3 - i))) & 0xFF;
        result << static_cast<int>(octet);
        if (i < 3) {
            result << ".";
        }
    }
    
    return result.str();
}

void P2PSystem::handleConnectionChange(bool connected) {
    if (!connected)
    {
        SYSTEM_LOG_INFO("[System] Disconnected from {}", peer_username_);

        // Stop the network interface
        stopNetworkInterface();
        
        peer_username_ = "";
        return;
    }

    SYSTEM_LOG_INFO("[System] Network connection inititated with {}", peer_username_);

    // Start the virtual network interface now that we're connected
    if (!startNetworkInterface()) {
        SYSTEM_LOG_WARNING("[System] Failed to start network interface, connection will be limited");
        return;
    }
    SYSTEM_LOG_INFO("[System] Virtual network is now active. You can use standard networking tools (ping, etc.)");
}