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
    std::shared_ptr<SystemStateManager> state_manager) 
    : running_(false)
    , local_port_(0)
    , next_sequence_number_(0)
    , socket_(std::move(socket))
    , io_context_(context)
    , state_manager_(state_manager)
    , keepAliveTimer(io_context_)
{
}

UDPNetwork::~UDPNetwork() {
    shutdown();
}

bool UDPNetwork::startListening(int port) {
    try {
        if (io_context_.stopped())
        {
            NETWORK_LOG_INFO("[Network] IO context is stopped, restarting...");
            io_context_.restart();
        }
        // Get local endpoint information
        boost::asio::ip::udp::endpoint local_endpoint = socket_->local_endpoint();
        local_address_ = local_endpoint.address().to_string();
        local_port_ = local_endpoint.port();
        
        // Increase socket buffer sizes for high-throughput scenarios
        boost::asio::socket_base::send_buffer_size sendBufferOption(4 * 1024 * 1024); // 4MB
        boost::asio::socket_base::receive_buffer_size recvBufferOption(4 * 1024 * 1024); // 4MB
        socket_->set_option(sendBufferOption);
        socket_->set_option(recvBufferOption);
        
        // Set running flag to true
        running_ = true;

        // TODO: REMOVE WORK GUARD FUNCTIONALITY IN CASE CONNECTIO PROBLEMS STILL ARISE
        NETWORK_LOG_INFO("[Network] Creating work guard");
        // if (!workGuard)
        //     workGuard.emplace(boost::asio::make_work_guard(io_context_));
        NETWORK_LOG_INFO("[Network] Work guard created successfully");
        
        // Start async receiving
        NETWORK_LOG_INFO("[Network] Starting async receive");
        startAsyncReceive();
        NETWORK_LOG_INFO("[Network] Async receive started");
        
        // Start IO thread to handle asynchronous operations
        if (!io_thread_.joinable()) {
            NETWORK_LOG_INFO("[Network] Starting IOContext thread");
            io_thread_ = std::thread([this]() {
                #ifdef _WIN32
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
                #endif
                try {
                    NETWORK_LOG_INFO("[Network] IO thread started, running io context");
                    // This will keep running tasks until the work guard is reset / destroyed
                    size_t handlers_run = io_context_.run();
                    NETWORK_LOG_INFO("[Network] IO context finished running, {} handlers executed", handlers_run);
                } catch (const std::exception& e) {
                    std::cerr << "[Network] IO thread error: " << e.what() << std::endl;
                    NETWORK_LOG_ERROR("[Network] IO thread error: {}", e.what());
                }
            NETWORK_LOG_WARNING("[Network] IO thread finished running, shutting down");
            });
        }
        
        SYSTEM_LOG_INFO("[Network] Listening on UDP {}:{}", local_address_, local_port_);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Network] Failed to start UDP listener: " << e.what() << std::endl;
        return false;
    }
}

bool UDPNetwork::connectToPeer(const std::string& ip, int port)
{
    if (peer_connection_.isConnected())
    {
        std::cout << "[Network] Already connected to a peer." << std::endl;
        return false;
    }
    
    try
    {
        boost::asio::ip::address addr = boost::asio::ip::make_address(ip);
        peer_endpoint_ = boost::asio::ip::udp::endpoint(addr, port);
        current_peer_endpoint_ = ip + ":" + std::to_string(port);

        // if (!workGuard)
        // {
        //     NETWORK_LOG_INFO("[Network] Work guard not initialized, creating work guard");
        //     workGuard.emplace(boost::asio::make_work_guard(io_context_));
        //     NETWORK_LOG_INFO("[Network] Work guard created successfully");
        // }

        NETWORK_LOG_INFO("[Network] Starting UDP hole punching to {}:{}", ip, port);
        running_ = true;
        
        // Update system state
        state_manager_->setState(SystemState::CONNECTING);
        
        // Start the hole punching process
        startHolePunchingProcess(peer_endpoint_);
        
        // NOTE: Don't mark as connected here - wait for first valid packet from peer
        // This will be done in processReceivedData when we receive the first packet
        
        return true;
    } catch (const std::exception& e)
    {
        NETWORK_LOG_ERROR("[Network] Connect error: {}", e.what());
        return false;
    }
}

