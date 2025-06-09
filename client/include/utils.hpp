#pragma once

#include <cstdint>
#include <string>
#include <sstream>

namespace utils
{
inline uint32_t ipToUint32(const std::string& ipAddress)
{
    uint32_t result = 0;
    
    // Parse IP address
    std::istringstream iss(ipAddress);
    std::string octet;
    int i = 0;
    
    while (std::getline(iss, octet, '.') && i < 4) {
        uint32_t value = std::stoi(octet);
        result = (result << 8) | (value & 0xFF);
        i++;
    }
    
    return result;
}

inline std::string uint32ToIp(uint32_t ipAddress)
{
    std::ostringstream result;
    
    for (int i = 0; i < 4; i++) {
        uint8_t octet = (ipAddress >> (8 * (3 - i))) & 0xFF;
        result << static_cast<int>(octet);
        if (i < 3) {
            result << ".";
        }
    }
    
    return result.str();
}
}