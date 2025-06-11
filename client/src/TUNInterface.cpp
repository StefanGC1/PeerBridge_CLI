#include "TUNInterface.hpp"
#include "Logger.hpp"
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

TunInterface::~TunInterface()
{
    close();
}

bool TunInterface::loadWintunFunctions(HMODULE wintunModule)
{
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
    pWintunGetReadWaitEvent =
        reinterpret_cast<WintunGetReadWaitEventFunc>(GetProcAddress(wintunModule, "WintunGetReadWaitEvent"));
    pWintunDeleteDriver = 
        reinterpret_cast<WintunDeleteDriverFunc>(GetProcAddress(wintunModule, "WintunDeleteDriver"));

    return pWintunOpenAdapter && pWintunCreateAdapter && 
           pWintunStartSession && pWintunAllocateSendPacket && pWintunSendPacket && 
           pWintunReceivePacket && pWintunReleaseReceivePacket && pWintunEndSession && 
           pWintunCloseAdapter && pWintunGetAdapterLUID && pWintunGetReadWaitEvent;
}

bool TunInterface::initialize(const std::string& deviceName)
{
    // Load wintun.dll
    std::wstring wideWintunPath = L"wintun.dll";
    wintunModule = LoadLibraryW(wideWintunPath.c_str());
    if (!wintunModule)
    {
        SYSTEM_LOG_ERROR("[TunInterface] Failed to load wintun.dll. Error: {}", GetLastError());
        return false;
    }

    // Load functions
    if (!loadWintunFunctions(wintunModule))
    {
        SYSTEM_LOG_ERROR("[TunInterface] Failed to load Wintun functions. Error: {}", GetLastError());
        FreeLibrary(wintunModule);
        wintunModule = nullptr;
        return false;
    }

    // Convert deviceName to wide string
    std::wstring wideDeviceName(deviceName.begin(), deviceName.end());

    // Attempt to open an existing adapter first
    adapter = pWintunOpenAdapter(wideDeviceName.c_str());
    if (!adapter)
    {
        DWORD error = GetLastError();
        SYSTEM_LOG_ERROR("[TunInterface] Adapter not found (error: {}); attempting to create a new one", error);
        GUID guid;

        // TODO: CREATE GUID AS CONFIG ON INSTALLER, LOAD FROM CONFIG AFTER
        static GUID WINTUN_ADAPTER_GUID = 
            { 0x593be3bb, 0x839a, 0x47e5, { 0x82, 0xa2, 0x95, 0xa0, 0x4a, 0xac, 0xb9, 0x1f } };
        adapter = pWintunCreateAdapter(wideDeviceName.c_str(), L"Wintun", &WINTUN_ADAPTER_GUID);

        if (!adapter) {
            SYSTEM_LOG_ERROR("[TunInterface] Failed to create WinTun adapter; please run as Administrator for setup. Error: {}", GetLastError());
            FreeLibrary(wintunModule);
            wintunModule = nullptr;
            return false;
        }
    }

    // Start a Wintun session
    const DWORD WINTUN_RING_CAPACITY = 0x800000; // 8 MiB
    session = pWintunStartSession(adapter, WINTUN_RING_CAPACITY);
    if (!session)
    {
        SYSTEM_LOG_ERROR("[TunInterface] Failed to start Wintun session. Error: {}", GetLastError());
        pWintunCloseAdapter(adapter);
        adapter = nullptr;
        FreeLibrary(wintunModule);
        wintunModule = nullptr;
        return false;
    }

    SYSTEM_LOG_INFO("[TunInterface] WinTun adapter initialized successfully.");
    return true;
}

bool TunInterface::startPacketProcessing()
{
    if (!adapter || !session)
    {
        SYSTEM_LOG_ERROR("[TunInterface] Adapter or session not initialized");
        return false;
    }
    
    if (running)
    {
        SYSTEM_LOG_ERROR("[TunInterface] Packet processing already running");
        return false;
    }
    
    running = true;
    
    // Start receive thread
    receiveThread = std::thread(&TunInterface::receiveThreadFunc, this);
    sendThread = std::thread(&TunInterface::sendThreadFunc, this);
    
    SYSTEM_LOG_INFO("[TunInterface] Packet processing started");
    return true;
}

void TunInterface::stopPacketProcessing()
{
    running = false;
    
    // Wait for threads to finish
    if (receiveThread.joinable())
    {
        receiveThread.join();
    }
    
    if (sendThread.joinable())
    {
        sendThread.join();
    }

    // In theory, it's possible this could cause problems :)
    // Classic race condition moment
    outgoingPackets = {};
    
    SYSTEM_LOG_INFO("[TunInterface] Packet processing stopped");
}

