#pragma once
#include <ixwebsocket/IXWebSocket.h>
#include <memory>
#include <atomic>
#include <functional>
#include <string>
#include <mutex>
#include <nlohmann/json.hpp>

class SignalingClient {
public:
    // Callback types
    using ConnectCallback = std::function<void(bool)>;
    using ChatRequestCallback = std::function<void(const std::string&)>;
    using PeerInfoCallback = std::function<void(const std::string&, const std::string&, int)>;
    using ChatInitCallback = std::function<void(const std::string&, const std::string&, int)>;
    
    SignalingClient();
    ~SignalingClient();
    
    // Connection management
    bool connect(const std::string& server_url);
    bool isConnected() const;
    void disconnect();
    
    // Server communication
    void sendGreeting();
    void registerUser(const std::string& username, const std::string& ip, int port);
    void requestUsername();
    void requestPeerInfo(const std::string& username);
    void sendChatRequest(const std::string& username);
    void acceptChatRequest();
    void declineChatRequest();
    
    // Callback setters
    void setConnectCallback(ConnectCallback callback);
    void setChatRequestCallback(ChatRequestCallback callback);
    void setPeerInfoCallback(PeerInfoCallback callback);
    void setChatInitCallback(ChatInitCallback callback);
    
private:
    void setupMessageHandlers();
    void handleMessage(const ix::WebSocketMessagePtr& msg);
    void handleJsonMessage(const nlohmann::json& data);
    
    std::unique_ptr<ix::WebSocket> ws_;
    std::atomic<bool> connected_;
    std::mutex mutex_;
    
    // Callbacks
    ConnectCallback onConnect_;
    ChatRequestCallback onChatRequest_;
    PeerInfoCallback onPeerInfo_;
    ChatInitCallback onChatInit_;
};