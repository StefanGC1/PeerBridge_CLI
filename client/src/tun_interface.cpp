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

// Random number generator for MAC address
std::random_device rd;
std::mt19937 gen(rd());
std::uniform_int_distribution<> dis(0, 255);

TunInterface::TunInterface() {
    // Generate a random virtual MAC address with a locally administered bit
    virtualMac_.resize(6);
    virtualMac_[0] = 0x02;  // Set locally administered bit
    for (int i = 1; i < 6; i++) {
        virtualMac_[i] = dis(gen);
    }
}

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

bool TunInterface::configureInterface(const std::string& ipAddress, const std::string& netmask) {
    if (!adapter) {
        std::cerr << "Adapter not initialized" << std::endl;
        return false;
    }

    ipAddress_ = ipAddress;
    netmask_ = netmask;

    // Get the adapter LUID
    NET_LUID adapterLuid;
    if (!pWintunGetAdapterLUID(adapter, &adapterLuid)) {
        std::cerr << "Failed to get adapter LUID. Error: " << GetLastError() << std::endl;
        return false;
    }

    // Get interface alias (friendly name) from LUID
    WCHAR interfaceAlias[IF_MAX_STRING_SIZE + 1] = { 0 };
    if (ConvertInterfaceLuidToAlias(&adapterLuid, interfaceAlias, IF_MAX_STRING_SIZE) != NO_ERROR) {
        std::cerr << "Failed to get interface alias from LUID. Error: " << GetLastError() << std::endl;
        return false;
    }

    // Convert wide string to narrow string
    char narrowAlias[IF_MAX_STRING_SIZE + 1] = { 0 };
    WideCharToMultiByte(CP_UTF8, 0, interfaceAlias, -1, narrowAlias, sizeof(narrowAlias), NULL, NULL);
    
    // Configure IP address using netsh
    std::ostringstream command;
    command << "netsh interface ip set address \"" << narrowAlias 
            << "\" static " << ipAddress << " " << netmask;
    
    if (!executeNetshCommand(command.str())) {
        std::cerr << "Failed to configure IP address" << std::endl;
        return false;
    }
    
    clog << "Interface configured with IP: " << ipAddress << " netmask: " << netmask << std::endl;
    return true;
}

bool TunInterface::setupRouting() {
    if (ipAddress_.empty() || netmask_.empty()) {
        std::cerr << "IP address or netmask not configured" << std::endl;
        return false;
    }

    // Convert IP address and netmask to uint32
    uint32_t ip = ipToUint32(ipAddress_);
    uint32_t mask = ipToUint32(netmask_);
    
    // Calculate network address
    uint32_t network = ip & mask;
    std::string networkAddr = uint32ToIp(network);
    
    // Get the adapter LUID
    NET_LUID adapterLuid;
    if (!pWintunGetAdapterLUID(adapter, &adapterLuid)) {
        std::cerr << "Failed to get adapter LUID. Error: " << GetLastError() << std::endl;
        return false;
    }

    // Get interface alias (friendly name) from LUID
    WCHAR interfaceAlias[IF_MAX_STRING_SIZE + 1] = { 0 };
    if (ConvertInterfaceLuidToAlias(&adapterLuid, interfaceAlias, IF_MAX_STRING_SIZE) != NO_ERROR) {
        std::cerr << "Failed to get interface alias from LUID. Error: " << GetLastError() << std::endl;
        return false;
    }

    // Convert wide string to narrow string
    char narrowAlias[IF_MAX_STRING_SIZE + 1] = { 0 };
    WideCharToMultiByte(CP_UTF8, 0, interfaceAlias, -1, narrowAlias, sizeof(narrowAlias), NULL, NULL);
    
    // Count mask bits for CIDR notation
    int maskBits = 0;
    uint32_t tempMask = mask;
    while (tempMask) {
        maskBits += (tempMask & 1);
        tempMask >>= 1;
    }
    
    // Try different approaches for adding the route
    
    // Approach 1: Add route using netsh with separate mask format
    std::ostringstream command;
    command << "netsh interface ipv4 add route " 
            << networkAddr << " " << netmask_ 
            << " \"" << narrowAlias << "\""
            << " metric=1";

    bool success = executeNetshCommand(command.str());
    
    if (!success) {
        clog << "First route command failed, trying alternative format..." << std::endl;
        
        // Approach 2: Try using CIDR notation
        command.str("");
        command << "netsh interface ipv4 add route " 
                << networkAddr << "/" << maskBits
                << " \"" << narrowAlias << "\""
                << " metric=1";
        
        success = executeNetshCommand(command.str());
    }
    
    if (!success) {
        clog << "Second route command failed, trying to add direct routes..." << std::endl;
        
        // Approach 3: Try adding a specific route to peer IP
        // This ensures at least basic connectivity even if subnet routing fails
        command.str("");
        
        // Determine peer IP based on local IP
        std::string peerIP;
        if (ipAddress_ == "10.0.0.1") {
            peerIP = "10.0.0.2";
        } else {
            peerIP = "10.0.0.1";
        }
        
        command << "netsh interface ipv4 add route " 
                << peerIP << "/32"
                << " \"" << narrowAlias << "\""
                << " metric=1";
        
        success = executeNetshCommand(command.str());
    }
    
    if (!success) {
        std::cerr << "Failed to add route for virtual network" << std::endl;
        return false;
    }
    
    // Enable forwarding on the interface
    command.str("");
    command << "netsh interface ipv4 set interface \"" << narrowAlias << "\" forwarding=enabled metric=1";
    
    if (!executeNetshCommand(command.str())) {
        std::cerr << "Failed to enable forwarding on interface" << std::endl;
        return false;
    }

    // 224.0.0.0/4  →  on-link via P2PBridge, metric 1
    command.str("");
    command << "netsh interface ipv4 add route "
            << "prefix=224.0.0.0/4 "
            << "interface=\"" << narrowAlias << "\" "
            << "metric=1";
    if (!executeNetshCommand(command.str())) {
        std::cerr << "Failed to add route for multicast traffic" << std::endl;
    }
    
    clog << "Routing configured successfully for virtual network" << std::endl;
    return true;
}

