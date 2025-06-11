#include "P2PSystem.hpp"
#include "Logger.hpp"
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <signal.h>
#include <csignal>
#include <boost/stacktrace.hpp>

// Global variables
static std::atomic<bool> g_running = true;
static std::mutex g_input_mutex;
static std::unique_ptr<P2PSystem> p2pSystem;

// Signal handler for graceful shutdown
void signalHandler(int signal)
{
    g_running = false;
}

static std::string stackTraceToString()
{
    std::ostringstream oss;
    oss << boost::stacktrace::stacktrace();
    return oss.str();
}

static void onTerminate()
{
    // if there’s an active exception, try to get its what()
    if (auto eptr = std::current_exception())
    {
        try { std::rethrow_exception(eptr); }
        catch (std::exception const& ex)
        {
            SYSTEM_LOG_ERROR("Unhandled exception: {}", ex.what());
        }
        catch (...)
        {
            SYSTEM_LOG_ERROR("Unhandled non-std exception");
        }
    }
    else
    {
        SYSTEM_LOG_ERROR("Terminate called without an exception");
    }
    SYSTEM_LOG_ERROR("Stack trace:\n{}", stackTraceToString());
    std::_Exit(EXIT_FAILURE);  // immediate exit
}

// Called on fatal signals
static void onSignal(int sig)
{
    SYSTEM_LOG_ERROR("Received signal: {}", sig);
    SYSTEM_LOG_ERROR("Stack trace:\n{}", stackTraceToString());
    std::_Exit(EXIT_FAILURE);
}

void inputThreadFunc()
{
    while (g_running) {
        std::string line;
        {
            std::lock_guard<std::mutex> lock(g_input_mutex);
            std::getline(std::cin, line);
        }
        
        if (line == "/quit" || line == "/exit") {
            g_running = false;
            break;
        }
        else if (line == "/help") {
            SYSTEM_LOG_INFO("Commands:");
            SYSTEM_LOG_INFO("  /connect <username> - Connect to a peer");
            SYSTEM_LOG_INFO("  /disconnect - Disconnect from current peer");
            SYSTEM_LOG_INFO("  /accept - Accept incoming connection request");
            SYSTEM_LOG_INFO("  /reject - Reject incoming connection request");
            SYSTEM_LOG_INFO("  /status - Display connection status");
            SYSTEM_LOG_INFO("  /ip - Show current virtual IP addresses");
            SYSTEM_LOG_INFO("  /logs - Toggle logging output (default: disabled)");
            SYSTEM_LOG_INFO("  /quit or /exit - Exit the application");
            SYSTEM_LOG_INFO("  /help - Show this help message");
            clog << std::endl;
            SYSTEM_LOG_INFO("When connected, you can use standard network tools like ping or connect");
            SYSTEM_LOG_INFO("to services on the other peer using the assigned virtual IP addresses.");
        }
        else if (line.substr(0, 9) == "/connect ") {
            std::string peer = line.substr(9);
            p2pSystem->connectToPeer(peer);
        }
        else if (line == "/disconnect") {
            p2pSystem->stopConnection();
        }
        else if (line == "/accept") {
            p2pSystem->acceptIncomingRequest();
        }
        else if (line == "/reject") {
            p2pSystem->rejectIncomingRequest();
        }
        else if (line == "/status") {
            if (p2pSystem->isConnected()) {
                SYSTEM_LOG_INFO("[Status] Connected");
                SYSTEM_LOG_INFO("  Role: {}", (p2pSystem->getIsHost() ? "Host" : "Client"));
            } else {
                SYSTEM_LOG_INFO("[Status] Not connected");
            }
        }
        else if (line == "/ip") {
            if (p2pSystem->isConnected()) {
                SYSTEM_LOG_INFO("[IP] Your virtual IP: {}", (p2pSystem->getIsHost() ? "10.0.0.1" : "10.0.0.2"));
                SYSTEM_LOG_INFO("[IP] Peer virtual IP: {}", (p2pSystem->getIsHost() ? "10.0.0.2" : "10.0.0.1"));
            } else {
                SYSTEM_LOG_INFO("[IP] Not connected");
            }
        }
    }
}

int main(int argc, char* argv[])
{
    // Init logging
    initLogging();
    // Setup signal handlers
    std::set_terminate(onTerminate);

    signal(SIGINT, signalHandler);
    std::signal(SIGSEGV, onSignal);
    std::signal(SIGABRT, onSignal);
    std::signal(SIGFPE, onSignal);
    std::signal(SIGILL, onSignal);
    std::signal(SIGTERM, onSignal);
    
    // TODO: Disable network traffic logging in prod
    setShouldLogTraffic(true);
    NETWORK_TRAFFIC_LOG("TEST");

    std::string username;
    SYSTEM_LOG_INFO("Enter your username: ");
    std::getline(std::cin, username);
    if (username.empty())
    {
        std::cerr << "Username cannot be empty. Exiting." << std::endl;
        return 1;
    }
    
    const std::string serverUrl = "wss://sector-classic-ear-ecommerce.trycloudflare.com";
    int localPort = 0; // Let system automatically choose a port
    p2pSystem = std::make_unique<P2PSystem>();
    
    // Initialize the application
    if (!p2pSystem->initialize(serverUrl, username, localPort))
    {
        SYSTEM_LOG_ERROR("Failed to initialize the application. Exiting.");
        return 1;
    }
    
    SYSTEM_LOG_INFO("P2P System initialized successfully.");
    SYSTEM_LOG_INFO("Type /help for available commands.");
    
    // Start input thread
    std::thread inputThread(inputThreadFunc);
    
    // Main loop (status updates, etc.)
    while (g_running)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Cleanup - use full shutdown at program exit
    p2pSystem->shutdown();
    
    // Wait for input thread to finish
    if (inputThread.joinable())
    {
        inputThread.join();
    }
    
    SYSTEM_LOG_INFO("Application exiting. Goodbye!");
    return 0;
}