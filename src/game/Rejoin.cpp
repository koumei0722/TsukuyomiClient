#include "game/Rejoin.h"

#include "core/Logger.h"
#include "core/Strings.h"
#include "game/ChatCommand.h"
#include "game/GameData.h"
#include "game/RejoinLogic.h"
#include "hooks/Detours.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <new>
#include <string>
#include <string_view>

namespace tsukuyomi::rejoin {
namespace {

using TransferHandleFn = void(__fastcall*)(void* handler, const void* identifier, const void* packet);

constexpr std::size_t kPacketAddress = 0x30;
constexpr std::size_t kPacketPort = 0x50;
constexpr std::size_t kPacketReload = 0x52;
constexpr std::size_t kPacketBytes = 0x200;
struct alignas(16) FakePacket { std::byte bytes[kPacketBytes]{}; };

std::atomic<void*> g_session{nullptr};
bool g_resolved = false;
TransferHandleFn g_transfer = nullptr;

void resolve()
{
    if (g_resolved) return;
    g_resolved = true;
    const Scanner& scanner = Scanner::instance();
    if (!scanner.found(Target::TransferHandle) || !scanner.found(Target::StartGameHandle)) {
        log().warn(L"Rejoin: the connection handlers were not found");
        return;
    }
    g_transfer = reinterpret_cast<TransferHandleFn>(const_cast<std::byte*>(scanner.address(Target::TransferHandle)));
}

int callGuarded(void* handler, const void* identifier, const void* packet)
{
    __try {
        g_transfer(handler, identifier, packet);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

std::optional<ServerAddress> currentServer()
{
    auto address = parseConnectionUrl(readConnectionUrl(hooks::gameClientInstance()));
    if (address) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            log().info(L"Rejoin: the connection URL is at client +{:#x}", 0x720);
        }
    }
    return address;
}

chatcommand::Reply run()
{
    resolve();
    if (!g_transfer) return {{"Rejoin: unavailable in this game version"}, true};
    void* handler = connectionHandler(g_session.load(std::memory_order_acquire), reinterpret_cast<const void*>(g_transfer),
        Scanner::instance().address(Target::StartGameHandle));
    if (!handler) return {{"Rejoin: the game's connection was not found"}, true};
    const bool local = GameData::instance().integratedServer();
    std::optional<ServerAddress> server;
    if (!local) {
        server = currentServer();
        if (!server) return {{"Rejoin: the server address is unknown (rejoining is supported in your local world and on servers)"}, true};
    }
    FakePacket packet;
    auto* address = new (packet.bytes + kPacketAddress) std::string(server ? server->host : std::string());
    const std::uint16_t port = server ? server->port : 0;
    std::memcpy(packet.bytes + kPacketPort, &port, sizeof(port));
    packet.bytes[kPacketReload] = std::byte{local ? 1u : 0u};
    alignas(16) std::byte identifier[0x100]{};
    const int result = callGuarded(handler, identifier, packet.bytes);
    address->~basic_string();
    if (result != 0) {
        log().warn(L"Rejoin: the transfer handler faulted");
        return {{"Rejoin: the game failed to start rejoining"}, true};
    }
    if (local) {
        log().info(L"Rejoin: reloading the local world");
        return {{"Rejoin: reloading the world"}, false};
    }
    log().info(L"Rejoin: rejoining {}:{}", toUtf16(server->host), server->port);
    return {{std::format("Rejoin: rejoining {}:{}", server->host, server->port)}, false};
}

}

void noteClientSession(void* session)
{
    g_session.store(session, std::memory_order_release);
}

void registerCommand()
{
    chatcommand::registerRawCommand({"rejoin"}, "/tk rejoin", [](const auto&, const auto&) { return run(); }, {}, true);
}

}
