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
#include "systemstatemanager.hpp"

class UDPNetwork {
public:
    using MessageCallback = std::function<void(const std::vector<uint8_t>)>;
    
    UDPNetwork(
        std::unique_ptr<boost::asio::ip::udp::socket> socket,
        boost::asio::io_context& context,
        std::shared_ptr<SystemStateManager> state_manager
    );
    ~UDPNetwork();
    
    // Setup and connection
    bool startListening(int port);
    bool connectToPeer(const std::string& ip, int port);
    
    // Disconnet and shutdown
    void stopConnection();
    void shutdown();
    
    bool isConnected() const;
    
    // Message handling
    bool sendMessage(const std::vector<uint8_t>& data);
    void setMessageCallback(MessageCallback callback);
    
    // Graceful disconnection
    void sendDisconnectNotification();
    
    // Get local information
    int getLocalPort() const;
    std::string getLocalAddress() const;

private:
    // Async operations
    void startAsyncReceive();
    void handleReceiveFrom(const boost::system::error_code& error, std::size_t bytes_transferred, 
                          std::shared_ptr<std::vector<uint8_t>> receiveBuffer, 
                          std::shared_ptr<boost::asio::ip::udp::endpoint> senderEndpoint);
    void handleSendComplete(const boost::system::error_code& error, std::size_t bytes_sent, uint32_t seq);
    void processReceivedData(std::size_t bytes_transferred, 
                           std::shared_ptr<std::vector<uint8_t>> receiveBuffer, 
                           std::shared_ptr<boost::asio::ip::udp::endpoint> senderEndpoint);
    
    // Internal disconnect handler
    void handleDisconnect();

    // UDP hole punching: send periodic keepalive packets
    void startHolePunchingProcess(const boost::asio::ip::udp::endpoint& peer_endpoint);
    void sendHolePunchPacket();
    void processMessage(std::vector<uint8_t> message, const boost::asio::ip::udp::endpoint& sender);
    
    // Connection management
    void checkAllConnections();
    void notifyConnectionEvent(NetworkEvent event, const std::string& endpoint = "");
    
    // Constants
    static constexpr size_t MAX_PACKET_SIZE = 65507; // Max UDP packet size
    static constexpr uint16_t PROTOCOL_VERSION = 1;
    static constexpr uint32_t MAGIC_NUMBER = 0x12345678;
    
    // Packet types
    enum class PacketType : uint8_t {
        HOLE_PUNCH = 0x01,
        HEARTBEAT = 0x02,
        MESSAGE = 0x03,
        ACK = 0x04,
        DISCONNECT = 0x05  // New disconnect notification packet
    };
    
    std::atomic<bool> running_;
    int local_port_;
    std::string local_address_;
    
    boost::asio::io_context& io_context_;
    std::optional<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>> workGuard;

    std::unique_ptr<boost::asio::ip::udp::socket> socket_;
    boost::asio::ip::udp::endpoint peer_endpoint_;
    
    // Async operation state
    std::thread io_thread_;

    std::thread keepalive_thread_;
    std::mutex send_mutex_;
    
    // Connection state tracking
    std::atomic<uint32_t> next_sequence_number_;
    std::unordered_map<uint32_t, std::chrono::time_point<std::chrono::steady_clock>> pending_acks_;
    std::mutex pending_acks_mutex_;
    
    // Peer connection management
    PeerConnectionInfo peer_connection_;  // Changed from shared_ptr to concrete object
    std::string current_peer_endpoint_;   // For event identification
    
    // State manager for event queuing
    std::shared_ptr<SystemStateManager> state_manager_;
    
    // Callbacks
    MessageCallback on_message_;
};