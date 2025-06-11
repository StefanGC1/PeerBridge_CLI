#pragma once
#include "signaling.hpp"
#include "Stun.hpp"
#include "NetworkingModule.hpp"
#include "TUNInterface.hpp"
#include "NetworkConfigManager.hpp"
#include "SystemStateManager.hpp"
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <functional>
#include <unordered_map>

// Forward declarations
struct IPPacket;

class P2PSystem
{
public:
    P2PSystem();
    ~P2PSystem();
    
    // Initialization
    // TODO: REFACTORIZE FOR *1
    bool initialize(const std::string&, const std::string&, int = 0);
    
    // Connection
    bool connectToPeer(const std::string&);
    
    // Disconnect and complete shutdown
    // TODO: FOR *1, maybe make a peer-based stopConnection
    void stopConnection();
    void shutdown();
    
    // Network interface
    bool startNetworkInterface();
    void stopNetworkInterface();
    
    // Status
    bool isConnected() const;
    bool isRunning() const;
    bool getIsHost() const;
    void setRunning();
    
    // Connection request handling
    // TODO: REMOVE FOR *1
    void acceptIncomingRequest();
    void rejectIncomingRequest();
    
    // Connection monitoring
    void monitorLoop();
    void handleNetworkEvent(const NetworkEventData&);
    
private:
    // Network discovery
    bool discoverPublicAddress();
    
    // Handler methods
    void handleConnectionRequest(const std::string&);
    void handlePeerInfo(const std::string&, const std::string&, int);
    void handleConnectionInit(const std::string&, const std::string&, int);
    void handleNetworkData(std::vector<uint8_t>);
    void handlePacketFromTun(const std::vector<uint8_t>&);
    
    // IP helpers
    void assignIPAddresses();
    
    // Packet analysis and forwarding
    bool forwardPacketToPeer(const std::vector<uint8_t>&);
    bool deliverPacketToTun(std::vector<uint8_t>);

    // Virtual network configuration
    static constexpr const char* VIRTUAL_NETWORK = "10.0.0.0";
    static constexpr const char* VIRTUAL_NETMASK = "255.255.255.0";
    static constexpr const char* HOST_IP = "10.0.0.1";
    static constexpr const char* CLIENT_IP = "10.0.0.2";

    // Data
    std::string username;
    std::string pendingRequestFrom;
    std::atomic<bool> running;
    std::atomic<bool> isHost;
    
    std::string localVirtualIp;
    // TODO: REFACTORIZE FOR *1, KEEP virtual_ip -> public_ip map
    std::string peerVirtualIp;

    std::string publicIp;
    int publicPort;

    std::string peerUsername;
    std::string peerIp;
    int peerPort;

    // State management
    std::shared_ptr<SystemStateManager> stateManager;
    std::thread monitorThread;
    
    // Components
    NetworkConfigManager networkConfigManager;
    SignalingClient signalingClient;
    StunClient stunService;
    std::unique_ptr<UDPNetwork> networkModule;
    std::unique_ptr<TunInterface> tunInterface;
}; 