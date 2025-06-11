#include "NetworkingModule.hpp"
#include "Logger.hpp"
#include <iostream>
#include <chrono>
#include <random>
#include <cstring>
#include <boost/asio/ip/address_v6.hpp>

UDPNetwork::UDPNetwork(
    std::unique_ptr<boost::asio::ip::udp::socket> socket,
    boost::asio::io_context& context,
    std::shared_ptr<SystemStateManager> state_manager) 
    : running(false)
    , localPort(0)
    , nextSeqNumber(0)
    , socket(std::move(socket))
    , ioContext(context)
    , stateManager(state_manager)
    , keepAliveTimer(ioContext)
{
}

UDPNetwork::~UDPNetwork()
{
    shutdown();
}

bool UDPNetwork::startListening(int port)
{
    try
    {
        if (ioContext.stopped())
        {
            NETWORK_LOG_INFO("[Network] IO context is stopped, restarting...");
            ioContext.restart();
        }
        // Get local endpoint information
        boost::asio::ip::udp::endpoint local_endpoint = socket->local_endpoint();
        localAddress = local_endpoint.address().to_string();
        localPort = local_endpoint.port();
        
        // Increase socket buffer sizes for high-throughput scenarios
        boost::asio::socket_base::send_buffer_size sendBufferOption(4 * 1024 * 1024); // 4MB
        boost::asio::socket_base::receive_buffer_size recvBufferOption(4 * 1024 * 1024); // 4MB
        socket->set_option(sendBufferOption);
        socket->set_option(recvBufferOption);

        // Set running flag to true
        running = true;

        // Start async receiving
        NETWORK_LOG_INFO("[Network] Starting async receive");
        startAsyncReceive();
        NETWORK_LOG_INFO("[Network] Async receive started");
        
        // Start IO thread to handle asynchronous operations
        if (!ioThread.joinable())
        {
            NETWORK_LOG_INFO("[Network] Starting IOContext thread");
            ioThread = std::thread([this]()
            {
                // Set thread priority to time-critical
                #ifdef _WIN32
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
                #endif
                try
                {
                    NETWORK_LOG_INFO("[Network] IO thread started, running io context");
                    // This will keep running tasks until the work guard is reset / destroyed
                    size_t handlers_run = ioContext.run();
                    NETWORK_LOG_INFO("[Network] IO context finished running, {} handlers executed", handlers_run);
                }
                catch (const std::exception& e)
                {
                    NETWORK_LOG_ERROR("[Network] IO thread error: {}", e.what());
                }
                NETWORK_LOG_WARNING("[Network] IO thread finished running, shutting down");
            });
        }

        SYSTEM_LOG_INFO("[Network] Listening on UDP {}:{}", localAddress, localPort);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Network] Failed to start UDP listener: " << e.what() << std::endl;
        return false;
    }
}

bool UDPNetwork::connectToPeer(const std::string& ip, int port)
{
    if (peerConnection.isConnected())
    {
        std::cout << "[Network] Already connected to a peer." << std::endl;
        return false;
    }
    
    try
    {
        boost::asio::ip::address addr = boost::asio::ip::make_address(ip);
        peerEndpoint = boost::asio::ip::udp::endpoint(addr, port);
        currentPeerEndpoint = ip + ":" + std::to_string(port);

        NETWORK_LOG_INFO("[Network] Starting UDP hole punching to {}:{}", ip, port);
        running = true;
        
        // Update system state
        stateManager->setState(SystemState::CONNECTING);
        
        // Start the hole punching process
        startHolePunchingProcess(peerEndpoint);
        
        return true;
    } catch (const std::exception& e)
    {
        NETWORK_LOG_ERROR("[Network] Connect error: {}", e.what());
        return false;
    }
}

