#include "p2p_system.hpp"
#include <iostream>
#include <vector>
#include <sstream>

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

    // Discover public address for NAT traversal
    if (!discoverPublicAddress()) {
        if (on_status_) {
            on_status_("Failed to discover public address, NAT traversal may not work");
        }
        return false;
    }

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
    
    // Initialize TUN interface
    tun_ = std::make_unique<TunInterface>();
    if (!tun_->initialize("P2PBridge")) {
        if (on_status_) {
            on_status_("Failed to initialize TUN interface");
        }
        return false;
    }
    
    // Register packet callback from TUN interface
    tun_->setPacketCallback([this](const std::vector<uint8_t>& packet) {
        this->handlePacketFromTun(packet);
    });
    
    // Connect to signaling server
    if (!signaling_.connect(server_url)) {
        if (on_status_) {
            on_status_("Failed to connect to signaling server");
        }
        return false;
    }
    
    // Start UDP network
    if (!network_->startListening(local_port)) {
        if (on_status_) {
            on_status_("Failed to start UDP network");
        }
        return false;
    }

    // Register with the signaling server
    signaling_.registerUser(username_, public_ip_, public_port_);

    if (on_status_) {
        on_status_("P2P System initialized successfully");
    }
    
    return true;
}

bool P2PSystem::discoverPublicAddress() {
    auto public_addr = stun_.discoverPublicAddress();
    if (!public_addr) {
        return false;
    }
    
    public_ip_ = public_addr->ip;
    public_port_ = public_addr->port;
    
    if (on_status_) {
        on_status_("Public address: " + public_ip_ + ":" + std::to_string(public_port_));
    }
    
    return true;
}

bool P2PSystem::connectToPeer(const std::string& peer_username) {
    if (network_->isConnected()) {
        if (on_status_) {
            on_status_("Already connected to a peer");
        }
        return false;
    }
    
    peer_username_ = peer_username;
    is_host_ = false;
    
    // Request peer info from signaling server
    signaling_.requestPeerInfo(peer_username);
    
    // Request connection
    signaling_.sendChatRequest(peer_username);
    
    if (on_status_) {
        on_status_("Sent connection request to " + peer_username);
    }
    
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
    
    if (on_status_) {
        on_status_("Disconnected");
    }
}

bool P2PSystem::isConnected() const {
    return network_->isConnected();
}

bool P2PSystem::isRunning() const {
    return running_;
}

bool P2PSystem::isHost() const {
    return is_host_;
}

void P2PSystem::acceptIncomingRequest() {
    if (pending_request_from_.empty()) {
        if (on_status_) {
            on_status_("No pending connection request");
        }
        return;
    }
    
    // We are the host
    is_host_ = true;
    
    signaling_.acceptChatRequest();
    if (on_status_) {
        on_status_("Accepted connection request from " + pending_request_from_);
    }
    
    peer_username_ = pending_request_from_;
    pending_request_from_ = "";
}

void P2PSystem::rejectIncomingRequest() {
    if (pending_request_from_.empty()) {
        if (on_status_) {
            on_status_("No pending connection request");
        }
        return;
    }
    
    signaling_.declineChatRequest();
    if (on_status_) {
        on_status_("Rejected connection request from " + pending_request_from_);
    }
    
    pending_request_from_ = "";
}

bool P2PSystem::startNetworkInterface() {
    if (!network_->isConnected()) {
        if (on_status_) {
            on_status_("Not connected to a peer");
        }
        return false;
    }
    
    // Assign IP addresses based on host/client role
    assignIPAddresses();
    
    // Set up virtual interface
    if (!setupVirtualInterface()) {
        if (on_status_) {
            on_status_("Failed to set up virtual interface");
        }
        return false;
    }
    
    // Start packet processing
    if (!tun_->startPacketProcessing()) {
        if (on_status_) {
            on_status_("Failed to start packet processing");
        }
        return false;
    }
    
    if (on_status_) {
        on_status_("Network interface started with IP " + local_virtual_ip_);
        on_status_("Peer has IP " + peer_virtual_ip_);
    }
    
    return true;
}

