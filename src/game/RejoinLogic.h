#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tsukuyomi::rejoin {

struct ServerAddress {
    std::string host;
    std::uint16_t port = 0;
};

std::optional<ServerAddress> parseConnectionUrl(std::string_view url);
std::string readConnectionUrl(const void* client);

void* connectionHandler(void* session, const void* transfer, const void* startGame);

}
