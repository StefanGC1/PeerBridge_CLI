#pragma once
#include <string>
#include <optional>

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

private:
    std::string stun_server_;
    std::string stun_port_;
};