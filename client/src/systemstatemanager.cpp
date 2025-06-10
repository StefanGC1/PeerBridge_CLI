#include "systemstatemanager.hpp"
#include "logger.hpp"

SystemStateManager::SystemStateManager() : current_state_(SystemState::IDLE) {}

bool SystemStateManager::isValidTransition(SystemState from, SystemState to) const {
    // Simple validation rules
    switch (from) {
        case SystemState::IDLE:
            return (
                to == SystemState::IDLE ||
                to == SystemState::CONNECTING ||
                to == SystemState::SHUTTING_DOWN);
            
        case SystemState::CONNECTING:
            return (
                to == SystemState::CONNECTED ||
                to == SystemState::IDLE ||
                to == SystemState::SHUTTING_DOWN);
            
        case SystemState::CONNECTED:
            return (
                to == SystemState::CONNECTED ||
                to == SystemState::IDLE ||
                to == SystemState::SHUTTING_DOWN);
            
        case SystemState::SHUTTING_DOWN:
            return to == SystemState::SHUTTING_DOWN;  // Terminal state
            
        default:
            return false;
    }
}

void SystemStateManager::setState(SystemState new_state) {
    SystemState current = current_state_.load();
    
    if (!isValidTransition(current, new_state)) {
        SYSTEM_LOG_WARNING("[StateManager] Invalid transition from {} to {}", 
                          static_cast<int>(current), static_cast<int>(new_state));
        return;
    }
    
    current_state_.store(new_state, std::memory_order_release);
    SYSTEM_LOG_INFO("[StateManager] State transition: {} -> {}", 
                    static_cast<int>(current), static_cast<int>(new_state));
}

SystemState SystemStateManager::getState() const {
    return current_state_.load(std::memory_order_acquire);
}

bool SystemStateManager::isInState(SystemState state) const {
    return current_state_.load(std::memory_order_acquire) == state;
}

void SystemStateManager::queueEvent(const NetworkEventData& event) {
    SYSTEM_LOG_INFO("[StateManager] Queuing event: {}", static_cast<int>(event.event));
    std::lock_guard<std::mutex> lock(event_mutex_);
    event_queue_.push(event);
}

std::optional<NetworkEventData> SystemStateManager::getNextEvent() {
    std::lock_guard<std::mutex> lock(event_mutex_);
    if (event_queue_.empty()) {
        return std::nullopt;
    }
    
    SYSTEM_LOG_INFO("[StateManager] Getting next event");
    NetworkEventData event = event_queue_.front();
    event_queue_.pop();
    return event;
}

bool SystemStateManager::hasEvents() const {
    std::lock_guard<std::mutex> lock(event_mutex_);
    return !event_queue_.empty();
}

// PeerConnectionInfo implementation
PeerConnectionInfo::PeerConnectionInfo() : connected_(false) {
    updateActivity(); // Initialize last activity time
}

void PeerConnectionInfo::updateActivity() {
    last_activity_.store(std::chrono::steady_clock::now(), std::memory_order_release);
}

bool PeerConnectionInfo::hasTimedOut(int timeout_seconds) const {
    auto last_activity = last_activity_.load(std::memory_order_acquire);
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_activity).count();
    return (elapsed > timeout_seconds) && connected_.load(std::memory_order_acquire);
}

void PeerConnectionInfo::setConnected(bool connected) {
    connected_.store(connected, std::memory_order_release);
    if (connected) {
        updateActivity();
    }
}

bool PeerConnectionInfo::isConnected() const {
    return connected_.load(std::memory_order_acquire);
}

std::chrono::steady_clock::time_point PeerConnectionInfo::getLastActivity() const {
    return last_activity_.load(std::memory_order_acquire);
} 