void TunInterface::receiveThreadFunc() {
    // Get Wintun's read-wait event handle
    HANDLE readWaitEvent = pWintunGetReadWaitEvent(session);
    if (!readWaitEvent)
    {
        NETWORK_LOG_ERROR("[TunInterface] Failed to get Wintun read wait event");
        return;
    }
    
    while (running)
    {
        DWORD packetSize;
        WINTUN_PACKET* packet = pWintunReceivePacket(session, &packetSize);
        
        if (packet)
        {
            // Copy packet data, cast to uint8_t* to copy to vector
            const uint8_t* packetDataPtr = reinterpret_cast<const uint8_t*>(packet);
            std::vector<uint8_t> packetData(packetDataPtr, packetDataPtr + packetSize);
            
            // Release the packet
            pWintunReleaseReceivePacket(session, packet);
            
            // Process the packet
            if (packetCallback)
            {
                packetCallback(packetData);
            }

            continue;
        }
        
        // Wait for "packet ready" event signal from wintun or timeout via Windows API
        // In high-level terms, this is like waiting on a kernel-level condition variable / signal
        DWORD waitResult = WaitForSingleObject(readWaitEvent, 5); // 5ms timeout for gaming responsiveness
        
        if (waitResult == WAIT_TIMEOUT)
        {
            // Signal to continue the loop
            continue;
        }
        else if (waitResult != WAIT_OBJECT_0)
        {
            // Error occurred
            if (running)
                SYSTEM_LOG_ERROR("[TunInterface] WaitForSingleObject failed: {}", GetLastError());
            break;
        }
    }
}

void TunInterface::sendThreadFunc()
{
    while (running)
    {
        std::vector<uint8_t> packetData;
        
        // Wait for packet or timeout
        {
            std::unique_lock<std::mutex> lock(packetQueueMutex);
            
            if (packetConditionVariable.wait_for(lock, std::chrono::milliseconds(1), 
                [this] { return !outgoingPackets.empty() || !running; })) 
            {
                // Got signaled - packet available or shutting down
                if (!running) break;
                
                if (!outgoingPackets.empty()) {
                    packetData = std::move(outgoingPackets.front());
                    outgoingPackets.pop();
                }
            }
        }
        
        if (!packetData.empty())
        {
            // Allocate a packet
            WINTUN_PACKET* packet = pWintunAllocateSendPacket(session, packetData.size());
            
            if (packet) {
                // Copy the data, cast to void* to copy to packet
                memcpy(reinterpret_cast<void*>(packet), 
                       reinterpret_cast<const void*>(packetData.data()), 
                       packetData.size());
                
                // Send the packet
                pWintunSendPacket(session, packet);
            }
        }
    }
}

bool TunInterface::sendPacket(std::vector<uint8_t> packet)
{
    if (!running)
    {
        SYSTEM_LOG_ERROR("[TunInterface] Packet processing not running");
        return false;
    }
    
    // Add the packet to the queue and notify the send thread
    {
        std::lock_guard<std::mutex> lock(packetQueueMutex);
        outgoingPackets.push(std::move(packet));
    }
    packetConditionVariable.notify_one();
    
    return true;
}

void TunInterface::setPacketCallback(PacketCallback callback)
{
    packetCallback = std::move(callback);
}

bool TunInterface::isRunning() const
{
    return running;
}

void TunInterface::close()
{
    // Stop packet processing
    if (running)
    {
        stopPacketProcessing();
    }
    
    // End session
    if (session)
    {
        pWintunEndSession(session);
        session = nullptr;
    }
    
    // Close adapter
    if (adapter)
    {
        pWintunCloseAdapter(adapter);
        adapter = nullptr;
    }
    
    // Unload library
    if (wintunModule)
    {
        FreeLibrary(wintunModule);
        wintunModule = nullptr;
    }
    
    SYSTEM_LOG_INFO("[TunInterface] TUN interface closed");
}

std::string TunInterface::getNarrowAlias() const
{
    NET_LUID adapterLuid;
    if (!pWintunGetAdapterLUID(adapter, &adapterLuid))
    {
        SYSTEM_LOG_ERROR("[TunInterface] Failed to get adapter LUID. Error: {}", GetLastError());
        return "";
    }

    // Get interface alias (friendly name) from LUID
    WCHAR interfaceAlias[IF_MAX_STRING_SIZE + 1] = { 0 };
    if (ConvertInterfaceLuidToAlias(&adapterLuid, interfaceAlias, IF_MAX_STRING_SIZE) != NO_ERROR)
    {
        SYSTEM_LOG_ERROR("[TunInterface] Failed to get interface alias from LUID. Error: {}", GetLastError());
        return "";
    }

    // Convert wide string to narrow string
    char narrowAlias[IF_MAX_STRING_SIZE + 1] = { 0 };
    WideCharToMultiByte(CP_UTF8, 0, interfaceAlias, -1, narrowAlias, sizeof(narrowAlias), NULL, NULL);
    return std::string(narrowAlias);
}
