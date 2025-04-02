#pragma once
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <chrono>
#include <unordered_map>
#include <boost/asio.hpp>

class UDPNetwork {
public:
    using MessageCallback = std::function<void(const std::string&)>;
    using ConnectionCallback = std::function<void(bool)>;
    
    UDPNetwork();
    ~UDPNetwork();
    
    // Setup and connection
    bool startListening(int port);
    bool connectToPeer(const std::string& ip, int port);
    void disconnect();
    bool isConnected() const;
    
    // Message handling
    bool sendMessage(const std::string& message);
    void setMessageCallback(MessageCallback callback);
    void setConnectionCallback(ConnectionCallback callback);
    
    // Get local information
    int getLocalPort() const;
    std::string getLocalAddress() const;

private:
    // UDP hole punching: send periodic keepalive packets
    void startHolePunchingProcess(const boost::asio::ip::udp::endpoint& peer_endpoint);
    void sendHolePunchPacket();
    void receiveLoop();
    void processMessage(const std::string& message, const boost::asio::ip::udp::endpoint& sender);
    
    // Connection management
    void checkConnectionStatus();
    
    // Constants
    static constexpr size_t MAX_PACKET_SIZE = 65507; // Max UDP packet size
    static constexpr uint16_t PROTOCOL_VERSION = 1;
    static constexpr uint32_t MAGIC_NUMBER = 0x12345678;
    
    // Packet types
    enum class PacketType : uint8_t {
        HOLE_PUNCH = 0x01,
        HEARTBEAT = 0x02,
        MESSAGE = 0x03,
        ACK = 0x04
    };
    
    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    int local_port_;
    std::string local_address_;
    
    boost::asio::io_context io_context_;
    std::unique_ptr<boost::asio::ip::udp::socket> socket_;
    boost::asio::ip::udp::endpoint peer_endpoint_;
    
    std::thread receive_thread_;
    std::thread keepalive_thread_;
    std::mutex send_mutex_;
    
    // Connection state tracking
    std::chrono::time_point<std::chrono::steady_clock> last_received_time_;
    std::atomic<uint32_t> next_sequence_number_;
    std::unordered_map<uint32_t, std::chrono::time_point<std::chrono::steady_clock>> pending_acks_;
    std::mutex pending_acks_mutex_;
    
    // Callbacks
    MessageCallback on_message_;
    ConnectionCallback on_connection_;
};