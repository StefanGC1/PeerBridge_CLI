#include "signaling.hpp"
#include "logger.hpp"
#include <iostream>
#include <thread>
#include <chrono>

using json = nlohmann::json;

SignalingClient::SignalingClient() 
    : connected_(false)
{
    ws_ = std::make_unique<ix::WebSocket>();
}

SignalingClient::~SignalingClient() {
    disconnect();
}

bool SignalingClient::connect(const std::string& server_url) {
    if (connected_) {
        return true; // Already connected
    }
    
    ws_->setUrl(server_url);
    setupMessageHandlers();
    ws_->start();

    // Wait for connection with timeout
    int retries = 50;
    for (int i = 0; i < retries && !connected_; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!connected_) {
        std::cerr << "[Client] Failed to connect to server after timeout.\n";
        ws_->stop();
        return false;
    }

    return true;
}

void SignalingClient::disconnect() {
    if (ws_ && connected_) {
        ws_->stop();
        connected_ = false;
    }
}

bool SignalingClient::isConnected() const {
    return connected_;
}

void SignalingClient::setupMessageHandlers() {
    ws_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        handleMessage(msg);
    });
}

void SignalingClient::handleMessage(const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Message) {
        try {
            auto data = json::parse(msg->str);
            handleJsonMessage(data);
        }
        catch (const std::exception& e) {
            std::cerr << "[Server] Failed to parse message: " << e.what() << std::endl;
            clog << "[Server] (unparsed): " << msg->str << std::endl;
        }
    }
    else if (msg->type == ix::WebSocketMessageType::Open) {
        clog << "[Client] Connected to server." << std::endl;
        connected_ = true;
        if (onConnect_) {
            onConnect_(true);
        }
    }
    else if (msg->type == ix::WebSocketMessageType::Close) {
        clog << "[Client] Connection closed." << std::endl;
        connected_ = false;
        if (onConnect_) {
            onConnect_(false);
        }
    }
    else if (msg->type == ix::WebSocketMessageType::Error) {
        std::cerr << "[Client] Connection error: " << msg->errorInfo.reason << std::endl;
        connected_ = false;
        if (onConnect_) {
            onConnect_(false);
        }
    }
}

void SignalingClient::handleJsonMessage(const nlohmann::json& data) {
    std::string type = data.value("type", "");
    
    if (type == "greet-back") {
        clog << "[Server -> Client] " << data["message"] << std::endl;
    }
    else if (type == "register-ack") {
        clog << "[Server -> Client] " << data["message"] << std::endl;
    }
    else if (type == "your-name") {
        clog << "[Server -> Client] You are registered as: " << data["username"] << std::endl;
    }
    else if (type == "peer-info") {
        std::string peerName = data["username"];
        std::string ip = data["ip"];
        int port = data["port"];
        clog << "[Server] Peer " << peerName << " is at " << ip << ":" << port << std::endl;
        
        if (onPeerInfo_) {
            onPeerInfo_(peerName, ip, port);
        }
    }
    else if (type == "chat-request") {
        std::string from = data["from"];
        clog << "[Request] " << from << " wants to chat." << std::endl;
        
        if (onChatRequest_) {
            onChatRequest_(from);
        }
    }
    else if (type == "chat-init") {
        std::string peer_ip = data["ip"];
        int peer_port = data["port"];
        std::string peer_username = data["username"];
        clog << "[Server] Chat initialized with " << peer_username << std::endl;
        
        if (onChatInit_) {
            onChatInit_(peer_username, peer_ip, peer_port);
        }
    }
    else if (type == "error") {
        clog << "[Server ERROR] " << data["message"] << std::endl;
    }
    else {
        clog << "[Server] Unexpected message type: " << type << std::endl;
    }
}

void SignalingClient::sendGreeting() {
    if (!isConnected()) {
        clog << "[Client] Not connected.\n";
        return;
    }
    
    json js = {
        {"type", "greeting"}
    };
    ws_->send(js.dump());
}

void SignalingClient::registerUser(const std::string& username, const std::string& ip, int port) {
    if (!isConnected()) {
        clog << "[Client] Not connected.\n";
        return;
    }
    
    json js = {
        {"type", "register"},
        {"username", username},
        {"ip", ip},
        {"port", port}
    };
    ws_->send(js.dump());
}

void SignalingClient::requestUsername() {
    if (!isConnected()) {
        clog << "[Client] Not connected.\n";
        return;
    }
    
    json js = {
        {"type", "get-name"}
    };
    ws_->send(js.dump());
}

void SignalingClient::requestPeerInfo(const std::string& username) {
    if (!isConnected()) {
        clog << "[Client] Not connected.\n";
        return;
    }
    
    json j = {
        {"type", "get-peer"},
        {"username", username}
    };
    ws_->send(j.dump());
}

void SignalingClient::sendChatRequest(const std::string& username) {
    if (!isConnected()) {
        clog << "[Client] Not connected.\n";
        return;
    }
    
    json j = {
        {"type", "start-chat"},
        {"target", username}
    };
    ws_->send(j.dump());
}

void SignalingClient::acceptChatRequest() {
    if (!isConnected()) {
        clog << "[Client] Not connected.\n";
        return;
    }
    
    json j = { {"type", "chat-accept"} };
    ws_->send(j.dump());
}

void SignalingClient::declineChatRequest() {
    if (!isConnected()) {
        clog << "[Client] Not connected.\n";
        return;
    }
    
    json j = { {"type", "chat-decline"} };
    ws_->send(j.dump());
}

void SignalingClient::setConnectCallback(ConnectCallback callback) {
    onConnect_ = std::move(callback);
}

void SignalingClient::setChatRequestCallback(ChatRequestCallback callback) {
    onChatRequest_ = std::move(callback);
}

void SignalingClient::setPeerInfoCallback(PeerInfoCallback callback) {
    onPeerInfo_ = std::move(callback);
}

void SignalingClient::setChatInitCallback(ChatInitCallback callback) {
    onChatInit_ = std::move(callback);
}