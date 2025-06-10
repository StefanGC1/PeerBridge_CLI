#pragma once
#include <mutex>
#include <chrono>
#include <atomic>

// System states
enum class SystemState {
    IDLE,        // Not connected to any peer
    CONNECTING,  // In the process of establishing connection
    CONNECTED,   // Successfully connected to a peer
    SHUTTING_DOWN // System is shutting down
};

// Manages the overall system state
class SystemStateManager {
public:
    SystemStateManager();
    
    // State management
    void setState(SystemState state);
    SystemState getState() const;
    bool isInState(SystemState state) const;
    
private:
    std::atomic<SystemState> current_state_;
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
