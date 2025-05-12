#include "tun_interface.hpp"

#include <Windows.h>
#include <iostream>
#include <string>

typedef WINTUN_ADAPTER_HANDLE (*WintunOpenAdapterFunc)(const WCHAR*);
typedef WINTUN_ADAPTER_HANDLE (*WintunCreateAdapterFunc)(const WCHAR*, const WCHAR*, GUID*);
typedef DWORD (*WintunGetLastErrorFunc)();

bool TunInterface::initialize(const std::string& deviceName) {
    // Load wintun.dll dynamically
    HMODULE wintunModule = LoadLibrary("wintun.dll");
    if (!wintunModule) {
        std::cerr << "Failed to load wintun.dll" << std::endl;
        return false;
    }

    // Retrieve the WintunOpenAdapter and WintunCreateAdapter functions
    WintunOpenAdapterFunc WintunOpenAdapter = 
        (WintunOpenAdapterFunc)GetProcAddress(wintunModule, "WintunOpenAdapter");
    WintunCreateAdapterFunc WintunCreateAdapter = 
        (WintunCreateAdapterFunc)GetProcAddress(wintunModule, "WintunCreateAdapter");

    if (!WintunOpenAdapter || !WintunCreateAdapter) {
        std::cerr << "Failed to load Wintun functions" << std::endl;
        FreeLibrary(wintunModule);
        return false;
    }

    // Convert deviceName to wide string
    std::wstring wideDeviceName(deviceName.begin(), deviceName.end());

    // Attempt to open an existing adapter first
    WINTUN_ADAPTER_HANDLE adapter = WintunOpenAdapter(wideDeviceName.c_str());
    if (!adapter) {
        std::cerr << "Adapter not found; attempting to create a new one" << std::endl;
        adapter = WintunCreateAdapter(wideDeviceName.c_str(), L"Wintun", NULL);

        if (!adapter) {
            std::cerr << "Failed to create WinTun adapter; please run as Administrator for setup" << std::endl;
            FreeLibrary(wintunModule);
            return false;
        }
    }

    this->adapter = adapter;
    std::cout << "WinTun adapter initialized successfully." << std::endl;

    FreeLibrary(wintunModule);
    return true;
}
