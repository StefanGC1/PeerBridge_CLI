#include "chat_app.hpp"
#include <iostream>

ChatApplication::ChatApplication() 
    : running_(false), public_port_(0), peer_port_(0)
{
}

ChatApplication::~ChatApplication() {
    disconnect();
}

bool ChatApplication::initialize(const std::string& server_url, const std::string& username, int local_port) {
    username_ = username;
    running_ = true;
    
    // Set up callbacks
    signaling_.setConnectCallback([this](bool connected) {
        if (connected) {
            this->signaling_.sendGreeting();
        }
    });
    
    signaling_.setChatRequestCallback([this](const std::string& from) {
        this->handleChatRequest(from);
    });
    
    signaling_.setPeerInfoCallback([this](const std::string& username, const std::string& ip, int port) {
        this->handlePeerInfo(username, ip, port);
    });
    
    signaling_.setChatInitCallback([this](const std::string& username, const std::string& ip, int port) {
        this->handleChatInit(username, ip, port);
    });
    
    network_.setMessageCallback([this](const std::string& message) {
        this->handlePeerMessage(message);
    });
    
    network_.setConnectionCallback([this](bool connected) {
        this->handleConnectionChange(connected);
    });
    
    // Connect to signaling server
    if (!signaling_.connect(server_url)) {
        if (on_status_) {
            on_status_("Failed to connect to signaling server");
        }
        return false;
    }
    
    // Start UDP network
    if (!network_.startListening(local_port)) {
        if (on_status_) {
            on_status_("Failed to start UDP network");
        }
        return false;
    }
    
    // Discover public address
    if (!discoverPublicAddress()) {
        if (on_status_) {
            on_status_("Failed to discover public address, NAT traversal may not work");
        }
        // Continue anyway, might work on local network
    }
    
    // Register with the signaling server
    if (public_ip_.empty()) {
        // Use local address if STUN failed
        signaling_.registerUser(username_, network_.getLocalAddress(), network_.getLocalPort());
    } else {
        signaling_.registerUser(username_, public_ip_, public_port_);
    }
    
    if (on_status_) {
        on_status_("Initialized successfully");
    }
    
    return true;
}

bool ChatApplication::discoverPublicAddress() {
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

bool ChatApplication::connectToPeer(const std::string& peer_username) {
    if (network_.isConnected()) {
        if (on_status_) {
            on_status_("Already connected to a peer");
        }
        return false;
    }
    
    peer_username_ = peer_username;
    
    // Request peer info from signaling server
    signaling_.requestPeerInfo(peer_username);
    
    // Request chat connection
    signaling_.sendChatRequest(peer_username);
    
    if (on_status_) {
        on_status_("Sent chat request to " + peer_username);
    }
    
    return true;
}

void ChatApplication::disconnect() {
    running_ = false;
    
    network_.disconnect();
    signaling_.disconnect();
    
    peer_username_ = "";
    pending_request_from_ = "";
    
    if (on_status_) {
        on_status_("Disconnected");
    }
}

bool ChatApplication::sendMessage(const std::string& message) {
    if (!network_.isConnected()) {
        if (on_status_) {
            on_status_("Not connected to a peer");
        }
        return false;
    }
    
    return network_.sendMessage(message);
}

void ChatApplication::processIncomingMessages() {
    // This function would be needed if we had a message queue
    // but our current design uses callbacks directly
}

bool ChatApplication::isConnected() const {
    return network_.isConnected();
}

bool ChatApplication::isRunning() const {
    return running_;
}

void ChatApplication::acceptIncomingRequest() {
    if (pending_request_from_.empty()) {
        if (on_status_) {
            on_status_("No pending chat request");
        }
        return;
    }
    
    signaling_.acceptChatRequest();
    if (on_status_) {
        on_status_("Accepted chat request from " + pending_request_from_);
    }
    
    peer_username_ = pending_request_from_;
    pending_request_from_ = "";
}

void ChatApplication::rejectIncomingRequest() {
    if (pending_request_from_.empty()) {
        if (on_status_) {
            on_status_("No pending chat request");
        }
        return;
    }
    
    signaling_.declineChatRequest();
    if (on_status_) {
        on_status_("Rejected chat request from " + pending_request_from_);
    }
    
    pending_request_from_ = "";
}

// Handler methods
void ChatApplication::handleChatRequest(const std::string& from) {
    pending_request_from_ = from;
    
    if (on_chat_request_) {
        on_chat_request_(from);
    }
}

void ChatApplication::handlePeerInfo(const std::string& username, const std::string& ip, int port) {
    if (username != peer_username_) {
        return; // Not the peer we're looking for
    }
    
    peer_ip_ = ip;
    peer_port_ = port;
    
    if (on_status_) {
        on_status_("Got peer info: " + username + " at " + ip + ":" + std::to_string(port));
    }
}

void ChatApplication::handleChatInit(const std::string& username, const std::string& ip, int port) {
    peer_username_ = username;
    peer_ip_ = ip;
    peer_port_ = port;
    
    if (on_status_) {
        on_status_("Chat initialized with " + username + ", connecting...");
    }
    
    // Start UDP hole punching process
    if (!network_.connectToPeer(ip, port)) {
        if (on_status_) {
            on_status_("Failed to initiate UDP hole punching, will retry...");
        }
        // We'll keep trying in the background
    }
}

void ChatApplication::handlePeerMessage(const std::string& message) {
    if (on_message_) {
        on_message_(peer_username_, message);
    }
}

void ChatApplication::handleConnectionChange(bool connected) {
    if (on_connection_) {
        on_connection_(connected, peer_username_);
    }
    
    if (connected) {
        if (on_status_) {
            on_status_("Connected to " + peer_username_);
        }
    } else {
        if (on_status_) {
            on_status_("Disconnected from peer");
        }
        peer_username_ = "";
    }
}

// Callback setters
void ChatApplication::setMessageCallback(MessageCallback callback) {
    on_message_ = std::move(callback);
}

void ChatApplication::setStatusCallback(StatusCallback callback) {
    on_status_ = std::move(callback);
}

void ChatApplication::setConnectionCallback(ConnectionCallback callback) {
    on_connection_ = std::move(callback);
}

void ChatApplication::setChatRequestCallback(ChatRequestCallback callback) {
    on_chat_request_ = std::move(callback);
}