#pragma once
#include <cstdint>
#include <optional>

namespace tsukuyomi { class HookManager; }
namespace tsukuyomi::chatcommand { struct TkCommandSpec; }

namespace tsukuyomi::bot {
void installHooks(HookManager& hooks, bool packetSendHooked);
void onPacketSend(void* packet, void* sender);
void addTkCompletion(chatcommand::TkCommandSpec& spec, std::uint32_t firstEnumIndex, std::optional<std::uint32_t> booleanIndex);
}
