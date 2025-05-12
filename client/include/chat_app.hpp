#pragma once
#include "signaling.hpp"
#include "stun.hpp"
#include "networking.hpp"
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <functional>

class ChatApplication {
public:
    ChatApplication();
    ~ChatApplication();
    
    // Initialization
    bool initialize(const std::string& server_url, const std::string& username, int local_port = 0);
    
    // Connection management
    bool connectToPeer(const std::string& peer_username);
    void disconnect();
    
    // Message handling
    bool sendMessage(const std::string& message);
    void processIncomingMessages();
    
    // Status
    bool isConnected() const;
    bool isRunning() const;
    
    // Chat request handling
    void acceptIncomingRequest();
    void rejectIncomingRequest();
    
    // Set callbacks
    using MessageCallback = std::function<void(const std::string&, const std::string&)>;
    using StatusCallback = std::function<void(const std::string&)>;
    using ConnectionCallback = std::function<void(bool, const std::string&)>;
    using ChatRequestCallback = std::function<void(const std::string&)>;
    
    void setMessageCallback(MessageCallback callback);
    void setStatusCallback(StatusCallback callback);
    void setConnectionCallback(ConnectionCallback callback);
    void setChatRequestCallback(ChatRequestCallback callback);
    
private:
    // Network discovery
    bool discoverPublicAddress();
    
    // Handler methods
    void handleChatRequest(const std::string& from);
    void handlePeerInfo(const std::string& username, const std::string& ip, int port);
    void handleChatInit(const std::string& username, const std::string& ip, int port);
    void handlePeerMessage(const std::string& message);
    void handleConnectionChange(bool connected);
    
    // Members
    std::string username_;
    std::string pending_request_from_;
    std::atomic<bool> running_;
    std::mutex message_mutex_;
    
    // Components
    SignalingClient signaling_;
    StunClient stun_;
    std::unique_ptr<UDPNetwork> network_; // Changed to UDPNetwork
    
    // Public address
    std::string public_ip_;
    int public_port_;
    
    // Peer info
    std::string peer_username_;
    std::string peer_ip_;
    int peer_port_;
    
    // Callbacks
    MessageCallback on_message_;
    StatusCallback on_status_;
    ConnectionCallback on_connection_;
    ChatRequestCallback on_chat_request_;
};