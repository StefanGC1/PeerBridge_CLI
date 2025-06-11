#include "SystemStateManager.hpp"
#include "Logger.hpp"

SystemStateManager::SystemStateManager() : currentState(SystemState::IDLE) {}

bool SystemStateManager::isValidTransition(SystemState from, SystemState to) const
{
    // Limit possible transitions
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
            return to == SystemState::SHUTTING_DOWN;
            
        default:
            return false;
    }
}

void SystemStateManager::setState(SystemState newState)
{
    SystemState current = currentState.load();

    if (!isValidTransition(current, newState))
    {
        SYSTEM_LOG_WARNING("[StateManager] Invalid transition from {} to {}", 
                          static_cast<int>(current), static_cast<int>(newState));
        return;
    }
    
    currentState.store(newState, std::memory_order_release);
    SYSTEM_LOG_INFO("[StateManager] State transition: {} -> {}", 
                    static_cast<int>(current), static_cast<int>(newState));
}

SystemState SystemStateManager::getState() const
{
    return currentState.load(std::memory_order_acquire);
}

bool SystemStateManager::isInState(SystemState state) const
{
    return currentState.load(std::memory_order_acquire) == state;
}

void SystemStateManager::queueEvent(const NetworkEventData& event)
{
    SYSTEM_LOG_INFO("[StateManager] Queuing event: {}", static_cast<int>(event.event));
    std::lock_guard<std::mutex> lock(eventMutex);
    eventQueue.push(event);
}

std::optional<NetworkEventData> SystemStateManager::getNextEvent()
{
    std::lock_guard<std::mutex> lock(eventMutex);
    if (eventQueue.empty())
    {
        return std::nullopt;
    }
    
    SYSTEM_LOG_INFO("[StateManager] Getting next event");
    NetworkEventData event = eventQueue.front();
    eventQueue.pop();
    return event;
}

bool SystemStateManager::hasEvents() const
{
    std::lock_guard<std::mutex> lock(eventMutex);
    return !eventQueue.empty();
}

PeerConnectionInfo::PeerConnectionInfo() : connected(false)
{
    updateActivity();
}

void PeerConnectionInfo::updateActivity()
{
    lastActivity.store(std::chrono::steady_clock::now(), std::memory_order_release);
}

bool PeerConnectionInfo::hasTimedOut(int timeoutSeconds) const
{
    auto lastActivity_ = lastActivity.load(std::memory_order_acquire);
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastActivity_).count();
    return (elapsed > timeoutSeconds) && connected.load(std::memory_order_acquire);
}

void PeerConnectionInfo::setConnected(bool connected_)
{
    connected.store(connected_, std::memory_order_release);
    if (connected_)
    {
        updateActivity();
    }
}

bool PeerConnectionInfo::isConnected() const {
    return connected.load(std::memory_order_acquire);
}

std::chrono::steady_clock::time_point PeerConnectionInfo::getLastActivity() const {
    return lastActivity.load(std::memory_order_acquire);
} 