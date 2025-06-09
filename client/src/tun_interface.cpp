#include "tun_interface.hpp"
#include "logger.hpp"
#include <Windows.h>
#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <iphlpapi.h>
#include <random>
#include <netioapi.h>

#pragma comment(lib, "iphlpapi.lib")

TunInterface::TunInterface() {}

TunInterface::~TunInterface() {
    close();
}

bool TunInterface::loadWintunFunctions(HMODULE wintunModule) {
    pWintunOpenAdapter = 
        reinterpret_cast<WintunOpenAdapterFunc>(GetProcAddress(wintunModule, "WintunOpenAdapter"));
    pWintunCreateAdapter = 
        reinterpret_cast<WintunCreateAdapterFunc>(GetProcAddress(wintunModule, "WintunCreateAdapter"));
    pWintunStartSession = 
        reinterpret_cast<WintunStartSessionFunc>(GetProcAddress(wintunModule, "WintunStartSession"));
    pWintunAllocateSendPacket = 
        reinterpret_cast<WintunAllocateSendPacketFunc>(GetProcAddress(wintunModule, "WintunAllocateSendPacket"));
    pWintunSendPacket = 
        reinterpret_cast<WintunSendPacketFunc>(GetProcAddress(wintunModule, "WintunSendPacket"));
    pWintunReceivePacket = 
        reinterpret_cast<WintunReceivePacketFunc>(GetProcAddress(wintunModule, "WintunReceivePacket"));
    pWintunReleaseReceivePacket = 
        reinterpret_cast<WintunReleaseReceivePacketFunc>(GetProcAddress(wintunModule, "WintunReleaseReceivePacket"));
    pWintunEndSession = 
        reinterpret_cast<WintunEndSessionFunc>(GetProcAddress(wintunModule, "WintunEndSession"));
    pWintunCloseAdapter = 
        reinterpret_cast<WintunCloseAdapterFunc>(GetProcAddress(wintunModule, "WintunCloseAdapter"));
    pWintunGetAdapterLUID =
        reinterpret_cast<WintunGetAdapterLUIDFunc>(GetProcAddress(wintunModule, "WintunGetAdapterLUID"));
    // TODO: GET WintunDeleteAdapter
    pWintunDeleteDriver = 
        reinterpret_cast<WintunDeleteDriverFunc>(GetProcAddress(wintunModule, "WintunDeleteDriver"));

    return pWintunOpenAdapter && pWintunCreateAdapter && 
           pWintunStartSession && pWintunAllocateSendPacket && pWintunSendPacket && 
           pWintunReceivePacket && pWintunReleaseReceivePacket && pWintunEndSession && 
           pWintunCloseAdapter && pWintunGetAdapterLUID;
}

bool TunInterface::initialize(const std::string& deviceName) {
    // Load wintun.dll dynamically
    // Convert string to wide string for LoadLibraryW
    std::wstring wideWintunPath = L"wintun.dll";
    wintunModule_ = LoadLibraryW(wideWintunPath.c_str());
    if (!wintunModule_) {
        std::cerr << "Failed to load wintun.dll. Error: " << GetLastError() << std::endl;
        return false;
    }

    // Load functions
    if (!loadWintunFunctions(wintunModule_)) {
        std::cerr << "Failed to load Wintun functions. Error: " << GetLastError() << std::endl;
        FreeLibrary(wintunModule_);
        wintunModule_ = nullptr;
        return false;
    }

    // Convert deviceName to wide string
    std::wstring wideDeviceName(deviceName.begin(), deviceName.end());

    // Attempt to open an existing adapter first
    adapter = pWintunOpenAdapter(wideDeviceName.c_str());
    if (!adapter) {
        DWORD error = GetLastError();
        std::cerr << "Adapter not found (error: " << error << "); attempting to create a new one" << std::endl;
        GUID guid;

        // TODO: CREATE GUID AS CONFIG ON INSTALLER, LOAD FROM CONFIG AFTER
        static GUID WINTUN_ADAPTER_GUID = 
            { 0x593be3bb, 0x839a, 0x47e5, { 0x82, 0xa2, 0x95, 0xa0, 0x4a, 0xac, 0xb9, 0x1f } };
        adapter = pWintunCreateAdapter(wideDeviceName.c_str(), L"Wintun", &WINTUN_ADAPTER_GUID);

        if (!adapter) {
            std::cerr << "Failed to create WinTun adapter; please run as Administrator for setup. Error: " << GetLastError() << std::endl;
            FreeLibrary(wintunModule_);
            wintunModule_ = nullptr;
            return false;
        }
    }

    // Start a Wintun session
    const DWORD WINTUN_RING_CAPACITY = 0x400000; // 4 MiB
    session = pWintunStartSession(adapter, WINTUN_RING_CAPACITY);
    if (!session) {
        std::cerr << "Failed to start Wintun session. Error: " << GetLastError() << std::endl;
        pWintunCloseAdapter(adapter);
        adapter = nullptr;
        FreeLibrary(wintunModule_);
        wintunModule_ = nullptr;
        return false;
    }

    clog << "WinTun adapter initialized successfully." << std::endl;
    return true;
}

