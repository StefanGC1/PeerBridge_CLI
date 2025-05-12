#ifndef TUN_INTERFACE_H
#define TUN_INTERFACE_H

#include <string>
#include <Windows.h>
#include <wintun.h>

class TunInterface {
public:
    TunInterface() = default;
    ~TunInterface() = default;

    // Initialize the TUN interface with a specified device name
    bool initialize(const std::string& deviceName);

    // Close and clean up the TUN interface
    void close();

private:
    WINTUN_ADAPTER_HANDLE adapter = nullptr;  // Adapter handle for WinTun on Windows
};

#endif // TUN_INTERFACE_H
