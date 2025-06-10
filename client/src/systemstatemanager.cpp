#include "systemstatemanager.hpp"

SystemStateManager::SystemStateManager() : current_state_(SystemState::IDLE) {}

void SystemStateManager::setState(SystemState state) {
    current_state_.store(state, std::memory_order_release);
}

SystemState SystemStateManager::getState() const {
    return current_state_.load(std::memory_order_acquire);
}

bool SystemStateManager::isInState(SystemState state) const {
    return current_state_.load(std::memory_order_acquire) == state;
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