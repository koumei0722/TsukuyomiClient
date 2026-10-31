#pragma once
#include "BotProtocol.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tsukuyomi::bot::relog {

struct ServerAddress { std::string host; std::uint16_t port = 0; };

std::string transfer(const ServerAddress& address, bool reloadWorld);

std::optional<ServerAddress> parseServerAddress(std::string_view text);

std::optional<ServerAddress> readLoginServerAddress(protocol::Bytes body);

struct LoginParts { std::int32_t protocolVersion = 0; std::string request, loginJson, clientData; };
std::optional<LoginParts> splitLogin(protocol::Bytes body);

std::string clientDataKey(const LoginParts&);
std::optional<std::string> replaceLoginToken(protocol::Bytes body, const std::string& token);

}
