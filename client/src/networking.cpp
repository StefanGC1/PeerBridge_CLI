#include "networking.hpp"
#include "logger.hpp"
#include <iostream>
#include <chrono>
#include <random>
#include <cstring>
#include <boost/asio/ip/address_v6.hpp>

UDPNetwork::UDPNetwork(
    std::unique_ptr<boost::asio::ip::udp::socket> socket,
    boost::asio::io_context& context,
    std::shared_ptr<SystemStateManager> state_manager,
    std::shared_ptr<PeerConnectionInfo> peer_info) 
    : running_(false), local_port_(0), next_sequence_number_(0)
    , socket_(std::move(socket)), io_context_(context)
    , state_manager_(state_manager), peer_info_(peer_info)
{
}

UDPNetwork::~UDPNetwork() {
    shutdown();
}

bool UDPNetwork::startListening(int port) {
    try {
        // Get local endpoint information
        boost::asio::ip::udp::endpoint local_endpoint = socket_->local_endpoint();
        local_address_ = local_endpoint.address().to_string();
        local_port_ = local_endpoint.port();
        
        // Set socket to non-blocking mode for async operations
        socket_->non_blocking(true);
        
        // Increase socket buffer sizes
        boost::asio::socket_base::send_buffer_size sendBufferOption(1024 * 1024); // 1MB
        boost::asio::socket_base::receive_buffer_size recvBufferOption(1024 * 1024); // 1MB
        socket_->set_option(sendBufferOption);
        socket_->set_option(recvBufferOption);
        
        // Set running flag to true
        running_ = true;
        
        // Start async receiving
        startAsyncReceive();
        
        // Start IO thread to handle asynchronous operations
        if (!io_thread_.joinable()) {
            io_thread_ = std::thread([this]() {
                try {
                    while (!state_manager_->isInState(SystemState::SHUTTING_DOWN)) {
                        try {
                            io_context_.run_for(std::chrono::milliseconds(100));
                            io_context_.restart();
                        } catch (const std::exception& e) {
                            std::cerr << "[Network] IO context error: " << e.what() << std::endl;
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[Network] IO thread error: " << e.what() << std::endl;
                }
            });
        }
        
        clog << "[Network] Listening on UDP " << local_address_ << ":" << local_port_ << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Network] Failed to start UDP listener: " << e.what() << std::endl;
        return false;
    }
}

bool UDPNetwork::connectToPeer(const std::string& ip, int port) {
    if (peer_info_->isConnected()) {
        std::cout << "[Network] Already connected to a peer." << std::endl;
        return false;
    }
    
    try {
        boost::asio::ip::address addr = boost::asio::ip::make_address(ip);
        peer_endpoint_ = boost::asio::ip::udp::endpoint(addr, port);
        
        std::cout << "[Network] Starting UDP hole punching to " << ip << ":" << port << "..." << std::endl;
        
        // Update system state
        state_manager_->setState(SystemState::CONNECTING);
        
        // Start the hole punching process
        startHolePunchingProcess(peer_endpoint_);
        
        // Mark as connected and update peer info
        peer_info_->setConnected(true);
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Network] Connect error: " << e.what() << std::endl;
        return false;
    }
}

