#pragma once
#include <iostream>
#include <sstream>
#include <mutex>
#include <string>

class ConditionalLogger {
public:
    // Singleton
    static ConditionalLogger& getInstance() {
        static ConditionalLogger instance;
        return instance;
    }

    void setLoggingEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(logMutex_);
        loggingEnabled_ = enabled;
    }

    bool isLoggingEnabled() const {
        return loggingEnabled_;
    }

    // Toggle logging status
    bool toggleLogging() {
        std::lock_guard<std::mutex> lock(logMutex_);
        loggingEnabled_ = !loggingEnabled_;
        return loggingEnabled_;
    }

    // Stream operator overloads
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
    ConditionalLogger() : loggingEnabled_(true){}
    
    ConditionalLogger(const ConditionalLogger&) = delete;
    ConditionalLogger& operator=(const ConditionalLogger&) = delete;

    bool loggingEnabled_;
    std::mutex logMutex_;
};

// Global instances for easy access
#define clog ConditionalLogger::getInstance()

#include <quill/Logger.h>
#include <quill/LogMacros.h>

void initLogging();
quill::Logger* sysLogger();
quill::Logger* netLogger();

#define SYSTEM_LOG_INFO(fmt, ...) QUILL_LOG_INFO(sysLogger(), fmt, ##__VA_ARGS__)
#define SYSTEM_LOG_WARNING(fmt, ...) QUILL_LOG_WARNING(sysLogger(), fmt, ##__VA_ARGS__)
#define SYSTEM_LOG_ERROR(fmt, ...) QUILL_LOG_ERROR(sysLogger(), fmt, ##__VA_ARGS__)

#define NETWORK_LOG_INFO(fmt, ...) QUILL_LOG_INFO(netLogger(), fmt, ##__VA_ARGS__)
#define NETWORK_LOG_WARNING(fmt, ...) QUILL_LOG_WARNING(netLogger(), fmt, ##__VA_ARGS__)
#define NETWORK_LOG_ERROR(fmt, ...) QUILL_LOG_ERROR(netLogger(), fmt, ##__VA_ARGS__)