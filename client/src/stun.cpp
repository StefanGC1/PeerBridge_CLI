#include "stun.hpp"
#include <iostream>
#include <sodium/randombytes.h>

StunClient::StunClient(const std::string& server, const std::string& port)
    : stun_server_(server), stun_port_(port)
    , io_context_()
{
}

void StunClient::setStunServer(const std::string& server, const std::string& port) {
    stun_server_ = server;
    stun_port_ = port;
}

std::optional<PublicAddress> StunClient::discoverPublicAddress()
{
    using boost::asio::ip::udp;
    try
    {
        socket_ = std::make_unique<udp::socket>(io_context_);
        // --- Setup socket, build and send STUN binding request --- //
        udp::resolver resolver(io_context_);
        udp::endpoint stun_endpoint = *resolver.resolve(stun_server_, stun_port_).begin();
        socket_->open(udp::v4());

        // Build STUN binding request according to RFC 5389 protocol
        std::array<uint8_t, 20> request{};
        request[0] = 0x00; request[1] = 0x01;  // Bytes 0..1 Binding Request
        request[2] = 0x00; request[3] = 0x00;  // Bytes 2..3 Message length
        request[4] = 0x21; request[5] = 0x12; request[6] = 0xA4; request[7] = 0x42;  // Bytes 4..7 Magic Cookie
        randombytes_buf(&request[8], 12); // Bytes 8..19 Transaction ID

        // Send request
        socket_->send_to(boost::asio::buffer(request), stun_endpoint);

        // Receive response
        std::array<uint8_t, 512> response{};
        udp::endpoint sender_endpoint;
        
        // Set timeout
        socket_->non_blocking(true);
        boost::asio::steady_timer timer(io_context_);
        timer.expires_after(std::chrono::seconds(5));
        
        boost::system::error_code ec;
        size_t len = 0;
        
        bool received = false;

        // Async receive with timeout
        socket_->async_receive_from(
            boost::asio::buffer(response), sender_endpoint,
            [&](const boost::system::error_code& error, std::size_t bytes_recvd) {
                if (!error) {
                    len = bytes_recvd;
                    received = true;
                    timer.cancel(); // cancel the timer if we received data
                }
            }
        );
        
        // Set up async timeout
        timer.async_wait([&](const boost::system::error_code& error) {
            if (!error) {
                socket_->cancel(); // cancel receive if timeout occurs
            }
        });
        
        // Run IO until one of them finishes
        io_context_.run();

        if (!received) {
            std::cerr << "[STUN] Response timeout or error.\n";
            return std::nullopt;
        }

        // --- Validate STUN Response --- //

        // Check length is not smaller than header size
        if (len < 20) {
            std::cerr << "[STUN] Response too short.\n";
            return std::nullopt;
        }

        // Check length against reported message length
        uint16_t msg_length = response[2] << 8 | response[3];
        if (20 + msg_length > len) {
            std::cerr << "[STUN] Message length exceeds received size.\n";
            return std::nullopt;
        }

        // Check message type is binding success
        uint16_t msg_type = response[0] << 8 | response[1];
        if (msg_type != 0x0101) { // 0x0101 = Binding success response
            std::cerr << "[STUN] Not a Binding Success Response.\n";
            return std::nullopt;
        }

        // Parse XOR-MAPPED-ADDRESS attribute
        for (size_t i = 20; i + 4 < len;) {
            uint16_t attr_type = (response[i] << 8) | response[i + 1];
            uint16_t attr_len  = (response[i + 2] << 8) | response[i + 3];
            i += 4;
            if (attr_type == 0x0020) {  // XOR-MAPPED-ADDRESS
                uint8_t family = response[i + 1];
                uint16_t xport = (response[i + 2] << 8) | response[i + 3];
                uint32_t xip = (response[i + 4] << 24) | (response[i + 5] << 16) |
                               (response[i + 6] << 8) | response[i + 7];

                uint16_t port = xport ^ 0x2112;
                uint32_t ip_raw = xip ^ 0x2112A442;

                boost::asio::ip::address_v4::bytes_type ip_bytes {
                    static_cast<uint8_t>((ip_raw >> 24) & 0xFF),
                    static_cast<uint8_t>((ip_raw >> 16) & 0xFF),
                    static_cast<uint8_t>((ip_raw >> 8) & 0xFF),
                    static_cast<uint8_t>((ip_raw) & 0xFF)
                };

                std::string ip_str = boost::asio::ip::address_v4(ip_bytes).to_string();
                return PublicAddress{ ip_str, port };
            }
            i += attr_len;
        }
    } catch (std::exception& e) {
        std::cerr << "[STUN] Failed: " << e.what() << std::endl;
    }

    return std::nullopt;
}

std::unique_ptr<boost::asio::ip::udp::socket> StunClient::getSocket()
{
    return std::move(socket_);
}

boost::asio::io_context& StunClient::getContext()
{
    return io_context_;
}