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
    std::cout << "Usage: p2p_net <username> [peer_username]" << std::endl;
    std::cout << "  username: Your username for the P2P connection" << std::endl;
    std::cout << "  peer_username: (Optional) Username of peer to connect to" << std::endl;
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
            std::cout << "Commands:" << std::endl;
            std::cout << "  /connect <username> - Connect to a peer" << std::endl;
            std::cout << "  /disconnect - Disconnect from current peer" << std::endl;
            std::cout << "  /accept - Accept incoming connection request" << std::endl;
            std::cout << "  /reject - Reject incoming connection request" << std::endl;
            std::cout << "  /status - Display connection status" << std::endl;
            std::cout << "  /ip - Show current virtual IP addresses" << std::endl;
            std::cout << "  /logs - Toggle logging output (default: disabled)" << std::endl;
            std::cout << "  /logs level - Cycle through log levels (debug, info, warning, error, none)" << std::endl;
            std::cout << "  /logs level <level> - Set log level to specified value" << std::endl;
            std::cout << "  /quit or /exit - Exit the application" << std::endl;
            std::cout << "  /help - Show this help message" << std::endl;
            std::cout << std::endl;
            std::cout << "When connected, you can use standard network tools like ping or connect" << std::endl;
            std::cout << "to services on the other peer using the assigned virtual IP addresses." << std::endl;
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
            if (enabled) {
                std::cout << "[System] Current log level: " << clog.getLogLevelName() << std::endl;
            }
        }
        else if (line == "/logs level") {
            if (!clog.isLoggingEnabled()) {
                clog.setLoggingEnabled(true);
                std::cout << "[System] Logging enabled" << std::endl;
            }
            std::string newLevel = clog.cycleLogLevel();
            std::cout << "[System] Log level set to: " << newLevel << std::endl;
        }
        else if (line.substr(0, 11) == "/logs level ") {
            std::string levelStr = line.substr(11);
            LogLevel level = LogLevel::INFO;
            
            if (levelStr == "debug") {
                level = LogLevel::DEBUG;
            } else if (levelStr == "info") {
                level = LogLevel::INFO; 
            } else if (levelStr == "warning") {
                level = LogLevel::WARNING;
            } else if (levelStr == "error") {
                level = LogLevel::ERROR;
            } else if (levelStr == "none") {
                level = LogLevel::NONE;
            } else {
                std::cout << "[Error] Unknown log level: " << levelStr << std::endl;
                std::cout << "  Available levels: debug, info, warning, error, none" << std::endl;
                continue;
            }
            
            if (!clog.isLoggingEnabled()) {
                clog.setLoggingEnabled(true);
                std::cout << "[System] Logging enabled" << std::endl;
            }
            
            clog.setLogLevel(level);
            std::cout << "[System] Log level set to: " << clog.getLogLevelName() << std::endl;
        }
    }
}

int main(int argc, char* argv[]) {
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Parse command line arguments
    if (argc < 2 || argc > 3) {
        print_usage();
        return 1;
    }
    
    std::string username = argv[1];
    std::string peer_username = "";
    
    if (argc == 3) {
        peer_username = argv[2];
    }
    
    const std::string server_url = "wss://2f31-86-125-92-244.ngrok-free.app"; // Change this to your server URL
    int local_port = 0; // Let the system pick an available port
    g_system = std::make_unique<P2PSystem>();
    
    // Setup callbacks
    g_system->setStatusCallback([](const std::string& status) {
        std::cout << "[Status] " << status << std::endl;
    });
    
    g_system->setConnectionCallback([](bool connected, const std::string& peer) {
        if (connected) {
            std::cout << "[System] Connected to " << peer << std::endl;
            std::cout << "Virtual network is now active. You can use standard networking tools (ping, etc.)" << std::endl;
        } else {
            std::cout << "[System] Disconnected from " << peer << std::endl;
        }
    });
    
    g_system->setConnectionRequestCallback([](const std::string& from) {
        std::cout << "[Request] " << from << " wants to connect with you." << std::endl;
        std::cout << "Type /accept to accept or /reject to decline." << std::endl;
    });
    
    // Initialize the application
    if (!g_system->initialize(server_url, username, local_port)) {
        std::cerr << "Failed to initialize the application. Exiting." << std::endl;
        return 1;
    }
    
    // If peer username provided, connect to them
    if (!peer_username.empty()) {
        g_system->connectToPeer(peer_username);
    }
    
    std::cout << "P2P System initialized successfully." << std::endl;
    std::cout << "Type /help for available commands." << std::endl;
    
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
    
    std::cout << "Application exiting. Goodbye!" << std::endl;
    return 0;
}