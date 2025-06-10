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
    IDLE,        // Not connected to any peer
    CONNECTING,  // In the process of establishing connection
    CONNECTED,   // Successfully connected to a peer
    SHUTTING_DOWN // System is shutting down
};

// Network event types for state transitions
enum class NetworkEvent {
    PEER_CONNECTED,         // New peer successfully connected
    ALL_PEERS_DISCONNECTED, // No active connections remain
    SHUTDOWN_REQUESTED      // External shutdown trigger
};

// Event data structure with variant for extensibility
struct NetworkEventData {
    NetworkEvent event;
    std::variant<std::string, std::monostate> data;  // String for endpoint, monostate for events without data
    std::chrono::steady_clock::time_point timestamp;
    
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
    
    // State management
    void setState(SystemState state);
    SystemState getState() const;
    bool isInState(SystemState state) const;
    
    // Event queue management
    void queueEvent(const NetworkEventData& event);
    std::optional<NetworkEventData> getNextEvent();
    bool hasEvents() const;
    
private:
    std::atomic<SystemState> current_state_;
    
    // Event queue
    std::queue<NetworkEventData> event_queue_;
    mutable std::mutex event_mutex_;
    
    // Simple state machine validation
    bool isValidTransition(SystemState from, SystemState to) const;
};

// Tracks information about a peer connection
class PeerConnectionInfo {
public:
    PeerConnectionInfo();
    
    // Activity tracking
    void updateActivity();
    bool hasTimedOut(int timeout_seconds = 10) const;
    
    // Connection state
    void setConnected(bool connected);
    bool isConnected() const;
    
    // Access last activity time for monitoring
    std::chrono::steady_clock::time_point getLastActivity() const;
    
private:
    std::atomic<std::chrono::steady_clock::time_point> last_activity_;
    std::atomic<bool> connected_;
};
