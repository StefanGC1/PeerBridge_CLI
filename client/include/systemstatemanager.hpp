#pragma once
#include <mutex>
#include <chrono>
#include <atomic>
#include <queue>
#include <optional>
#include <variant>
#include <string>

// System states
enum class SystemState {
    IDLE,
    CONNECTING,
    CONNECTED,
    SHUTTING_DOWN
};

// Network event types, used for transitions
enum class NetworkEvent {
    PEER_CONNECTED,
    ALL_PEERS_DISCONNECTED,
    SHUTDOWN_REQUESTED
};

// Event data to be sent by the network module
struct NetworkEventData {
    NetworkEvent event;
    std::variant<std::string, std::monostate> data;
    std::chrono::steady_clock::time_point timestamp; // UNUSED
    
    // Constructor for events with string data
    NetworkEventData(NetworkEvent e, const std::string& endpoint) 
        : event(e), data(endpoint), timestamp(std::chrono::steady_clock::now()) {}
    
    // Constructor for events without data
    NetworkEventData(NetworkEvent e) 
        : event(e), data(std::monostate{}), timestamp(std::chrono::steady_clock::now()) {}
};

// Manages the overall system state
class SystemStateManager {
public:
    SystemStateManager();

    // System state management
    void setState(SystemState state);
    SystemState getState() const;
    bool isInState(SystemState state) const;

    // Event queue
    void queueEvent(const NetworkEventData& event);
    std::optional<NetworkEventData> getNextEvent();
    bool hasEvents() const;
    
private:
    std::atomic<SystemState> currentState;

    std::queue<NetworkEventData> eventQueue;
    mutable std::mutex eventMutex;

    bool isValidTransition(SystemState from, SystemState to) const;
};

// Tracks information about a peer connection
class PeerConnectionInfo {
public:
    PeerConnectionInfo();
    
    // Last active time (receive timestamp)
    void updateActivity();
    bool hasTimedOut(int = 10) const;
    
    // Connection state
    void setConnected(bool);
    bool isConnected() const;
    
    // Access last activity time for monitoring
    std::chrono::steady_clock::time_point getLastActivity() const;
    
private:
    std::atomic<std::chrono::steady_clock::time_point> lastActivity;
    std::atomic<bool> connected;
};
