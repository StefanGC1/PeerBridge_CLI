#pragma once
#include <ixwebsocket/IXWebSocket.h>
#include <memory>
#include <atomic>
#include <nlohmann/json.hpp>

static std::unique_ptr<ix::WebSocket> ws;
static std::atomic<bool> connected = false;

void init_socket(const std::string&);

void register_user(const std::string&, const std::string&, int);
void request_peer_info(const std::string&);