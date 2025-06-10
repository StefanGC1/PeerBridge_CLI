#pragma once
#include "signaling.hpp"
#include "stun.hpp"
#include "networking.hpp"
#include "tun_interface.hpp"
#include "networkconfigmanager.hpp"
#include "systemstatemanager.hpp"
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <functional>
#include <unordered_map>

// Forward declarations
struct IPPacket;

class P2PSystem {
public:
    P2PSystem();
    ~P2PSystem();
    
    // Initialization
    bool initialize(const std::string& server_url, const std::string& username, int local_port = 0);
    
    // Connection management
    bool connectToPeer(const std::string& peer_username);
    
    // New separated disconnect functions
    void stopConnection();   // Stop connection but keep system running
    void shutdown();         // Complete system shutdown
    
    // Network interface
    bool startNetworkInterface();
    void stopNetworkInterface();
    
    // Status
    bool isConnected() const;
    bool isRunning() const;
    bool isHost() const;

    void setRunning();
    
    // Connection request handling
    void acceptIncomingRequest();
    void rejectIncomingRequest();
    
    // Connection monitoring
    void monitorLoop();
    
private:
    // Network discovery
    bool discoverPublicAddress();
    
    // Handler methods
    void handleConnectionRequest(const std::string& from);
    void handlePeerInfo(const std::string& username, const std::string& ip, int port);
    void handleConnectionInit(const std::string& username, const std::string& ip, int port);
    void handleNetworkData(std::vector<uint8_t> data);
    void handlePacketFromTun(const std::vector<uint8_t>& packet);
    
    // IP helpers
    void assignIPAddresses();
    
    // Packet analysis and forwarding
    bool forwardPacketToPeer(const std::vector<uint8_t>& packet);
    bool deliverPacketToTun(std::vector<uint8_t> packet);
    
    // Members
    std::string username_;
    std::string pending_request_from_;
    std::atomic<bool> running_;
    std::atomic<bool> is_host_;
    std::mutex packet_mutex_;
    
    // Virtual network configuration
    static constexpr const char* VIRTUAL_NETWORK = "10.0.0.0";
    static constexpr const char* VIRTUAL_NETMASK = "255.255.255.0";
    static constexpr const char* HOST_IP = "10.0.0.1";
    static constexpr const char* CLIENT_IP = "10.0.0.2";
    
    std::string local_virtual_ip_;
    std::string peer_virtual_ip_;
    
    // State management
    std::shared_ptr<SystemStateManager> state_manager_;
    std::shared_ptr<PeerConnectionInfo> peer_connection_;
    std::thread monitor_thread_;
    
    // Components
    NetworkConfigManager networkConfigManager;
    SignalingClient signaling_;
    StunClient stun_;
    std::unique_ptr<UDPNetwork> network_;
    std::unique_ptr<TunInterface> tun_;
    
    // P2P network thread
    std::thread packet_handling_thread_;
    
    // Public address
    std::string public_ip_;
    int public_port_;
    
    // Peer info
    std::string peer_username_;
    std::string peer_ip_;
    int peer_port_;
}; 