#include "networking.hpp"
#include <iostream>
#include <chrono>
#include <random>
#include <cstring>

UDPNetwork::UDPNetwork(
    std::unique_ptr<boost::asio::ip::udp::socket> socket,
    boost::asio::io_context& context) 
    : running_(false), connected_(false), local_port_(0), next_sequence_number_(0)
    , socket_(std::move(socket)), io_context_(context)
{
}

UDPNetwork::~UDPNetwork() {
    disconnect();
}

bool UDPNetwork::startListening(int port) {
    try {
        // Get local endpoint information
        boost::asio::ip::udp::endpoint local_endpoint = socket_->local_endpoint();
        local_address_ = local_endpoint.address().to_string();
        local_port_ = local_endpoint.port();
        socket_->non_blocking(false);
        
        // Start IO context in a separate thread
        running_ = true;
        
        // Start receive loop
        receive_thread_ = std::thread([this]() {
            this->receiveLoop();
        });
        
        std::cout << "[Network] Listening on UDP " << local_address_ << ":" << local_port_ << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Network] Failed to start UDP listener: " << e.what() << std::endl;
        return false;
    }
}

bool UDPNetwork::connectToPeer(const std::string& ip, int port) {
    if (connected_) {
        std::cout << "[Network] Already connected to a peer." << std::endl;
        return false;
    }
    
    try {
        // Set up peer endpoint
        peer_endpoint_ = boost::asio::ip::udp::endpoint(
            boost::asio::ip::make_address(ip), port);
        
        std::cout << "[Network] Starting UDP hole punching to " << ip << ":" << port << "..." << std::endl;
        
        // Start the hole punching process
        startHolePunchingProcess(peer_endpoint_);
        
        // Mark as connected and start heartbeat thread
        connected_ = true;
        last_received_time_ = std::chrono::steady_clock::now();
        
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
        while (running_ && connected_) {
            // Send hole punch packet
            if (connected_) {
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
        std::lock_guard<std::mutex> lock(send_mutex_);
        
        // Create hole-punch packet
        std::vector<uint8_t> packet(16);
        
        // Set magic number
        packet[0] = (MAGIC_NUMBER >> 24) & 0xFF;
        packet[1] = (MAGIC_NUMBER >> 16) & 0xFF;
        packet[2] = (MAGIC_NUMBER >> 8) & 0xFF;
        packet[3] = MAGIC_NUMBER & 0xFF;
        
        // Set protocol version
        packet[4] = (PROTOCOL_VERSION >> 8) & 0xFF;
        packet[5] = PROTOCOL_VERSION & 0xFF;
        
        // Set packet type
        packet[6] = static_cast<uint8_t>(PacketType::HOLE_PUNCH);
        
        // Set sequence number
        uint32_t seq = next_sequence_number_++;
        packet[8] = (seq >> 24) & 0xFF;
        packet[9] = (seq >> 16) & 0xFF;
        packet[10] = (seq >> 8) & 0xFF;
        packet[11] = seq & 0xFF;
        
        // Send packet
        socket_->send_to(boost::asio::buffer(packet), peer_endpoint_);
    } catch (const std::exception& e) {
        std::cerr << "[Network] Error sending hole-punch packet: " << e.what() << std::endl;
    }
}

void UDPNetwork::checkConnectionStatus() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_received_time_).count();
    
    // If we haven't received anything in 10 seconds, consider the connection lost
    if (elapsed > 10 && connected_) {
        std::cout << "[Network] Connection timeout. No packets received for " << elapsed << " seconds." << std::endl;
        
        // Mark as disconnected
        connected_ = false;
        
        // Notify about connection state change
        if (on_connection_) {
            on_connection_(false);
        }
    }
}

void UDPNetwork::disconnect() {
    running_ = false;
    
    if (socket_) {
        try {
            socket_->close();
        } catch (...) {}
        socket_.reset();
    }
    
    if (connected_) {
        connected_ = false;
        
        // Notify
        if (on_connection_) {
            on_connection_(false);
        }
    }
    
    // Wait for threads to finish
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }
    
    io_context_.stop();
}