void UDPNetwork::startHolePunchingProcess(const boost::asio::ip::udp::endpoint& peer_endpoint) {
    // Send initial hole punching packets
    for (int i = 0; i < 5; i++) {
        sendHolePunchPacket();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    startKeepAliveTimer();
}

void UDPNetwork::sendHolePunchPacket() {
    try {
        NETWORK_LOG_INFO("[Network] Sending hole-punch / keep-alive packet to peer: {}", peer_endpoint_.address().to_string());
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
                    NETWORK_LOG_ERROR("[Network] Error sending hole-punch packet: {}, with error code: {}", error.message(), error.value());
                }
            }
        );
    } catch (const std::exception& e) {
        SYSTEM_LOG_ERROR("[Network] Error preparing hole-punch packet: {}", e.what());
        NETWORK_LOG_ERROR("[Network] Error preparing hole-punch packet: {}", e.what());
    }
}

void UDPNetwork::checkAllConnections() {
    if (peer_connection_.hasTimedOut(10)) {
        auto last_activity = peer_connection_.getLastActivity();
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_activity).count();
        
        SYSTEM_LOG_ERROR("[Network] Connection timeout. No packets received for {} seconds (threshold: 10s).", elapsed);
        NETWORK_LOG_ERROR("[Network] Connection timeout. No packets received for {} seconds (threshold: 10s).", elapsed);
        
        // Mark as disconnected
        peer_connection_.setConnected(false);
        
        // Notify ALL_PEERS_DISCONNECTED for single peer setup
        notifyConnectionEvent(NetworkEvent::ALL_PEERS_DISCONNECTED);
        
        // TODO: Comment out when implementing multi-peer
        // if (!hasActiveConnections()) {
        //     notifyConnectionEvent(NetworkEvent::ALL_PEERS_DISCONNECTED);
        // }
    }
}

void UDPNetwork::notifyConnectionEvent(NetworkEvent event, const std::string& endpoint) {
    SYSTEM_LOG_INFO("[Network] Shouldn't be here often");
    if (endpoint.empty()) {
        state_manager_->queueEvent(NetworkEventData(event));
    } else {
        state_manager_->queueEvent(NetworkEventData(event, endpoint));
    }
}

// New implementation: Stop connection but keep network stack running
void UDPNetwork::stopConnection() {
    // Send disconnect notification to peer
    sendDisconnectNotification();

    peer_connection_.setConnected(false);
    running_ = false;

    stopKeepAliveTimer();
    // if (workGuard) {
    //     workGuard->reset();
    // } else {
    //     NETWORK_LOG_ERROR("[Network] Work guard not initialized");
    // }
    
    state_manager_->setState(SystemState::IDLE);
    
    SYSTEM_LOG_INFO("[Network] Stopped connection to peer");
    NETWORK_LOG_INFO("[Network] Stopped connection to peer");
}

void UDPNetwork::shutdown() {
    // First stop any active connection
    if (peer_connection_.isConnected()) {
        stopConnection();
    }
    
    // Then shut down the network stack
    running_ = false;
    peer_connection_.setConnected(false);
    state_manager_->setState(SystemState::SHUTTING_DOWN);

    stopKeepAliveTimer();
    // if (workGuard) {
    //     workGuard->reset();
    // }

    if (socket_)
    {
        boost::system::error_code ec;
        socket_->cancel(ec);
        socket_->close(ec);
    }
    
    // Stop io_context 
    io_context_.stop();

    if (io_thread_.joinable())
        io_thread_.join();
    
    SYSTEM_LOG_INFO("[Network] Network subsystem shut down");
}

void UDPNetwork::sendDisconnectNotification() {
    try {
        if (!peer_connection_.isConnected() || !socket_) {
            return; // No connection to notify
        }

        SYSTEM_LOG_INFO("[Network] Sending disconnect notification to peer");
        NETWORK_LOG_INFO("[Network] Sending disconnect notification to peer");
        
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
        SYSTEM_LOG_ERROR("[Network] Error sending disconnect notification: {}", e.what());
        NETWORK_LOG_ERROR("[Network] Error sending disconnect notification: {}", e.what());
    }
}

bool UDPNetwork::isConnected() const {
    return peer_connection_.isConnected();
}

