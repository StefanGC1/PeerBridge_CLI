#include "p2p_system.hpp"
#include "logger.hpp"
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <signal.h>

// Global variables
static std::atomic<bool> g_running = true;
static std::mutex g_input_mutex;
static std::unique_ptr<P2PSystem> g_system;

// Signal handler for graceful shutdown
void signal_handler(int signal) {
    g_running = false;
}

void print_usage() {
    SYSTEM_LOG_INFO("Usage: p2p_net <username> [peer_username]");
    SYSTEM_LOG_INFO("  username: Your username for the P2P connection");
    SYSTEM_LOG_INFO("  peer_username: (Optional) Username of peer to connect to");
}

void input_thread_func() {
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
            g_system->connectToPeer(peer);
        }
        else if (line == "/disconnect") {
            g_system->disconnect();
        }
        else if (line == "/accept") {
            g_system->acceptIncomingRequest();
        }
        else if (line == "/reject") {
            g_system->rejectIncomingRequest();
        }
        else if (line == "/status") {
            if (g_system->isConnected()) {
                SYSTEM_LOG_INFO("[Status] Connected");
                SYSTEM_LOG_INFO("  Role: {}", (g_system->isHost() ? "Host" : "Client"));
            } else {
                SYSTEM_LOG_INFO("[Status] Not connected");
            }
        }
        else if (line == "/ip") {
            if (g_system->isConnected()) {
                SYSTEM_LOG_INFO("[IP] Your virtual IP: {}", (g_system->isHost() ? "10.0.0.1" : "10.0.0.2"));
                SYSTEM_LOG_INFO("[IP] Peer virtual IP: {}", (g_system->isHost() ? "10.0.0.2" : "10.0.0.1"));
            } else {
                SYSTEM_LOG_INFO("[IP] Not connected");
            }
        }
    }
}

int main(int argc, char* argv[]) {
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // TODO: Disable network traffic logging in prod
    initLogging();
    setShouldLogTraffic(true);
    NETWORK_TRAFFIC_LOG("TEST");

    std::string username;
    SYSTEM_LOG_INFO("Enter your username: ");
    std::getline(std::cin, username);
    if (username.empty()) {
        std::cerr << "Username cannot be empty. Exiting." << std::endl;
        return 1;
    }
    std::string peer_username = "";
    
    const std::string server_url = "wss://sector-classic-ear-ecommerce.trycloudflare.com";
    int local_port = 0; // Let system automatically choose a port
    g_system = std::make_unique<P2PSystem>();
    
    // Initialize the application
    if (!g_system->initialize(server_url, username, local_port)) {
        SYSTEM_LOG_ERROR("Failed to initialize the application. Exiting.");
        return 1;
    }
    
    SYSTEM_LOG_INFO("P2P System initialized successfully.");
    SYSTEM_LOG_INFO("Type /help for available commands.");
    
    // Start input thread
    std::thread input_thread(input_thread_func);
    
    // Main loop (status updates, etc.)
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Cleanup
    g_system->disconnect();
    
    // Wait for input thread to finish
    if (input_thread.joinable()) {
        input_thread.join();
    }
    
    SYSTEM_LOG_INFO("Application exiting. Goodbye!");
    return 0;
}