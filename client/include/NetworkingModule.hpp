#pragma once
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <boost/asio.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/steady_timer.hpp>
#include "SystemStateManager.hpp"

class UDPNetwork {
public:
    using MessageCallback = std::function<void(const std::vector<uint8_t>)>;
    
    UDPNetwork(
        std::unique_ptr<boost::asio::ip::udp::socket>,
        boost::asio::io_context&,
        std::shared_ptr<SystemStateManager>);
    ~UDPNetwork();
    
    // Setup and connection
    bool startListening(int port);
    bool connectToPeer(const std::string& ip, int port);
    
    // Disconnet and shutdown
    void stopConnection();
    void shutdown();
    
    bool isConnected() const;
    
    // Async operations, sending to peer, called from TUNInterface
    bool sendMessage(const std::vector<uint8_t>& data);
    void setMessageCallback(MessageCallback callback);
    
    // Graceful disconnection
    void sendDisconnectNotification();
    
    // Get local information
    int getLocalPort() const;
    std::string getLocalAddress() const;

private:
     // Packet types
    enum class PacketType : uint8_t {
        HOLE_PUNCH = 0x01,
        HEARTBEAT = 0x02,
        MESSAGE = 0x03,
        ACK = 0x04,
        DISCONNECT = 0x05
    };

    // Async operations, receiving from peer, sending to TUNInterface
    void startAsyncReceive();
    void handleReceiveFrom(
        const boost::system::error_code&,
        std::size_t, 
        std::shared_ptr<std::vector<uint8_t>>, 
        std::shared_ptr<boost::asio::ip::udp::endpoint>);
    void processReceivedData(
        std::size_t, 
        std::shared_ptr<std::vector<uint8_t>>, 
        std::shared_ptr<boost::asio::ip::udp::endpoint>);
    void processMessage(std::vector<uint8_t>, const boost::asio::ip::udp::endpoint&);
    void handleSendComplete(const boost::system::error_code&, std::size_t, uint32_t);
    
    // Internal disconnect handler
    void handleDisconnect();

    // UDP hole punching
    void startHolePunchingProcess(const boost::asio::ip::udp::endpoint&);
    void sendHolePunchPacket();
    
    // Connection management
    void checkAllConnections();
    void notifyConnectionEvent(NetworkEvent, const std::string& = "");

    // Keep-alive functionality
    void startKeepAliveTimer();
    void stopKeepAliveTimer();
    void handleKeepAlive(const boost::system::error_code&);

    // Custom header
    uint32_t attachCustomHeader(
        const std::shared_ptr<std::vector<uint8_t>>&,
        PacketType,
        std::optional<uint32_t> = std::nullopt);
    
    // Constants
    static constexpr size_t MAX_PACKET_SIZE = 65507; // Max UDP packet size
    static constexpr uint16_t PROTOCOL_VERSION = 1;
    static constexpr uint32_t MAGIC_NUMBER = 0x12345678;

    std::atomic<bool> running;
    int localPort;
    std::string localAddress;

    // ASIO and IO context objects
    std::unique_ptr<boost::asio::ip::udp::socket> socket;
    boost::asio::io_context& ioContext;
    std::thread ioThread;
    boost::asio::steady_timer keepAliveTimer;
    
    // Ack tracking
    std::atomic<uint32_t> nextSeqNumber;
    std::unordered_map<uint32_t, std::chrono::time_point<std::chrono::steady_clock>> pendingAcks;
    std::mutex pendingAcksMutex;
    
    // Peer connection management
    boost::asio::ip::udp::endpoint peerEndpoint;
    PeerConnectionInfo peerConnection;
    // TODO: FOR *1, SEE HOW THIS WILL CHANGE FOR MULTIPLE PEERS
    std::string currentPeerEndpoint;

    // TODO: FOR *1, USE MAP, maybe from std::endpoint to PeerConnectionInfo?
    // Maybe fromm std::string to PeerConnectionInfo?
    // Maybe PeerConnectionInfo holds endpoint or string?
    
    // State manager for event queuing
    std::shared_ptr<SystemStateManager> stateManager;
    
    // Callbacks
    MessageCallback onMessageCallback;
};
