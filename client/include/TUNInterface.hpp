#pragma once

#include <string>
#include <Windows.h>
#include <wintun.h>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <boost/asio.hpp>

#ifdef __cplusplus
extern "C" {
#endif

using WINTUN_PACKET = BYTE*;

// TODO: Test if removing this is ok
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

    // Initialize TUN adapter with a device name
    bool initialize(const std::string&);

    // Start and stop packet processing
    bool startPacketProcessing();
    void stopPacketProcessing();

    // Add a packet to injection queue
    bool sendPacket(std::vector<uint8_t>);

    // Set callback for extracted packets
    void setPacketCallback(PacketCallback callback);

    // Check if the interface is running
    bool isRunning() const;

    // Close and clean up the TUN interface
    void close();

    // Getter for adapter alias
    // We use this to make sure we get the exact name of the adapter
    std::string getNarrowAlias() const;

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
    typedef HANDLE (*WintunGetReadWaitEventFunc)(WINTUN_SESSION_HANDLE);
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
    WintunGetReadWaitEventFunc pWintunGetReadWaitEvent = nullptr;
    WintunDeleteDriverFunc pWintunDeleteDriver = nullptr;

    // State management
    std::atomic<bool> running{false};
    std::mutex packetQueueMutex;
    std::condition_variable packetConditionVariable;
    std::queue<std::vector<uint8_t>> outgoingPackets;
    
    // Thread for packet processing
    std::thread receiveThread;
    std::thread sendThread;
    
    // Callback for received packets
    PacketCallback packetCallback;
    
    // Interface management
    bool loadWintunFunctions(HMODULE);
    void receiveThreadFunc();
    void sendThreadFunc();
    
    // System's network interfaces
    HMODULE wintunModule = nullptr;
};
