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
    clog << "Usage: p2p_net <username> [peer_username]" << std::endl;
    clog << "  username: Your username for the P2P connection" << std::endl;
    clog << "  peer_username: (Optional) Username of peer to connect to" << std::endl;
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
            clog << "Commands:" << std::endl;
            clog << "  /connect <username> - Connect to a peer" << std::endl;
            clog << "  /disconnect - Disconnect from current peer" << std::endl;
            clog << "  /accept - Accept incoming connection request" << std::endl;
            clog << "  /reject - Reject incoming connection request" << std::endl;
            clog << "  /status - Display connection status" << std::endl;
            clog << "  /ip - Show current virtual IP addresses" << std::endl;
            clog << "  /logs - Toggle logging output (default: disabled)" << std::endl;
            clog << "  /quit or /exit - Exit the application" << std::endl;
            clog << "  /help - Show this help message" << std::endl;
            clog << std::endl;
            clog << "When connected, you can use standard network tools like ping or connect" << std::endl;
            clog << "to services on the other peer using the assigned virtual IP addresses." << std::endl;
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
                std::cout << "[Status] Connected" << std::endl;
                std::cout << "  Role: " << (g_system->isHost() ? "Host" : "Client") << std::endl;
            } else {
                std::cout << "[Status] Not connected" << std::endl;
            }
        }
        else if (line == "/ip") {
            if (g_system->isConnected()) {
                std::cout << "[IP] Your virtual IP: " << (g_system->isHost() ? "10.0.0.1" : "10.0.0.2") << std::endl;
                std::cout << "[IP] Peer virtual IP: " << (g_system->isHost() ? "10.0.0.2" : "10.0.0.1") << std::endl;
            } else {
                std::cout << "[IP] Not connected" << std::endl;
            }
        }
        else if (line == "/logs") {
            bool enabled = clog.toggleLogging();
            std::cout << "[System] Logging " << (enabled ? "enabled" : "disabled") << std::endl;
        }
    }
}

int main(int argc, char* argv[]) {
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    initLogging();

    SYSTEM_LOG_INFO("LOGGER GET INFO, FMT TEST PARAM {}", 5);
    SYSTEM_LOG_WARNING("LOGGER GET WARNING, FMT TEST PARAM {}", 5);
    SYSTEM_LOG_ERROR("LOGGER GET ERROR, FMT TEST PARAM {}", 5);

    std::string username;
    std::cout << "Enter your username: " << std::endl;
    std::getline(std::cin, username);
    if (username.empty()) {
        std::cerr << "Username cannot be empty. Exiting." << std::endl;
        return 1;
    }
    std::string peer_username = "";
    
    const std::string server_url = "wss://striking-washer-hist-range.trycloudflare.com";
    int local_port = 0; // Let system automatically choose a port
    g_system = std::make_unique<P2PSystem>();
    
    // Setup callbacks
    g_system->setStatusCallback([](const std::string& status) {
        clog << "[Status] " << status << std::endl;
    });
    
    g_system->setConnectionCallback([](bool connected, const std::string& peer) {
        if (connected) {
            clog << "[System] Connected to " << peer << std::endl;
            clog << "Virtual network is now active. You can use standard networking tools (ping, etc.)" << std::endl;
        } else {
            clog << "[System] Disconnected from " << peer << std::endl;
        }
    });
    
    g_system->setConnectionRequestCallback([](const std::string& from) {
        clog << "[Request] " << from << " wants to connect with you." << std::endl;
        clog << "Type /accept to accept or /reject to decline." << std::endl;
    });
    
    // Initialize the application
    if (!g_system->initialize(server_url, username, local_port)) {
        std::cerr << "Failed to initialize the application. Exiting." << std::endl;
        return 1;
    }
    
    clog << "P2P System initialized successfully." << std::endl;
    clog << "Type /help for available commands." << std::endl;
    
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
    
    clog << "Application exiting. Goodbye!" << std::endl;
    return 0;
}