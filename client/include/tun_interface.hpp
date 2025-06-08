#ifndef TUN_INTERFACE_H
#define TUN_INTERFACE_H

#include <string>
#include <Windows.h>
#include <wintun.h>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <mutex>
#include <queue>
#include <boost/asio.hpp>

// IP Protocol constants
constexpr uint8_t IP_PROTO_ICMP = 1;
constexpr uint8_t IP_PROTO_TCP = 6;
constexpr uint8_t IP_PROTO_UDP = 17;

// Add missing declarations needed for compilation
#ifdef __cplusplus
extern "C" {
#endif

using WINTUN_PACKET = BYTE*;

// Corrected declarations for WireGuard's Wintun API
BOOL WINAPI WintunGetAdapterLUID(_In_ WINTUN_ADAPTER_HANDLE Adapter, _Out_ NET_LUID *Luid);

#ifdef __cplusplus
}
#endif

class TunInterface {
public:
    TunInterface();
    ~TunInterface();

    // Callback types
    using PacketCallback = std::function<void(const std::vector<uint8_t>&)>;

    // Initialize the TUN interface with a specified device name
    bool initialize(const std::string& deviceName);

    // Configure the TUN interface with an IP address, netmask, and gateway
    bool configureInterface(const std::string& ipAddress, const std::string& netmask);

    // Set up routing for the TUN interface
    bool setupRouting();

    // Start and stop packet processing
    bool startPacketProcessing();
    void stopPacketProcessing();

    // Send a packet through the TUN interface
    bool sendPacket(const std::vector<uint8_t>& packet);

    // Set callback for received packets
    void setPacketCallback(PacketCallback callback);

    // Check if the interface is running
    bool isRunning() const;

    // Close and clean up the TUN interface
    void close();

    // Get the IP address of the interface
    std::string getIPAddress() const;

    // Get the local MAC address (for ARP responses)
    std::vector<uint8_t> getVirtualMacAddress() const;

    // Execute a netsh command to configure the interface
    bool executeNetshCommand(const std::string& command);

private:
    // Wintun session and adapter
    WINTUN_ADAPTER_HANDLE adapter = nullptr;
    WINTUN_SESSION_HANDLE session = nullptr;

    // Wintun function pointer types
    typedef WINTUN_ADAPTER_HANDLE (*WintunOpenAdapterFunc)(const WCHAR*);
    typedef WINTUN_ADAPTER_HANDLE (*WintunCreateAdapterFunc)(const WCHAR*, const WCHAR*, GUID*);
    typedef WINTUN_SESSION_HANDLE (*WintunStartSessionFunc)(WINTUN_ADAPTER_HANDLE, DWORD);
    typedef WINTUN_PACKET* (*WintunAllocateSendPacketFunc)(WINTUN_SESSION_HANDLE, DWORD);
    typedef void (*WintunSendPacketFunc)(WINTUN_SESSION_HANDLE, WINTUN_PACKET*);
    typedef WINTUN_PACKET* (*WintunReceivePacketFunc)(WINTUN_SESSION_HANDLE, DWORD*);
    typedef void (*WintunReleaseReceivePacketFunc)(WINTUN_SESSION_HANDLE, WINTUN_PACKET*);
    typedef void (*WintunEndSessionFunc)(WINTUN_SESSION_HANDLE);
    typedef void (*WintunCloseAdapterFunc)(WINTUN_ADAPTER_HANDLE);
    typedef BOOL (*WintunGetAdapterLUIDFunc)(WINTUN_ADAPTER_HANDLE, NET_LUID*);
    typedef BOOL (*WintunDeleteDriverFunc)(void);
    
    // Wintun function pointers
    WintunOpenAdapterFunc pWintunOpenAdapter = nullptr;
    WintunCreateAdapterFunc pWintunCreateAdapter = nullptr;
    WintunStartSessionFunc pWintunStartSession = nullptr;
    WintunAllocateSendPacketFunc pWintunAllocateSendPacket = nullptr;
    WintunSendPacketFunc pWintunSendPacket = nullptr;
    WintunReceivePacketFunc pWintunReceivePacket = nullptr;
    WintunReleaseReceivePacketFunc pWintunReleaseReceivePacket = nullptr;
    WintunEndSessionFunc pWintunEndSession = nullptr;
    WintunCloseAdapterFunc pWintunCloseAdapter = nullptr;
    WintunGetAdapterLUIDFunc pWintunGetAdapterLUID = nullptr;
    WintunDeleteDriverFunc pWintunDeleteDriver = nullptr;

    // State management
    std::atomic<bool> running_{false};
    std::mutex packetQueueMutex_;
    std::queue<std::vector<uint8_t>> outgoingPackets_;
    
    // Thread for packet processing
    std::thread receiveThread_;
    std::thread sendThread_;
    
    // Callback for received packets
    PacketCallback packetCallback_;
    
    // Configuration
    std::string ipAddress_;
    std::string netmask_;
    std::vector<uint8_t> virtualMac_;
    
    // Interface management
    bool loadWintunFunctions(HMODULE wintunModule);
    void receiveThreadFunc();
    void sendThreadFunc();
    
    // IP helper functions
    uint32_t ipToUint32(const std::string& ipAddress);
    std::string uint32ToIp(uint32_t ipAddress);
    
    // System's network interfaces
    HMODULE wintunModule_ = nullptr;
};

#endif // TUN_INTERFACE_H
