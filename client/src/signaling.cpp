#include "networking.hpp"
#include "signaling.hpp"
#include <iostream>

using json = nlohmann::json;

static std::mutex cin_mutex;

void init_socket(const std::string& server_url)
{
    ws = std::make_unique<ix::WebSocket>();
    ws->setUrl(server_url);

    ws->setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg)
    {
        if (msg->type == ix::WebSocketMessageType::Message)
        {
            try
            {
                auto data = json::parse(msg->str);
                std::string type = data.value("type", "");
    
                if (type == "greet-back")
                {
                    std::cout << "[Server -> Client] " << data["message"] << std::endl;
                }

                else if (type == "register-ack")
                {
                    std::cout << "[Server -> Client] " << data["message"] << std::endl;
                }

                else if (type == "peer-info")
                {
                    std::string peerName = data["username"];
                    std::string ip = data["ip"];
                    int port = data["port"];
                    std::cout << "[Server] Peer " << peerName << " is at " << ip << ":" << port << std::endl;
                }

                else if (type == "error")
                {
                    std::cout << "[Server ERROR] " << data["message"] << std::endl;
                }
                else
                {
                    std::cout << "[Server] Unexpected msg:" << msg->str << std::endl;
                }
            }
            catch (...)
            {
                std::cout << "[Server] (unparsed): " << msg->str << std::endl;
            }
        }
        else if (msg->type == ix::WebSocketMessageType::Open)
        {
            std::cout << "[Client] Connected to server." << std::endl;
            connected = true;
        }
        else if (msg->type == ix::WebSocketMessageType::Close)
        {
            std::cout << "[Client] Connection closed." << std::endl;
            connected = false;
        }
        else if (msg->type == ix::WebSocketMessageType::Error)
        {
            std::cerr << "[Client] Connection error: " << msg->errorInfo.reason << std::endl;
            connected = false;
        }
    });

    ws->start();

    int retries = 50;
    for (int i = 0; i < retries && !connected; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!connected)
    {
        std::cerr << "[Client] Failed to connect to server after timeout. Exiting.\n";
        ws->stop();
        std::exit(1);
    }
}

void register_user(const std::string& username, const std::string& ip, int port)
{
    if (ws && connected)
    {
        json js =
        {
            {"type", "register"},
            {"username", username},
            {"ip", ip},
            {"port", port}
        };
        ws->send(js.dump());
    }
    else
    {
        std::cout << "[Client] Not connected.\n";
    }
}

void request_peer_info(const std::string& username)
{
    if (ws && connected)
    {
        json j = {
            {"type", "get-peer"},
            {"username", username}
        };
        ws->send(j.dump());
    }
}

