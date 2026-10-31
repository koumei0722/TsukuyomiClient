#include "game/RejoinLogic.h"
#include "memory/Memory.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tsukuyomi::rejoin {

std::string readConnectionUrl(const void* client)
{
    if (!memory::plausiblePointer(client)) return {};
    std::uintptr_t words[4]{};
    if (!memory::copyGuarded(static_cast<const std::byte*>(client) + 0x720, words, sizeof(words))) return {};
    const auto size = words[2], capacity = words[3];
    if (size < 10 || size > 512 || size > capacity || capacity < 15) return {};
    std::string url(size, '\0');
    if (capacity == 15) std::memcpy(url.data(), words, size);
    else if (!memory::plausiblePointer(words[0])
        || !memory::copyGuarded(reinterpret_cast<const void*>(words[0]), url.data(), size)) return {};
    return parseConnectionUrl(url) ? url : std::string{};
}

void* connectionHandler(void* session, const void* transfer, const void* startGame)
{
    if (!memory::plausiblePointer(session) || !transfer || !startGame) return nullptr;
    const auto* field = static_cast<const std::byte*>(session) + 0x50;
    void* handler = nullptr;
    if (!memory::isReadable(field, sizeof(handler)) || !memory::copyGuarded(field, &handler, sizeof(handler))
        || !memory::plausiblePointer(handler) || !memory::isReadable(handler, sizeof(void*))) return nullptr;
    const std::byte* table = nullptr;
    constexpr std::size_t kTransferSlot = 138, kStartGameSlot = 243;
    if (!memory::copyGuarded(handler, &table, sizeof(table)) || !memory::inGameModule(table)
        || !memory::isReadable(table, (kStartGameSlot + 1) * sizeof(void*))) return nullptr;
    const void* transferSlot = nullptr;
    const void* startGameSlot = nullptr;
    if (!memory::copyGuarded(table + kTransferSlot * sizeof(void*), &transferSlot, sizeof(transferSlot))
        || !memory::copyGuarded(table + kStartGameSlot * sizeof(void*), &startGameSlot, sizeof(startGameSlot))
        || transferSlot != transfer || startGameSlot != startGame) return nullptr;
    return handler;
}

std::optional<ServerAddress> parseConnectionUrl(std::string_view url)
{
    for (std::string_view scheme : {std::string_view("http://"), std::string_view("https://")}) {
        if (url.starts_with(scheme)) {
            url.remove_prefix(scheme.size());
            break;
        }
        if (scheme == "https://") return std::nullopt;
    }
    if (const auto path = url.find('/'); path != std::string_view::npos) url = url.substr(0, path);
    std::string_view host, port;
    if (url.starts_with('[')) {
        const auto close = url.find(']');
        if (close == std::string_view::npos || close + 1 >= url.size() || url[close + 1] != ':') return std::nullopt;
        host = url.substr(1, close - 1);
        port = url.substr(close + 2);
    } else {
        const auto colon = url.rfind(':');
        if (colon == std::string_view::npos || url.find(':') != colon) return std::nullopt;
        host = url.substr(0, colon);
        port = url.substr(colon + 1);
    }
    if (host.empty() || port.empty() || port.size() > 5) return std::nullopt;
    unsigned value = 0;
    for (const char c : port) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value == 0 || value > 65535) return std::nullopt;
    for (const char c : host) {
        if (static_cast<unsigned char>(c) <= ' ' || c == '/' || c == '@') return std::nullopt;
    }
    return ServerAddress{std::string(host), static_cast<std::uint16_t>(value)};
}

}