void UDPNetwork::startHolePunchingProcess(const boost::asio::ip::udp::endpoint& peer_endpoint)
{
    // Send initial hole punching packets
    for (int i = 0; i < 5; i++) {
        sendHolePunchPacket();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Start keep-alive timer
    startKeepAliveTimer();
}

void UDPNetwork::sendHolePunchPacket()
{
    try
    {
        NETWORK_LOG_INFO("[Network] Sending hole-punch / keep-alive packet to peer: {}", peerEndpoint.address().to_string());
        // Create hole-punch packet with shared ownership
        auto packet = std::make_shared<std::vector<uint8_t>>(16);

        // Attach custom header
        attachCustomHeader(packet, PacketType::HOLE_PUNCH);
        
        // Send packet asynchronously
        // TODO: REFACTOR FOR *1, FOR MULTIPLE PEERS
        socket->async_send_to(
            boost::asio::buffer(*packet), peerEndpoint,
            [packet](const boost::system::error_code& error, std::size_t bytesSent)
            {
                if (error && error != boost::asio::error::operation_aborted && 
                    error != boost::asio::error::would_block &&
                    error.value() != 10035) // WSAEWOULDBLOCK
                    {
                    NETWORK_LOG_ERROR("[Network] Error sending hole-punch packet: {}, with error code: {}", error.message(), error.value());
                }
            });
    }
    catch (const std::exception& e)
    {
        SYSTEM_LOG_ERROR("[Network] Error preparing hole-punch packet: {}", e.what());
        NETWORK_LOG_ERROR("[Network] Error preparing hole-punch packet: {}", e.what());
    }
}

void UDPNetwork::checkAllConnections()
{
    if (peerConnection.hasTimedOut(20))
    {
        // Time out peer after 20 seconds of inactivity
        auto last_activity = peerConnection.getLastActivity();
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_activity).count();
        
        SYSTEM_LOG_ERROR("[Network] Connection timeout. No packets received for {} seconds (threshold: 20s).", elapsed);
        NETWORK_LOG_ERROR("[Network] Connection timeout. No packets received for {} seconds (threshold: 20s).", elapsed);
        
        // Mark as disconnected
        peerConnection.setConnected(false);
        
        // Notify ALL_PEERS_DISCONNECTED for single peer setup
        notifyConnectionEvent(NetworkEvent::ALL_PEERS_DISCONNECTED);
        
        // TODO: REFACTOR FOR *1, FOR MULTIPLE PEERS
        // if (!hasActiveConnections()) {
        //     notifyConnectionEvent(NetworkEvent::ALL_PEERS_DISCONNECTED);
        // }
    }
}

void UDPNetwork::notifyConnectionEvent(NetworkEvent event, const std::string& endpoint)
{
    SYSTEM_LOG_INFO("[Network] Queuing network event: {}", static_cast<int>(event));
    if (endpoint.empty())
    {
        stateManager->queueEvent(NetworkEventData(event));
    }
    else
    {
        stateManager->queueEvent(NetworkEventData(event, endpoint));
    }
}

// TODO: REFACTOR FOR *1, FOR MULTIPLE PEERS
void UDPNetwork::stopConnection()
{
    // Send disconnect notification to peer
    sendDisconnectNotification();

    peerConnection.setConnected(false);
    running = false;

    stopKeepAliveTimer();
    
    stateManager->setState(SystemState::IDLE);
    
    SYSTEM_LOG_INFO("[Network] Stopped connection to peer");
    NETWORK_LOG_INFO("[Network] Stopped connection to peer");
}

void UDPNetwork::shutdown()
{
    // Stop any active connection
    // TODO: REFACTOR FOR *1, FOR MULTIPLE PEERS
    if (peerConnection.isConnected()) {
        stopConnection();
    }
    
    // Then shut down the network stack
    running = false;
    peerConnection.setConnected(false);
    stateManager->setState(SystemState::SHUTTING_DOWN);

    stopKeepAliveTimer();

    if (socket)
    {
        boost::system::error_code ec;
        socket->cancel(ec);
        socket->close(ec);
    }
    
    // Stop io_context 
    ioContext.stop();

    if (ioThread.joinable())
        ioThread.join();
    
    SYSTEM_LOG_INFO("[Network] Network subsystem shut down");
}