bool TunInterface::startPacketProcessing() {
    if (!adapter || !session) {
        std::cerr << "Adapter or session not initialized" << std::endl;
        return false;
    }
    
    if (running_) {
        std::cerr << "Packet processing already running" << std::endl;
        return false;
    }
    
    running_ = true;
    
    // Start receive thread
    receiveThread_ = std::thread(&TunInterface::receiveThreadFunc, this);
    sendThread_ = std::thread(&TunInterface::sendThreadFunc, this);
    
    clog << "Packet processing started" << std::endl;
    return true;
}

void TunInterface::stopPacketProcessing() {
    running_ = false;
    
    // Wait for threads to finish
    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }
    
    if (sendThread_.joinable()) {
        sendThread_.join();
    }
    
    clog << "Packet processing stopped" << std::endl;
}

void TunInterface::receiveThreadFunc() {
    while (running_) {
        DWORD packetSize;
        WINTUN_PACKET* packet = pWintunReceivePacket(session, &packetSize);
        
        if (packet) {
            // Copy packet data - in WireGuard's Wintun, the packet pointer is the data
            // Use explicit cast to uint8_t* and construct the vector properly
            const uint8_t* packetDataPtr = reinterpret_cast<const uint8_t*>(packet);
            std::vector<uint8_t> packetData(packetDataPtr, packetDataPtr + packetSize);
            
            // Release the packet
            pWintunReleaseReceivePacket(session, packet);
            
            // Process the packet
            if (packetCallback_) {
                packetCallback_(packetData);
            }
        } else {
            // No packet available, sleep for a bit
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void TunInterface::sendThreadFunc() {
    while (running_) {
        std::vector<uint8_t> packetData;
        
        // Wait for packet or timeout (500μs for gaming responsiveness)
        {
            std::unique_lock<std::mutex> lock(packetQueueMutex_);
            
            if (packetCondition_.wait_for(lock, std::chrono::microseconds(500), 
                [this] { return !outgoingPackets_.empty() || !running_; })) {
                
                // Got signaled - packet available or shutting down
                if (!running_) break;
                
            if (!outgoingPackets_.empty()) {
                packetData = std::move(outgoingPackets_.front());
                outgoingPackets_.pop();
            }
            }
            // If timeout occurred, continue loop (keeps thread responsive)
        }
        
        if (!packetData.empty()) {
            // Allocate a packet
            WINTUN_PACKET* packet = pWintunAllocateSendPacket(session, packetData.size());
            
            if (packet) {
                // Copy the data - in WireGuard's Wintun, the packet pointer is the data
                // Use memcpy with proper casting for C++ compliance
                memcpy(reinterpret_cast<void*>(packet), 
                       reinterpret_cast<const void*>(packetData.data()), 
                       packetData.size());
                
                // Send the packet
                pWintunSendPacket(session, packet);
            }
        }
    }
}

bool TunInterface::sendPacket(std::vector<uint8_t> packet) {
    if (!running_) {
        std::cerr << "Packet processing not running" << std::endl;
        return false;
    }
    
    // Add the packet to the queue and notify the send thread
    {
        std::lock_guard<std::mutex> lock(packetQueueMutex_);
        outgoingPackets_.push(std::move(packet));
    }
    packetCondition_.notify_one();
    
    return true;
}

void TunInterface::setPacketCallback(PacketCallback callback) {
    packetCallback_ = std::move(callback);
}

bool TunInterface::isRunning() const {
    return running_;
}

void TunInterface::close() {
    // Stop packet processing
    if (running_) {
        stopPacketProcessing();
    }
    
    // End session
    if (session) {
        pWintunEndSession(session);
        session = nullptr;
    }
    
    // Close adapter
    if (adapter) {
        pWintunCloseAdapter(adapter);
        adapter = nullptr;
    }
    
    // Unload library
    if (wintunModule_) {
        FreeLibrary(wintunModule_);
        wintunModule_ = nullptr;
    }
    
    clog << "TUN interface closed" << std::endl;
}

std::string TunInterface::getNarrowAlias() const {
    NET_LUID adapterLuid;
    if (!pWintunGetAdapterLUID(adapter, &adapterLuid)) {
        std::cerr << "Failed to get adapter LUID. Error: " << GetLastError() << std::endl;
        return "";
    }

    // Get interface alias (friendly name) from LUID
    WCHAR interfaceAlias[IF_MAX_STRING_SIZE + 1] = { 0 };
    if (ConvertInterfaceLuidToAlias(&adapterLuid, interfaceAlias, IF_MAX_STRING_SIZE) != NO_ERROR) {
        std::cerr << "Failed to get interface alias from LUID. Error: " << GetLastError() << std::endl;
        return "";
    }

    // Convert wide string to narrow string
    char narrowAlias[IF_MAX_STRING_SIZE + 1] = { 0 };
    WideCharToMultiByte(CP_UTF8, 0, interfaceAlias, -1, narrowAlias, sizeof(narrowAlias), NULL, NULL);
    return std::string(narrowAlias);
}