bool TunInterface::executeNetshCommand(const std::string& command) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    
    // Add "cmd /c " prefix to the command to execute it via cmd
    std::string fullCommand = "cmd /c " + command;
    
    clog << "Executing: " << fullCommand << std::endl;
    
    // Convert to wide string for CreateProcessW
    int size = MultiByteToWideChar(CP_UTF8, 0, fullCommand.c_str(), -1, NULL, 0);
    std::vector<wchar_t> wideCommand(size);
    MultiByteToWideChar(CP_UTF8, 0, fullCommand.c_str(), -1, wideCommand.data(), size);
    
    // Create the process (use CreateProcessW for wide strings)
    if (!CreateProcessW(NULL, wideCommand.data(), NULL, NULL, FALSE, 
                      CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        std::cerr << "Failed to execute command: " << command << std::endl;
        std::cerr << "Windows error: " << GetLastError() << std::endl;
        return false;
    }
    
    // Wait for the process to finish
    WaitForSingleObject(pi.hProcess, INFINITE);
    
    // Get the exit code
    DWORD exitCode;
    if (!GetExitCodeProcess(pi.hProcess, &exitCode)) {
        std::cerr << "Failed to get process exit code" << std::endl;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return false;
    }
    
    // Clean up handles
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    
    if (exitCode != 0) {
        std::cerr << "Command failed with exit code: " << exitCode << std::endl;
    }
    
    return exitCode == 0;
}

uint32_t TunInterface::ipToUint32(const std::string& ipAddress) {
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

std::string TunInterface::uint32ToIp(uint32_t ipAddress) {
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
            
            // Debug: Print packet info
            if (packetSize >= 20) { // Minimum IPv4 header size
                uint8_t protocol = packetDataPtr[9];
                uint32_t srcIp = (packetDataPtr[12] << 24) | (packetDataPtr[13] << 16) | 
                                  (packetDataPtr[14] << 8) | packetDataPtr[15];
                uint32_t dstIp = (packetDataPtr[16] << 24) | (packetDataPtr[17] << 16) | 
                                  (packetDataPtr[18] << 8) | packetDataPtr[19];
                
                // clog << "TUN Recv: " << uint32ToIp(srcIp) << " -> " << uint32ToIp(dstIp) 
                //           << " (Proto: " << static_cast<int>(protocol) << ", Size: " << packetSize << ")" << std::endl;
            }
            
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
        
        // Get a packet from the queue
        {
            std::lock_guard<std::mutex> lock(packetQueueMutex_);
            if (!outgoingPackets_.empty()) {
                packetData = std::move(outgoingPackets_.front());
                outgoingPackets_.pop();
            }
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
        } else {
            // No packet available, sleep for a bit
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

bool TunInterface::sendPacket(const std::vector<uint8_t>& packet) {
    if (!running_) {
        std::cerr << "Packet processing not running" << std::endl;
        return false;
    }
    
    // Debug: Print packet info
    if (packet.size() >= 20) { // Minimum IPv4 header size
        uint8_t protocol = packet[9];
        uint32_t srcIp = (packet[12] << 24) | (packet[13] << 16) | 
                          (packet[14] << 8) | packet[15];
        uint32_t dstIp = (packet[16] << 24) | (packet[17] << 16) | 
                          (packet[18] << 8) | packet[19];
        
        // clog << "TUN Send: " << uint32ToIp(srcIp) << " -> " << uint32ToIp(dstIp) 
        //           << " (Proto: " << static_cast<int>(protocol) << ", Size: " << packet.size() << ")" << std::endl;
    }
    
    // Add the packet to the queue
    {
        std::lock_guard<std::mutex> lock(packetQueueMutex_);
        outgoingPackets_.push(packet);
    }
    
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

std::string TunInterface::getIPAddress() const {
    return ipAddress_;
}

std::vector<uint8_t> TunInterface::getVirtualMacAddress() const {
    return virtualMac_;
}