void P2PSystem::stopNetworkInterface() {
    if (tun_ && tun_->isRunning()) {
        tun_->stopPacketProcessing();
        tun_->close();
        
        if (on_status_) {
            on_status_("Network interface stopped");
        }
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
        if (on_status_) {
            on_status_("Failed to configure interface with IP " + local_virtual_ip_);
        }
        return false;
    }
    
    // Set up routing
    if (!tun_->setupRouting()) {
        if (on_status_) {
            on_status_("Failed to set up routing");
        }
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
    
    std::cout << "Adding firewall rule: " << firewallCmd.str() << std::endl;
    
    // Execute firewall command (using our TUN interface's command helper)
    if (!tun_->executeNetshCommand(firewallCmd.str())) {
        std::cout << "Warning: Failed to add inbound firewall rule. Connectivity may be limited." << std::endl;
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
        std::cout << "Warning: Failed to add outbound firewall rule. Connectivity may be limited." << std::endl;
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
        std::cout << "Warning: Failed to add ICMP firewall rule. Ping may not work." << std::endl;
    }
    
    // Enable File and Printer Sharing (needed for some network discovery protocols)
    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall set rule "
                << "group=\"File and Printer Sharing\" "
                << "new enable=Yes";
    
    if (!tun_->executeNetshCommand(firewallCmd.str())) {
        std::cout << "Warning: Failed to enable File and Printer Sharing. Network discovery may be limited." << std::endl;
    }

    std::ostringstream netCategoryCmd;
    netCategoryCmd << "powershell -Command \"Set-NetConnectionProfile -InterfaceAlias 'P2PBridge' -NetworkCategory Private\"";

    if (!tun_->executeNetshCommand(netCategoryCmd.str())) {
        std::cout << "Warning: Failed to set network category to Private" << std::endl;
    }
    
    return true;
}

void P2PSystem::handleConnectionRequest(const std::string& from) {
    pending_request_from_ = from;
    
    if (on_connection_request_) {
        on_connection_request_(from);
    }
}

void P2PSystem::handlePeerInfo(const std::string& username, const std::string& ip, int port) {
    if (username != peer_username_) {
        return; // Not the peer we're looking for
    }
    
    peer_ip_ = ip;
    peer_port_ = port;
    
    if (on_status_) {
        on_status_("Got peer info: " + username + " at " + ip + ":" + std::to_string(port));
    }
}

void P2PSystem::handleConnectionInit(const std::string& username, const std::string& ip, int port) {
    peer_username_ = username;
    peer_ip_ = ip;
    peer_port_ = port;
    
    if (on_status_) {
        on_status_("Connection initialized with " + username + ", connecting...");
    }
    
    // Start UDP hole punching process
    if (!network_->connectToPeer(ip, port)) {
        if (on_status_) {
            on_status_("Failed to initiate UDP hole punching, will retry...");
        }
    }
}

void P2PSystem::handleNetworkData(const std::vector<uint8_t>& data) {
    // Debug: Log packet received from peer
    if (data.size() >= 20) { // Minimum IPv4 header size
        uint8_t protocol = data[9];
        uint32_t srcIp = (data[12] << 24) | (data[13] << 16) | (data[14] << 8) | data[15];
        uint32_t dstIp = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
        
        std::cout << "NET Recv: " << uint32ToIp(srcIp) << " -> " << uint32ToIp(dstIp) 
                  << " (Proto: " << static_cast<int>(protocol) << ", Size: " << data.size() << ")" << std::endl;
    }
    
    // Process received data from the peer and deliver to TUN interface
    deliverPacketToTun(data);
}

void P2PSystem::handlePacketFromTun(const std::vector<uint8_t>& packet) {
    // We received a packet from our TUN interface, forward to peer
    if (packet.size() >= sizeof(IPPacket) && network_->isConnected()) {
        // Debug: Log packet from TUN interface
        if (packet.size() >= 20) { // Minimum IPv4 header size
            uint8_t protocol = packet[9];
            uint32_t srcIp = (packet[12] << 24) | (packet[13] << 16) | (packet[14] << 8) | packet[15];
            uint32_t dstIp = (packet[16] << 24) | (packet[17] << 16) | (packet[18] << 8) | packet[19];
            
            std::cout << "P2P Forward: " << uint32ToIp(srcIp) << " -> " << uint32ToIp(dstIp) 
                      << " (Proto: " << static_cast<int>(protocol) << ", Size: " << packet.size() << ")" << std::endl;
        }
        
        forwardPacketToPeer(packet);
    }
}

bool P2PSystem::forwardPacketToPeer(const std::vector<uint8_t>& packet) {
    // Check if the packet is an IPv4 packet
    if (packet.empty() || (packet[0] >> 4) != 4) {
        return false;  // Not an IPv4 packet
    }
    
    // Extract source and destination IPs for filtering
    uint32_t srcIp = (packet[12] << 24) | (packet[13] << 16) | (packet[14] << 8) | packet[15];
    uint32_t dstIp = (packet[16] << 24) | (packet[17] << 16) | (packet[18] << 8) | packet[19];
    
    std::string srcIpStr = uint32ToIp(srcIp);
    std::string dstIpStr = uint32ToIp(dstIp);
    
    // Forward packets that are meant for the peer OR are broadcast/multicast packets
    // Modified to allow multicast/broadcast packets necessary for game discovery
    bool isForPeer = (dstIpStr == peer_virtual_ip_);
    bool isBroadcast = (dstIpStr == "10.0.0.255" || dstIpStr == "255.255.255.255");
    bool isMulticast = (dstIp >> 28) == 14; // 224.0.0.0/4 (first octet 224-239)
    
    if (!isForPeer && !isBroadcast && !isMulticast) {
        std::cout << "Dropping packet not meant for peer: " << srcIpStr << " -> " << dstIpStr << std::endl;
        return false;
    }
    
    // Convert packet to string for UDP transmission
    std::string packet_str(packet.begin(), packet.end());
    
    // Send the packet to the peer
    std::cout << "NET Send: " << srcIpStr << " -> " << dstIpStr 
              << " (Size: " << packet.size() << ")" << std::endl;
    return network_->sendMessage(packet_str);
}

bool P2PSystem::deliverPacketToTun(const std::vector<uint8_t>& packet) {
    // Check if the packet is an IPv4 packet
    if (packet.empty() || (packet[0] >> 4) != 4) {
        return false;  // Not an IPv4 packet
    }
    
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
        std::cout << "Dropping received packet not meant for us: " << srcIpStr << " -> " << dstIpStr << std::endl;
        return false;
    }
    
    // Send the packet to the TUN interface
    std::cout << "Delivering packet to TUN: " << srcIpStr << " -> " << dstIpStr 
              << " (Size: " << packet.size() << ")" << std::endl;
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
    if (on_connection_) {
        on_connection_(connected, peer_username_);
    }
    
    if (connected) {
        if (on_status_) {
            on_status_("Connected to " + peer_username_);
        }
        
        // Start the virtual network interface now that we're connected
        if (!startNetworkInterface()) {
            if (on_status_) {
                on_status_("Failed to start network interface, connection will be limited");
            }
        }
    } else {
        if (on_status_) {
            on_status_("Disconnected from peer");
        }
        
        // Stop the network interface
        stopNetworkInterface();
        
        peer_username_ = "";
    }
}

void P2PSystem::setStatusCallback(StatusCallback callback) {
    on_status_ = std::move(callback);
}

void P2PSystem::setConnectionCallback(ConnectionCallback callback) {
    on_connection_ = std::move(callback);
}

void P2PSystem::setConnectionRequestCallback(ConnectionRequestCallback callback) {
    on_connection_request_ = std::move(callback);
} 