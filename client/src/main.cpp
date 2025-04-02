#include "chat_app.hpp"
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <signal.h>

// Global variables
static std::atomic<bool> g_running = true;
static std::mutex g_input_mutex;
static std::unique_ptr<ChatApplication> g_app;

// Signal handler for graceful shutdown
void signal_handler(int signal) {
    g_running = false;
}

void print_usage() {
    std::cout << "Usage: p2p_chat <username> [peer_username]" << std::endl;
    std::cout << "  username: Your username for the chat session" << std::endl;
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
            std::cout << "  /accept - Accept incoming chat request" << std::endl;
            std::cout << "  /reject - Reject incoming chat request" << std::endl;
            std::cout << "  /quit or /exit - Exit the application" << std::endl;
            std::cout << "  /help - Show this help message" << std::endl;
        }
        else if (line.substr(0, 9) == "/connect ") {
            std::string peer = line.substr(9);
            g_app->connectToPeer(peer);
        }
        else if (line == "/disconnect") {
            g_app->disconnect();
        }
        else if (line == "/accept") {
            g_app->acceptIncomingRequest();
        }
        else if (line == "/reject") {
            g_app->rejectIncomingRequest();
        }
        else if (!line.empty() && g_app->isConnected()) {
            g_app->sendMessage(line);
            std::cout << "You: " << line << std::endl;
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
    
    // Create and initialize chat application
    const std::string server_url = "wss://979c-188-26-252-82.ngrok-free.app"; // Change this to your server URL
    int local_port = 0; // Let the system pick an available port
    g_app = std::make_unique<ChatApplication>();
    
    // Setup callbacks
    g_app->setStatusCallback([](const std::string& status) {
        std::cout << "[Status] " << status << std::endl;
    });
    
    g_app->setMessageCallback([](const std::string& from, const std::string& message) {
        std::cout << from << ": " << message << std::endl;
    });
    
    g_app->setConnectionCallback([](bool connected, const std::string& peer) {
        if (connected) {
            std::cout << "[Chat] Connected to " << peer << std::endl;
            std::cout << "Type your messages, or use /help for commands." << std::endl;
        } else {
            std::cout << "[Chat] Disconnected from " << peer << std::endl;
        }
    });
    
    g_app->setChatRequestCallback([](const std::string& from) {
        std::cout << "[Request] " << from << " wants to chat with you." << std::endl;
        std::cout << "Type /accept to accept or /reject to decline." << std::endl;
    });
    
    // Initialize the application with the UDP networking
    if (!g_app->initialize(server_url, username, local_port)) {
        std::cerr << "Failed to initialize the application. Exiting." << std::endl;
        return 1;
    }
    
    // If peer username provided, connect to them
    if (!peer_username.empty()) {
        g_app->connectToPeer(peer_username);
    }
    
    // Start input thread
    std::thread input_thread(input_thread_func);
    
    // Main loop (status updates, etc.)
    while (g_running) {
        // In the UDP implementation, we don't need to explicitly process messages
        // as they're handled by callbacks in the background threads
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Cleanup
    g_app->disconnect();
    
    // Wait for input thread to finish
    if (input_thread.joinable()) {
        input_thread.join();
    }
    
    std::cout << "Application exiting. Goodbye!" << std::endl;
    return 0;
}