#include <iostream>
#include <string>
#include <boost/asio.hpp>
#include "crypto.hpp"
#include "signaling.hpp"
#include "stun.hpp"
#include <sodium.h>
#include <nlohmann/json.hpp>
#include <networking.hpp>

int main()
{
    // Initialize libsodium
    if (init_crypto() < 0)
    {
        std::cerr << "Libsodium init failed.\n";
        return 1;
    }

    // Use STUN to get NAT info (public IP + port)
    std::optional<PublicAddress> clientAddress = get_public_address();
    if (!clientAddress)
    {
        std::cerr << "Catastrophic failure, couldn't get public IP.\n";
        return 1;
    }

    const auto [ip, port] = *clientAddress;
    std::cout << "IP and Port: " << ip << ":" << port << std::endl;

    // Initialize websocket with backend
    std::string url = "wss://461c-188-26-252-82.ngrok-free.app";
    init_socket(url);

    // Register user
    std::string username;
    std::cout << "Enter your username: ";
    std::getline(std::cin, username);
    register_user(username, ip, port);

    // Start p2p socket
    start_p2p_listener(port);

    // Main event loop
    while (true)
    {
        std::string input;
        std::getline(std::cin, input);
    
        if (waiting_for_chat_init)
        {
            std::cout << "[System] Waiting for chat response...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        if (in_p2p_chat)
        {
            if (input == "/leave")
            {
                if (in_p2p_chat)
                {
                    leave_chat();
                    in_p2p_chat = false;
                    waiting_for_chat_init = false;
                } else
                {
                    std::cout << "[System] You're not in a chat.\n";
                }
                continue;
            }
            send_chat_message(input);
        }

        if (input == "/hello")
        {
            send_greeting();
            continue;
        }

        else if (input == "/get-own-name")
        {
            request_username();
            continue;
        }

        else if (input.rfind("/get-peer ", 0) == 0)
        {
            std::string target = input.substr(10);
            request_peer_info(target);
            continue;
        }

        else if (input.rfind("/chat ", 0) == 0) {
            std::string target = input.substr(6);
            std::cout << target << std::endl;

            send_chat_request(target);
            waiting_for_chat_init = true;
            std::cout << "[System] Request sent to chat with " << target << "\n";
            continue;
        }

        else if (input == "/exit")
        {
            break;
        }
    }

    leave_chat();
    std::cout << "Stefan";
    return 0;
}