void UDPNetwork::startHolePunchingProcess(const boost::asio::ip::udp::endpoint& peer_endpoint) {
    // Send initial hole punching packets
    for (int i = 0; i < 5; i++) {
        sendHolePunchPacket();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Start keepalive thread
    keepalive_thread_ = std::thread([this]() {
        while (running_ && peer_info_->isConnected()) {
            // Send hole punch packet
            if (peer_info_->isConnected()) {
                this->sendHolePunchPacket();
            }
            
            // Check connection status
            this->checkConnectionStatus();
            
            // Sleep for a short period
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    });
    keepalive_thread_.detach();
}

void UDPNetwork::sendHolePunchPacket() {
    try {
        // Create hole-punch packet with shared ownership
        auto packet = std::make_shared<std::vector<uint8_t>>(16);
        
        // Set magic number
        (*packet)[0] = (MAGIC_NUMBER >> 24) & 0xFF;
        (*packet)[1] = (MAGIC_NUMBER >> 16) & 0xFF;
        (*packet)[2] = (MAGIC_NUMBER >> 8) & 0xFF;
        (*packet)[3] = MAGIC_NUMBER & 0xFF;
        
        // Set protocol version
        (*packet)[4] = (PROTOCOL_VERSION >> 8) & 0xFF;
        (*packet)[5] = PROTOCOL_VERSION & 0xFF;
        
        // Set packet type
        (*packet)[6] = static_cast<uint8_t>(PacketType::HOLE_PUNCH);
        
        // Set sequence number
        uint32_t seq = next_sequence_number_++;
        (*packet)[8] = (seq >> 24) & 0xFF;
        (*packet)[9] = (seq >> 16) & 0xFF;
        (*packet)[10] = (seq >> 8) & 0xFF;
        (*packet)[11] = seq & 0xFF;
        
        // Send packet asynchronously
        socket_->async_send_to(
            boost::asio::buffer(*packet), peer_endpoint_,
            [packet](const boost::system::error_code& error, std::size_t bytes_sent) {
                if (error && error != boost::asio::error::operation_aborted && 
                    error != boost::asio::error::would_block &&
                    error.value() != 10035) { // WSAEWOULDBLOCK
                    std::cerr << "[Network] Error sending hole-punch packet: " << error.message()
                              << " with error code: " << error.value() << std::endl;
                }
            }
        );
    } catch (const std::exception& e) {
        std::cerr << "[Network] Error preparing hole-punch packet: " << e.what() << std::endl;
    }
}

void UDPNetwork::checkConnectionStatus() {
    if (peer_info_->hasTimedOut(10)) {
        clog << "[Network] Connection timeout. No packets received for over 10 seconds." << std::endl;
        
        // Handle disconnection
        handleDisconnect();
    }
}

// New implementation: Stop connection but keep network stack running
void UDPNetwork::stopConnection() {
    // Send disconnect notification to peer
    sendDisconnectNotification();

    if (keepalive_thread_.joinable()) {
        keepalive_thread_.join();
    }
    
    // Update state
    peer_info_->setConnected(false);
    state_manager_->setState(SystemState::IDLE);
    
    clog << "[Network] Stopped connection to peer" << std::endl;
}

// New implementation: Full network subsystem shutdown
void UDPNetwork::shutdown() {
    // First stop any active connection
    if (peer_info_->isConnected()) {
        stopConnection();
    }
    
    // Then shut down the network stack
    running_ = false;
    state_manager_->setState(SystemState::SHUTTING_DOWN);
    
    if (socket_) {
        try {
            // Cancel any pending async operations
            boost::system::error_code ec;
            socket_->cancel(ec);
            socket_->close(ec);
        } catch (...) {}
    }
    
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    
    // Stop io_context 
    io_context_.stop();
    
    clog << "[Network] Network subsystem shut down" << std::endl;
}

// New implementation: Send a disconnect notification packet to peer
void UDPNetwork::sendDisconnectNotification() {
    try {
        if (!peer_info_->isConnected() || !socket_) {
            return; // No connection to notify
        }
        
        // Create disconnect packet
        auto packet = std::make_shared<std::vector<uint8_t>>(16);
        
        // Set magic number
        (*packet)[0] = (MAGIC_NUMBER >> 24) & 0xFF;
        (*packet)[1] = (MAGIC_NUMBER >> 16) & 0xFF;
        (*packet)[2] = (MAGIC_NUMBER >> 8) & 0xFF;
        (*packet)[3] = MAGIC_NUMBER & 0xFF;
        
        // Set protocol version
        (*packet)[4] = (PROTOCOL_VERSION >> 8) & 0xFF;
        (*packet)[5] = PROTOCOL_VERSION & 0xFF;
        
        // Set packet type to DISCONNECT
        (*packet)[6] = static_cast<uint8_t>(PacketType::DISCONNECT);
        
        // Set sequence number
        uint32_t seq = next_sequence_number_++;
        (*packet)[8] = (seq >> 24) & 0xFF;
        (*packet)[9] = (seq >> 16) & 0xFF;
        (*packet)[10] = (seq >> 8) & 0xFF;
        (*packet)[11] = seq & 0xFF;
        
        // Send packet - try multiple times to increase chance of delivery
        for (int i = 0; i < 3; i++) {
            socket_->async_send_to(
                boost::asio::buffer(*packet), peer_endpoint_,
                [packet](const boost::system::error_code& error, std::size_t bytes_sent) {
                    // We don't care about errors here since we're disconnecting anyway
                }
            );
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } catch (const std::exception& e) {
        std::cerr << "[Network] Error sending disconnect notification: " << e.what() << std::endl;
    }
}

bool UDPNetwork::isConnected() const {
    return peer_info_->isConnected();
}

bool UDPNetwork::sendMessage(const std::vector<uint8_t>& dataToSend) {
    if (!running_ || !socket_) {
        std::cerr << "[Network] Cannot send message: socket not available or system not running (disconnected)" << std::endl;
        return false;
    }
    
    try {
        // Calculate total packet size: header (16 bytes) + message
        size_t packetSize = 16 + dataToSend.size();
        if (packetSize > MAX_PACKET_SIZE) {
            std::cerr << "[Network] Message too large, max size is " << (MAX_PACKET_SIZE - 16) << " bytes" << std::endl;
            return false;
        }
        
        /*
        * SMALL CUSTOM PROTOCOL HEADER
        */

        // Create packet with shared ownership for async operation
        auto packet = std::make_shared<std::vector<uint8_t>>(packetSize);
        
        // Set magic number
        (*packet)[0] = (MAGIC_NUMBER >> 24) & 0xFF;
        (*packet)[1] = (MAGIC_NUMBER >> 16) & 0xFF;
        (*packet)[2] = (MAGIC_NUMBER >> 8) & 0xFF;
        (*packet)[3] = MAGIC_NUMBER & 0xFF;
        
        // Set protocol version
        (*packet)[4] = (PROTOCOL_VERSION >> 8) & 0xFF;
        (*packet)[5] = PROTOCOL_VERSION & 0xFF;
        
        // Set packet type
        (*packet)[6] = static_cast<uint8_t>(PacketType::MESSAGE);
        
        // Set sequence number
        uint32_t seq = next_sequence_number_++;
        (*packet)[8] = (seq >> 24) & 0xFF;
        (*packet)[9] = (seq >> 16) & 0xFF;
        (*packet)[10] = (seq >> 8) & 0xFF;
        (*packet)[11] = seq & 0xFF;
        
        // Set message length
        uint32_t msg_len = static_cast<uint32_t>(dataToSend.size());
        (*packet)[12] = (msg_len >> 24) & 0xFF;
        (*packet)[13] = (msg_len >> 16) & 0xFF;
        (*packet)[14] = (msg_len >> 8) & 0xFF;
        (*packet)[15] = msg_len & 0xFF;
        
        // Copy message content
        std::memcpy(packet->data() + 16, dataToSend.data(), dataToSend.size());
        
        // Track for acknowledgment
        {
            std::lock_guard<std::mutex> ack_lock(pending_acks_mutex_);
            pending_acks_[seq] = std::chrono::steady_clock::now();
        }
        
        // Send packet asynchronously
        socket_->async_send_to(
            boost::asio::buffer(*packet), peer_endpoint_,
            [this, packet, seq](const boost::system::error_code& error, std::size_t bytes_sent) {
                this->handleSendComplete(error, bytes_sent, seq);
            }
        );
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Network] Send preparation error: " << e.what() << std::endl;
        return false;
    }
}

void UDPNetwork::handleSendComplete(
    const boost::system::error_code& error, 
    std::size_t bytes_sent, 
    uint32_t seq) {
    
    if (error) {
        if (error == boost::asio::error::would_block || 
            error == boost::asio::error::try_again ||
            error.value() == 10035) {  // WSAEWOULDBLOCK
            
            // Retry after a short delay - this handles buffer full condition
            std::cerr << "[Network] Send buffer full, retrying after delay" << std::endl;
            
            auto packet_copy = std::make_shared<std::vector<uint8_t>>(0);  // Empty packet as placeholder

            boost::asio::post(io_context_, [this, seq]() {
                // We won't resend, just track that we're dropping the packet
                std::cerr << "[Network] Dropping packet due to send buffer limits: seq=" << seq << std::endl;
                
                // Remove from pending acks since we're not retrying
                std::lock_guard<std::mutex> lock(pending_acks_mutex_);
                pending_acks_.erase(seq);
            });
        }
        else {
            std::cerr << "[Network] Send error: " << error.message() 
                      << " with error code: " << error.value() << std::endl;
            
            // Only disconnect on fatal errors, not temporary ones
            if (error != boost::asio::error::operation_aborted) {
                boost::asio::post(io_context_, [this]() {this->handleDisconnect(); });
            }
        }
    }
}

void UDPNetwork::processMessage(std::vector<uint8_t> message, const boost::asio::ip::udp::endpoint& sender) {
    if (on_message_) {
        on_message_(std::move(message));
    }
}

void UDPNetwork::setMessageCallback(MessageCallback callback) {
    on_message_ = std::move(callback);
}

int UDPNetwork::getLocalPort() const {
    return local_port_;
}

std::string UDPNetwork::getLocalAddress() const {
    return local_address_;
}

void UDPNetwork::startAsyncReceive() {
    if (!socket_) return;
    
    // Reuse the same buffer
    if (!receiveBuffer_)
        receiveBuffer_ = std::make_shared<std::vector<uint8_t>>(MAX_PACKET_SIZE);
    senderEndpoint_ = std::make_shared<boost::asio::ip::udp::endpoint>();
    
    socket_->async_receive_from(
        boost::asio::buffer(*receiveBuffer_), *senderEndpoint_,
        [this](const boost::system::error_code& error, std::size_t bytes_transferred) {
            this->handleReceiveFrom(error, bytes_transferred);
        }
    );
}

void UDPNetwork::handleReceiveFrom(const boost::system::error_code& error, std::size_t bytes_transferred) {
    if (!running_) return;
    
    if (!error) {
        // Process the received data
        processReceivedData(bytes_transferred);
        
        // Queue up another receive operation immediately
        startAsyncReceive();
    }
    else if (error != boost::asio::error::operation_aborted) {
        // Handle error but don't terminate unless it's fatal
        std::cerr << "[Network] Receive error: " << error.message()
                  << " with error code: " << error.value() << std::endl;
        
        // For recoverable errors, try again after short delay
        if (error == boost::asio::error::would_block || 
            error == boost::asio::error::try_again) {
            boost::asio::post(io_context_, [this]() { startAsyncReceive(); });
        }
        else {
            // For fatal errors, handle disconnect
            handleDisconnect();
        }
    }
}

void UDPNetwork::processReceivedData(std::size_t bytes_transferred) {
    // Skip if we don't have enough data for header
    if (bytes_transferred < 16) {
        std::cerr << "[Network] Received packet too small: " << bytes_transferred << " bytes" << std::endl;
        return;
    }
    
    const std::vector<uint8_t>& buffer = *receiveBuffer_;
    
    // Validate magic number
    uint32_t magic = (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
    if (magic != MAGIC_NUMBER) {
        // Silent ignore, probably not our protocol
        return;
    }
    
    // Validate protocol version
    uint16_t version = (buffer[4] << 8) | buffer[5];
    if (version != PROTOCOL_VERSION) {
        std::cerr << "[Network] Unsupported protocol version: " << version << std::endl;
        return;
    }
    
    // Get packet type
    PacketType packetType = static_cast<PacketType>(buffer[6]);
    
    // Get sequence number
    uint32_t seq = (buffer[8] << 24) | (buffer[9] << 16) | (buffer[10] << 8) | buffer[11];
    
    // Update peer activity time
    peer_info_->updateActivity();
    
    // Store peer endpoint if not already connected
    if (!peer_info_->isConnected()) {
        peer_endpoint_ = *senderEndpoint_;
        peer_info_->setConnected(true);
        state_manager_->setState(SystemState::CONNECTED);
    }
    
    // Process packet based on type
    switch (packetType) {
        case PacketType::HOLE_PUNCH:
            // Update activity time, which was done above
            break;
            
        case PacketType::HEARTBEAT:
            // Done by keep-alive thread
            break;
            
        case PacketType::DISCONNECT:
            // Peer wants to disconnect

            // TODO: CHECK TOMORROW
            SYSTEM_LOG_INFO("[Network] Received disconnect notification from peer");
            NETWORK_LOG_INFO("[Network] Received disconnect notification from peer");
            handleDisconnect();
            break;
            
        case PacketType::MESSAGE: {
            // Get message length
            uint32_t msgLen = (buffer[12] << 24) | (buffer[13] << 16) | (buffer[14] << 8) | buffer[15];
            
            // Validate message length
            if (16 + msgLen > bytes_transferred) {
                NETWORK_LOG_ERROR("[Network] Message length exceeds packet size");
                return;
            }
            
            // Send ACK asynchronously
            auto ack = std::make_shared<std::vector<uint8_t>>(16);
            
            // Set magic number
            (*ack)[0] = (MAGIC_NUMBER >> 24) & 0xFF;
            (*ack)[1] = (MAGIC_NUMBER >> 16) & 0xFF;
            (*ack)[2] = (MAGIC_NUMBER >> 8) & 0xFF;
            (*ack)[3] = MAGIC_NUMBER & 0xFF;
            
            // Set protocol version
            (*ack)[4] = (PROTOCOL_VERSION >> 8) & 0xFF;
            (*ack)[5] = PROTOCOL_VERSION & 0xFF;
            
            // Set packet type
            (*ack)[6] = static_cast<uint8_t>(PacketType::ACK);
            
            // Set sequence number (same as received)
            (*ack)[8] = (seq >> 24) & 0xFF;
            (*ack)[9] = (seq >> 16) & 0xFF;
            (*ack)[10] = (seq >> 8) & 0xFF;
            (*ack)[11] = seq & 0xFF;
            
            // Send ACK
            socket_->async_send_to(
                boost::asio::buffer(*ack), *senderEndpoint_,
                [this, ack](const boost::system::error_code& error, std::size_t sent) {
                    if (error && error != boost::asio::error::operation_aborted) {
                        std::cerr << "[Network] Error sending ACK: " << error.message() 
                                  << " with error code: " << error.value() << std::endl;
                    }
                }
            );

            // Extract wintun packet
            std::vector<uint8_t> tunPacket(msgLen);
            std::memcpy(tunPacket.data(), buffer.data() + 16, msgLen);
            
            // Process message on the IO thread to avoid potential deadlocks
            auto sender_copy = *senderEndpoint_;
            boost::asio::post([this, tunPacket, sender_copy]() {
                this->processMessage(std::move(tunPacket), sender_copy);
            });
            break;
        }
        
        case PacketType::ACK: {
            // Remove from pending acks
            std::lock_guard<std::mutex> lock(pending_acks_mutex_);
            pending_acks_.erase(seq);
            break;
        }
        
        default:
            NETWORK_LOG_ERROR("[Network] Unknown packet type: {}", static_cast<int>(packetType));
            break;
    }
}

void UDPNetwork::handleDisconnect() {
    if (!peer_info_->isConnected()) return;
    
    peer_info_->setConnected(false);
    // state_manager_->setState(SystemState::IDLE);
}