// TODO: REFACTOR FOR *1, FOR MULTIPLE PEERS
void UDPNetwork::sendDisconnectNotification()
{
    try
    {
        if (!peerConnection.isConnected() || !socket)
        {
            return; // No connection to notify
        }

        SYSTEM_LOG_INFO("[Network] Sending disconnect notification to peer");
        NETWORK_LOG_INFO("[Network] Sending disconnect notification to peer");
        
        // Create disconnect packet
        auto packet = std::make_shared<std::vector<uint8_t>>(16);
        
        // Attach custom header
        attachCustomHeader(packet, PacketType::DISCONNECT);
        
        // Send packet - try multiple times to increase chance of delivery
        for (int i = 0; i < 3; i++)
        {
            socket->async_send_to(
                boost::asio::buffer(*packet), peerEndpoint,
                [packet](const boost::system::error_code& error, std::size_t bytesSent)
                {
                    // Ignore errors since we're disconnecting
                });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    catch (const std::exception& e)
    {
        SYSTEM_LOG_ERROR("[Network] Error sending disconnect notification: {}", e.what());
        NETWORK_LOG_ERROR("[Network] Error sending disconnect notification: {}", e.what());
    }
}

// TODO: REFACTOR FOR *1, FOR MULTIPLE PEERS
bool UDPNetwork::isConnected() const
{
    return peerConnection.isConnected();
}

// TODO: REFACTOR FOR *1, FOR MULTIPLE PEERS
bool UDPNetwork::sendMessage(const std::vector<uint8_t>& dataToSend)
{
    if (!running || !socket)
    {
        SYSTEM_LOG_ERROR("[Network] Cannot send message: socket not available or system not running (disconnected)");
        NETWORK_LOG_ERROR("[Network] Cannot send message: socket not available or system not running (disconnected)");
        return false;
    }
    
    try
    {
        // Calculate total packet size: header (16 bytes) + message
        size_t packetSize = 16 + dataToSend.size();
        if (packetSize > MAX_PACKET_SIZE)
        {
            NETWORK_LOG_ERROR("[Network] Message too large, max size is {}", (MAX_PACKET_SIZE - 16));
            return false;
        }
        
        /*
        * SMALL CUSTOM PROTOCOL HEADER
        */

        // Create packet with shared ownership for async operation
        auto packet = std::make_shared<std::vector<uint8_t>>(packetSize);

        // Attach custom header
        uint32_t seq = attachCustomHeader(packet, PacketType::MESSAGE);
        
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
            std::lock_guard<std::mutex> ack_lock(pendingAcksMutex);
            pendingAcks[seq] = std::chrono::steady_clock::now();
        }
        
        // Send packet asynchronously
        socket->async_send_to(
            boost::asio::buffer(*packet), peerEndpoint,
            [this, packet, seq](const boost::system::error_code& error, std::size_t bytesSent)
            {
                this->handleSendComplete(error, bytesSent, seq);
            });
        
        return true;
    }
    catch (const std::exception& e)
    {
        SYSTEM_LOG_ERROR("[Network] Send preparation error: {}", e.what());
        NETWORK_LOG_ERROR("[Network] Send preparation error: {}", e.what());
        return false;
    }
}

void UDPNetwork::handleSendComplete(
    const boost::system::error_code& error,
    std::size_t bytesSent,
    uint32_t seq)
{
    if (error)
    {
        if (error == boost::asio::error::would_block || 
            error == boost::asio::error::try_again ||
            error.value() == 10035) // WSAEWOULDBLOCK
        {

            NETWORK_LOG_INFO("[Network] Send buffer full");

            boost::asio::post(ioContext, [this, seq]()
            {
                // No resend, just log that we're dropping the packet
                NETWORK_LOG_INFO("[Network] Dropping packet due to send buffer limits: seq={}", seq);
                
                // Remove from pending acks
                std::lock_guard<std::mutex> lock(pendingAcksMutex);
                pendingAcks.erase(seq);
            });
        }
        else
        {
            SYSTEM_LOG_ERROR("[Network] Send error: {}, with error code: {}", error.message(), error.value());
            NETWORK_LOG_ERROR("[Network] Send error: {}, with error code: {}", error.message(), error.value());
            
            // Disconnect on fatal errors, not temporary ones
            if (error != boost::asio::error::operation_aborted)
            {
                boost::asio::post(ioContext, [this]() {this->handleDisconnect(); });
            }
        }
    }
}

void UDPNetwork::processMessage(std::vector<uint8_t> message, const boost::asio::ip::udp::endpoint& sender)
{
    if (onMessageCallback)
    {
        onMessageCallback(std::move(message));
    }
}

void UDPNetwork::setMessageCallback(MessageCallback callback)
{
    onMessageCallback = std::move(callback);
}

int UDPNetwork::getLocalPort() const
{
    return localPort;
}

std::string UDPNetwork::getLocalAddress() const
{
    return localAddress;
}

void UDPNetwork::startAsyncReceive()
{
    if (!socket) {
        NETWORK_LOG_ERROR("[Network] startAsyncReceive: socket is null!");
        return;
    }
    
    if (!socket->is_open()) {
        NETWORK_LOG_ERROR("[Network] startAsyncReceive: socket is not open!");
        return;
    }
    
    // Create buffer for each receive operation
    auto receiveBuffer = std::make_shared<std::vector<uint8_t>>(MAX_PACKET_SIZE);
    auto senderEndpoint = std::make_shared<boost::asio::ip::udp::endpoint>();
    
    socket->async_receive_from(
        boost::asio::buffer(*receiveBuffer), *senderEndpoint,
        [this, receiveBuffer, senderEndpoint](const boost::system::error_code& error, std::size_t bytesTransferred)
        {
            this->handleReceiveFrom(error, bytesTransferred, receiveBuffer, senderEndpoint);
        }
    );
}

void UDPNetwork::handleReceiveFrom(
    const boost::system::error_code& error,
    std::size_t bytesTransferred,
    std::shared_ptr<std::vector<uint8_t>> receiveBuffer,
    std::shared_ptr<boost::asio::ip::udp::endpoint> senderEndpoint)
{
    if (socket && socket->is_open() && error != boost::asio::error::operation_aborted)
    {
        startAsyncReceive(); // Continuously queue up another startAsyncReceive
    }

    if (!error)
    {
        processReceivedData(bytesTransferred, receiveBuffer, senderEndpoint);
    }
    else if (error != boost::asio::error::operation_aborted)
    {
        // Handle error but don't terminate unless it's fatal
        NETWORK_LOG_ERROR("[Network] Receive error: {} (code: {})", error.message(), error.value());
        
        if (error == boost::asio::error::would_block || 
            error == boost::asio::error::try_again ||
            error.value() == 10035) // WSAEWOULDBLOCK
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

void UDPNetwork::processReceivedData(
    std::size_t bytesTransferred,
    std::shared_ptr<std::vector<uint8_t>> receiveBuffer,
    std::shared_ptr<boost::asio::ip::udp::endpoint> senderEndpoint)
{
    // Skip if we don't have enough data for header
    if (bytesTransferred < 16)
    {
        NETWORK_LOG_ERROR("[Network] Received packet too small: {} bytes", bytesTransferred);
        return;
    }
    
    const std::vector<uint8_t>& buffer = *receiveBuffer;

    /*
    * SMALL CUSTOM PROTOCOL HEADER
    */

    // Validate magic number
    uint32_t magic = (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
    if (magic != MAGIC_NUMBER)
    {
        NETWORK_LOG_WARNING("[Network] Received packet with invalid magic number: {}", magic);
        return;
    }
    
    // Validate protocol version
    uint16_t version = (buffer[4] << 8) | buffer[5];
    if (version != PROTOCOL_VERSION)
    {
        NETWORK_LOG_ERROR("[Network] Unsupported protocol version: {}", version);
        return;
    }
    
    // Get packet type
    PacketType packetType = static_cast<PacketType>(buffer[6]);
    
    // Get sequence number
    uint32_t seq = (buffer[8] << 24) | (buffer[9] << 16) | (buffer[10] << 8) | buffer[11];
    
    // Update peer activity time
    peerConnection.updateActivity();

    // This could probablt be structured better, lol
    if (packetType != PacketType::DISCONNECT)
    {
        // Consume packet if network not running
        if (!running)
        {
            NETWORK_LOG_ERROR("[Network] Received packet, but network not running");
            return;
        }

        // Store peer endpoint if not already connected
        if (!peerConnection.isConnected())
        {
            NETWORK_LOG_INFO("[Network] First valid packet received from peer, establishing connection");
            peerEndpoint = *senderEndpoint;
            currentPeerEndpoint = senderEndpoint->address().to_string() + ":" + std::to_string(senderEndpoint->port());
            peerConnection.setConnected(true);
            
            // Notify peer connected event
            notifyConnectionEvent(NetworkEvent::PEER_CONNECTED, currentPeerEndpoint);
        }
    }

    // Process packet based on type
    switch (packetType)
    {
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
            
        case PacketType::MESSAGE:
        {
            // Get message length
            uint32_t msgLen = (buffer[12] << 24) | (buffer[13] << 16) | (buffer[14] << 8) | buffer[15];
            
            // Validate message length
            if (16 + msgLen > bytesTransferred)
            {
                NETWORK_LOG_ERROR("[Network] Message length exceeds packet size");
                return;
            }
            
            // Create ACK packet
            auto ack = std::make_shared<std::vector<uint8_t>>(16);

            // Attach custom header
            attachCustomHeader(ack, PacketType::ACK, std::make_optional(seq));
            
            // Send ACK
            socket->async_send_to(
                boost::asio::buffer(*ack), *senderEndpoint,
                [this, ack](const boost::system::error_code& error, std::size_t sent)
                {
                    if (error && error != boost::asio::error::operation_aborted)
                    {
                        NETWORK_LOG_ERROR("[Network] Error sending ACK: {} (code: {})", error.message(), error.value());
                    }
                });

            // Extract wintun packet
            std::vector<uint8_t> tunPacket(msgLen);
            std::memcpy(tunPacket.data(), buffer.data() + 16, msgLen);
            
            // Process message, send to wintun interface
            auto sender_copy = *senderEndpoint;
            // Revert to boost::asio::post in case the following breaks the program
            this->processMessage(std::move(tunPacket), sender_copy);
            break;
        }
        case PacketType::ACK:
        {
            // Remove from pending acks
            std::lock_guard<std::mutex> lock(pendingAcksMutex);
            pendingAcks.erase(seq);
            break;
        }
        default:
            NETWORK_LOG_ERROR("[Network] Unknown packet type: {}", static_cast<int>(packetType));
            break;
    }
}

// TODO: REFACTOR FOR *1, FOR MULTIPLE PEERS
void UDPNetwork::handleDisconnect()
{
    if (!peerConnection.isConnected()) return;
    
    peerConnection.setConnected(false);
    
    // Notify ALL_PEERS_DISCONNECTED for single peer setup
    notifyConnectionEvent(NetworkEvent::ALL_PEERS_DISCONNECTED);
}

void UDPNetwork::startKeepAliveTimer()
{
    if (!running) return;

    keepAliveTimer.expires_after(std::chrono::seconds(3));
    keepAliveTimer.async_wait([this](const boost::system::error_code& error)
    {
        handleKeepAlive(error);
    });
}

void UDPNetwork::stopKeepAliveTimer()
{
    try
    {
        NETWORK_LOG_INFO("[Network] Stopping keep-alive timer");
        keepAliveTimer.cancel();
    }
    catch (const boost::system::system_error& e)
    {
        NETWORK_LOG_ERROR("[Network] Error cancelling keep-alive timer: {}", e.what());
    }
}

void UDPNetwork::handleKeepAlive(const boost::system::error_code& error)
{
    if (error == boost::asio::error::operation_aborted)
    {
        NETWORK_LOG_INFO("[Network] Keep-alive timer cancelled");
        return;
    }

    if (!running)
    {
        NETWORK_LOG_INFO("[Network] Network not running, cancelling keep-alive");
        return;
    }

    // TODO: REFACTOR FOR *1, FOR MULTIPLE PEERS
    NETWORK_LOG_INFO("[Network] Running keep-alive functionality");
    sendHolePunchPacket(); // Send hole punch packet
    if (peerConnection.isConnected())
    {
        checkAllConnections(); // Check connection status
    }

    startKeepAliveTimer(); // Restart timer
}

uint32_t UDPNetwork::attachCustomHeader(
    const std::shared_ptr<std::vector<uint8_t>>& packet,
    PacketType packetType,
    std::optional<uint32_t> seqOpt)
{
    // Set magic number
    (*packet)[0] = (MAGIC_NUMBER >> 24) & 0xFF;
    (*packet)[1] = (MAGIC_NUMBER >> 16) & 0xFF;
    (*packet)[2] = (MAGIC_NUMBER >> 8) & 0xFF;
    (*packet)[3] = MAGIC_NUMBER & 0xFF;
    
    // Set protocol version
    (*packet)[4] = (PROTOCOL_VERSION >> 8) & 0xFF;
    (*packet)[5] = PROTOCOL_VERSION & 0xFF;
    
    // Set packet type
    (*packet)[6] = static_cast<uint8_t>(packetType);
    
    // Set sequence number
    uint32_t seq = seqOpt.value_or(nextSeqNumber++);
    (*packet)[8] = (seq >> 24) & 0xFF;
    (*packet)[9] = (seq >> 16) & 0xFF;
    (*packet)[10] = (seq >> 8) & 0xFF;
    (*packet)[11] = seq & 0xFF;

    return seq;
}