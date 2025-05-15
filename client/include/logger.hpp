#pragma once
#include <iostream>
#include <sstream>
#include <mutex>
#include <string>

class ConditionalLogger {
public:
    // Singleton pattern to ensure global control
    static ConditionalLogger& getInstance() {
        static ConditionalLogger instance;
        return instance;
    }

    // Enable/disable logging
    void setLoggingEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(logMutex_);
        loggingEnabled_ = enabled;
    }

    // Get logging status
    bool isLoggingEnabled() const {
        return loggingEnabled_;
    }

    // Toggle logging status
    bool toggleLogging() {
        std::lock_guard<std::mutex> lock(logMutex_);
        loggingEnabled_ = !loggingEnabled_;
        return loggingEnabled_;
    }

    // Stream operator overloads to behave like std::cout (default level INFO)
    template<typename T>
    ConditionalLogger& operator<<(const T& data) {
        if (loggingEnabled_) {
            std::lock_guard<std::mutex> lock(logMutex_);
            std::cout << data;
        }
        return *this;
    }

    // Special handling for manipulators like std::endl
    ConditionalLogger& operator<<(std::ostream& (*manip)(std::ostream&)) {
        if (loggingEnabled_) {
            std::lock_guard<std::mutex> lock(logMutex_);
            std::cout << manip;
        }
        return *this;
    }

private:
    ConditionalLogger() : loggingEnabled_(false){} // Disabled by default
    
    // Prevent copying and assignment
    ConditionalLogger(const ConditionalLogger&) = delete;
    ConditionalLogger& operator=(const ConditionalLogger&) = delete;

    bool loggingEnabled_;
    std::mutex logMutex_;
};

// Global instances for easy access
#define clog ConditionalLogger::getInstance()