bool UDPNetwork::sendMessage(const std::vector<uint8_t>& dataToSend) {
    if (!running_ || !socket_) {
        SYSTEM_LOG_ERROR("[Network] Cannot send message: socket not available or system not running (disconnected)");
        NETWORK_LOG_ERROR("[Network] Cannot send message: socket not available or system not running (disconnected)");
        return false;
    }
    
    try {
        // Calculate total packet size: header (16 bytes) + message
        size_t packetSize = 16 + dataToSend.size();
        if (packetSize > MAX_PACKET_SIZE) {
            NETWORK_LOG_ERROR("[Network] Message too large, max size is {}", (MAX_PACKET_SIZE - 16));
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
        SYSTEM_LOG_ERROR("[Network] Send preparation error: {}", e.what());
        NETWORK_LOG_ERROR("[Network] Send preparation error: {}", e.what());
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
            NETWORK_LOG_INFO("[Network] Send buffer full, retrying after delay");
            
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
            SYSTEM_LOG_ERROR("[Network] Send error: {}, with error code: {}", error.message(), error.value());
            NETWORK_LOG_ERROR("[Network] Send error: {}, with error code: {}", error.message(), error.value());
            
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
    if (!socket_) {
        NETWORK_LOG_ERROR("[Network] startAsyncReceive: socket is null!");
        return;
    }
    
    if (!socket_->is_open()) {
        NETWORK_LOG_ERROR("[Network] startAsyncReceive: socket is not open!");
        return;
    }
    
    // Create NEW buffer for each receive operation to avoid race conditions
    auto receiveBuffer = std::make_shared<std::vector<uint8_t>>(MAX_PACKET_SIZE);
    auto senderEndpoint = std::make_shared<boost::asio::ip::udp::endpoint>();
    
    socket_->async_receive_from(
        boost::asio::buffer(*receiveBuffer), *senderEndpoint,
        [this, receiveBuffer, senderEndpoint](const boost::system::error_code& error, std::size_t bytes_transferred) {
            this->handleReceiveFrom(error, bytes_transferred, receiveBuffer, senderEndpoint);
        }
    );
}

void UDPNetwork::handleReceiveFrom(const boost::system::error_code& error, std::size_t bytes_transferred, std::shared_ptr<std::vector<uint8_t>> receiveBuffer, std::shared_ptr<boost::asio::ip::udp::endpoint> senderEndpoint) {
    // if (!running_) {
    //     NETWORK_LOG_ERROR("[Network] Received data from peer, but network not running");
    //     startAsyncReceive(); // "Consume" packet and queue up another startAsyncReceive
    //     return;
    // }
    
    // if (!error) {
    //     // Process the received data
    //     processReceivedData(bytes_transferred, receiveBuffer, senderEndpoint);
        
    //     // Queue up another receive operation immediately
    //     startAsyncReceive();
    // }
    // else if (error != boost::asio::error::operation_aborted) {
    //     // Handle error but don't terminate unless it's fatal
    //     NETWORK_LOG_ERROR("[Network] Receive error: {}, with error code: {}", error.message(), error.value());
        
    //     // For recoverable errors, try again after short delay
    //     if (error == boost::asio::error::would_block || 
    //         error == boost::asio::error::try_again) {
    //         boost::asio::post(io_context_, [this]() { startAsyncReceive(); });
    //     }
    //     else {
    //         // For fatal errors, handle disconnect
    //         handleDisconnect();
    //     }
    // }
    // UNCOMMENT IN CASE THIS BREAKS!
    if (socket_ && socket_->is_open() && error != boost::asio::error::operation_aborted)
    {
        startAsyncReceive(); // Continuously queue up another startAsyncReceive
    }

    if (!error)
    {
        processReceivedData(bytes_transferred, receiveBuffer, senderEndpoint);
    }
    else if (error != boost::asio::error::operation_aborted)
    {
        // Handle error but don't terminate unless it's fatal
        NETWORK_LOG_ERROR("[Network] Receive error: {} (code: {})", error.message(), error.value());
        
        if (error == boost::asio::error::would_block || 
            error == boost::asio::error::try_again)
        {
            // Recoverable errors
            NETWORK_LOG_WARNING("[Network] Recoverable receive error: {} (code: {}), continuing", error.message(), error.value());
        }
        else
        {
            // Fatal errors
            NETWORK_LOG_ERROR("[Network] Fatal receive error: {} (code: {}), disconnecting", error.message(), error.value());
            handleDisconnect();
        }
    }
}

void UDPNetwork::processReceivedData(std::size_t bytes_transferred, std::shared_ptr<std::vector<uint8_t>> receiveBuffer, std::shared_ptr<boost::asio::ip::udp::endpoint> senderEndpoint) {
    // Skip if we don't have enough data for header
    if (bytes_transferred < 16) {
        NETWORK_LOG_ERROR("[Network] Received packet too small: {} bytes", bytes_transferred);
        return;
    }
    
    const std::vector<uint8_t>& buffer = *receiveBuffer;
    
    // Validate magic number
    uint32_t magic = (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
    if (magic != MAGIC_NUMBER) {
        NETWORK_LOG_WARNING("[Network] Received packet with invalid magic number: {}", magic);
        return;
    }
    
    // Validate protocol version
    uint16_t version = (buffer[4] << 8) | buffer[5];
    if (version != PROTOCOL_VERSION) {
        NETWORK_LOG_ERROR("[Network] Unsupported protocol version: {}", version);
        return;
    }
    
    // Get packet type
    PacketType packetType = static_cast<PacketType>(buffer[6]);
    
    // Get sequence number
    uint32_t seq = (buffer[8] << 24) | (buffer[9] << 16) | (buffer[10] << 8) | buffer[11];
    
    // Update peer activity time
    peer_connection_.updateActivity();

    if (packetType != PacketType::DISCONNECT)
    {
        // Consume packet if network not running
        if (!running_)
        {
            NETWORK_LOG_ERROR("[Network] Received packet, but network not running");
            return;
        }

        // Store peer endpoint if not already connected
        if (!peer_connection_.isConnected())
        {
            NETWORK_LOG_INFO("[Network] First valid packet received from peer, establishing connection");
            peer_endpoint_ = *senderEndpoint;
            current_peer_endpoint_ = senderEndpoint->address().to_string() + ":" + std::to_string(senderEndpoint->port());
            peer_connection_.setConnected(true);
            
            // Notify peer connected event
            notifyConnectionEvent(NetworkEvent::PEER_CONNECTED, current_peer_endpoint_);
        }
    }

    
    // Process packet based on type
    switch (packetType) {
        
        case PacketType::HOLE_PUNCH:
            NETWORK_LOG_INFO("[Network] Received hole-punch packet from peer");
            // Activity time was already updated above
            break;
            
        case PacketType::HEARTBEAT:
            NETWORK_LOG_INFO("[Network] Received heartbeat packet from peer");
            // Activity time was already updated above
            break;
            
        case PacketType::DISCONNECT:
            // Peer wants to disconnect
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
                boost::asio::buffer(*ack), *senderEndpoint,
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
            
            // Process message, send to wintun interface
            auto sender_copy = *senderEndpoint;
            // boost::asio::post(io_context_, [this, tunPacket, sender_copy]() {
            //     this->processMessage(std::move(tunPacket), sender_copy);
            // }); // UNCOMMENT IN CASE THIS BREAKS!
            this->processMessage(std::move(tunPacket), sender_copy);
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
    if (!peer_connection_.isConnected()) return;
    
    peer_connection_.setConnected(false);
    
    // Notify ALL_PEERS_DISCONNECTED for single peer setup
    notifyConnectionEvent(NetworkEvent::ALL_PEERS_DISCONNECTED);
}

void UDPNetwork::startKeepAliveTimer() {
    if (!running_) return;

    keepAliveTimer.expires_after(std::chrono::seconds(3));
    keepAliveTimer.async_wait([this](const boost::system::error_code& error) {
        handleKeepAlive(error);
    });
}

void UDPNetwork::stopKeepAliveTimer() {
    try {
        NETWORK_LOG_INFO("[Network] Stopping keep-alive timer");
        keepAliveTimer.cancel();
    } catch (const boost::system::system_error& e) {
        NETWORK_LOG_ERROR("[Network] Error cancelling keep-alive timer: {}", e.what());
    }
}

void UDPNetwork::handleKeepAlive(const boost::system::error_code& error) {
    if (error == boost::asio::error::operation_aborted) {
        NETWORK_LOG_INFO("[Network] Keep-alive timer cancelled");
        return;
    }

    if (!running_) {
        NETWORK_LOG_INFO("[Network] Network not running, skipping keep-alive");
        return;
    }

    NETWORK_LOG_INFO("[Network] Running keep-alive functionality");
    sendHolePunchPacket(); // Send hole punch packet
    if (peer_connection_.isConnected()) {
        checkAllConnections(); // Check connection status
    }

    startKeepAliveTimer(); // Restart timer
}