bool UDPNetwork::isConnected() const {
    return connected_;
}

bool UDPNetwork::sendMessage(const std::string& message) {
    if (!connected_ || !socket_) {
        std::cerr << "[Network] Cannot send message: not connected" << std::endl;
        return false;
    }
    
    try {
        std::lock_guard<std::mutex> lock(send_mutex_);
        
        // Calculate total packet size: header (16 bytes) + message
        size_t packetSize = 16 + message.size();
        if (packetSize > MAX_PACKET_SIZE) {
            std::cerr << "[Network] Message too large, max size is " << (MAX_PACKET_SIZE - 16) << " bytes" << std::endl;
            return false;
        }
        
        // Create packet
        std::vector<uint8_t> packet(packetSize);
        
        // Set magic number
        packet[0] = (MAGIC_NUMBER >> 24) & 0xFF;
        packet[1] = (MAGIC_NUMBER >> 16) & 0xFF;
        packet[2] = (MAGIC_NUMBER >> 8) & 0xFF;
        packet[3] = MAGIC_NUMBER & 0xFF;
        
        // Set protocol version
        packet[4] = (PROTOCOL_VERSION >> 8) & 0xFF;
        packet[5] = PROTOCOL_VERSION & 0xFF;
        
        // Set packet type
        packet[6] = static_cast<uint8_t>(PacketType::MESSAGE);
        
        // Set sequence number
        uint32_t seq = next_sequence_number_++;
        packet[8] = (seq >> 24) & 0xFF;
        packet[9] = (seq >> 16) & 0xFF;
        packet[10] = (seq >> 8) & 0xFF;
        packet[11] = seq & 0xFF;
        
        // Set message length
        uint32_t msg_len = static_cast<uint32_t>(message.size());
        packet[12] = (msg_len >> 24) & 0xFF;
        packet[13] = (msg_len >> 16) & 0xFF;
        packet[14] = (msg_len >> 8) & 0xFF;
        packet[15] = msg_len & 0xFF;
        
        // Copy message content
        std::memcpy(packet.data() + 16, message.data(), message.size());
        
        // Send packet
        socket_->send_to(boost::asio::buffer(packet), peer_endpoint_);
        
        // Track for acknowledgment
        {
            std::lock_guard<std::mutex> ack_lock(pending_acks_mutex_);
            pending_acks_[seq] = std::chrono::steady_clock::now();
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Network] Send error: " << e.what() << std::endl;
        connected_ = false;
        if (on_connection_) {
            on_connection_(false);
        }
        return false;
    }
}

