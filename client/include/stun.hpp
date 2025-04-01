#pragma once
#include <string>
#include <optional>

struct PublicAddress {
    std::string ip;
    int port;
};

std::optional<PublicAddress> get_public_address();
