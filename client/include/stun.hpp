#pragma once
#include <string>
#include <optional>
#include <boost/asio.hpp>

struct PublicAddress {
    std::string ip;
    int port;
};

class StunClient {
public:
    StunClient(const std::string& server = "stun.l.google.com", const std::string& port = "19302");
    
    // Get public IP and port
    std::optional<PublicAddress> discoverPublicAddress();
    
    // Set custom STUN server
    void setStunServer(const std::string& server, const std::string& port = "19302");

    std::unique_ptr<boost::asio::ip::udp::socket> getSocket();
    boost::asio::io_context& getContext();

private:
    std::string stun_server_;
    std::string stun_port_;
    std::unique_ptr<boost::asio::ip::udp::socket> socket_;
    boost::asio::io_context io_context_;
};