void UDPNetwork::receiveLoop() {
    std::vector<uint8_t> buffer(MAX_PACKET_SIZE);
    
    while (running_) {
        try {
            boost::asio::ip::udp::endpoint sender_endpoint;
            size_t length = 0;
            
            // Set socket to non-blocking mode
            socket_->non_blocking(true);
            
            // Try to receive data
            boost::system::error_code error;
            length = socket_->receive_from(boost::asio::buffer(buffer), sender_endpoint, 0, error);

            // If no data or would block, sleep a bit and try again
            if (error == boost::asio::error::would_block) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            
            // Handle other errors
            if (error) {
                if (error != boost::asio::error::operation_aborted) {
                    std::cerr << "[Network] Receive error: " << error.message() << std::endl;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            
            // Minimum packet size
            if (length < 16) {
                std::cerr << "[Network] Received packet too small: " << length << " bytes" << std::endl;
                continue;
            }
            
            // Validate magic number
            uint32_t magic = (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
            if (magic != MAGIC_NUMBER) {
                continue; // Silent ignore, probably not our protocol
            }
            
            // Validate protocol version
            uint16_t version = (buffer[4] << 8) | buffer[5];
            if (version != PROTOCOL_VERSION) {
                std::cerr << "[Network] Unsupported protocol version: " << version << std::endl;
                continue;
            }
            
            // Get packet type
            PacketType packetType = static_cast<PacketType>(buffer[6]);
            
            // Get sequence number
            uint32_t seq = (buffer[8] << 24) | (buffer[9] << 16) | (buffer[10] << 8) | buffer[11];
            
            // Update last received time
            last_received_time_ = std::chrono::steady_clock::now();
            
            // Store peer endpoint if not already connected
            if (!connected_) {
                peer_endpoint_ = sender_endpoint;
                connected_ = true;
                if (on_connection_) {
                    on_connection_(true);
                }
            }
            
            // Process packet based on type
            switch (packetType) {
                case PacketType::HOLE_PUNCH:
                    // Update last received time, which was done above
                    break;
                    
                case PacketType::HEARTBEAT:
                    // Done by keep-alive thread
                    break;
                    
                case PacketType::MESSAGE: {
                    // Get message length
                    uint32_t msgLen = (buffer[12] << 24) | (buffer[13] << 16) | (buffer[14] << 8) | buffer[15];
                    
                    // Validate message length
                    if (16 + msgLen > length) {
                        std::cerr << "[Network] Message length exceeds packet size" << std::endl;
                        continue;
                    }
                    
                    // Extract message
                    std::string message(reinterpret_cast<char*>(buffer.data() + 16), msgLen);
                    
                    // Send ACK
                    try {
                        std::lock_guard<std::mutex> lock(send_mutex_);
                        std::vector<uint8_t> ack(16);
                        
                        // Set magic number
                        ack[0] = (MAGIC_NUMBER >> 24) & 0xFF;
                        ack[1] = (MAGIC_NUMBER >> 16) & 0xFF;
                        ack[2] = (MAGIC_NUMBER >> 8) & 0xFF;
                        ack[3] = MAGIC_NUMBER & 0xFF;
                        
                        // Set protocol version
                        ack[4] = (PROTOCOL_VERSION >> 8) & 0xFF;
                        ack[5] = PROTOCOL_VERSION & 0xFF;
                        
                        // Set packet type
                        ack[6] = static_cast<uint8_t>(PacketType::ACK);
                        
                        // Set sequence number (same as received)
                        ack[8] = (seq >> 24) & 0xFF;
                        ack[9] = (seq >> 16) & 0xFF;
                        ack[10] = (seq >> 8) & 0xFF;
                        ack[11] = seq & 0xFF;
                        
                        // Send ACK
                        socket_->send_to(boost::asio::buffer(ack), sender_endpoint);
                    } catch (const std::exception& e) {
                        std::cerr << "[Network] Error sending ACK: " << e.what() << std::endl;
                    }
                    
                    // Process message
                    processMessage(message, sender_endpoint);
                    break;
                }
                
                case PacketType::ACK: {
                    // Remove from pending acks
                    std::lock_guard<std::mutex> lock(pending_acks_mutex_);
                    pending_acks_.erase(seq);
                    break;
                }
                
                default:
                    std::cerr << "[Network] Unknown packet type: " << static_cast<int>(packetType) << std::endl;
                    break;
            }
            
        } catch (const std::exception& e) {
            if (running_) {
                std::cerr << "[Network] Receive loop error: " << e.what() << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
}

void UDPNetwork::processMessage(const std::string& message, const boost::asio::ip::udp::endpoint& sender) {
    if (on_message_) {
        on_message_(message);
    }
}

void UDPNetwork::setMessageCallback(MessageCallback callback) {
    on_message_ = std::move(callback);
}

void UDPNetwork::setConnectionCallback(ConnectionCallback callback) {
    on_connection_ = std::move(callback);
}

int UDPNetwork::getLocalPort() const {
    return local_port_;
}

std::string UDPNetwork::getLocalAddress() const {
    return local_address_;
}