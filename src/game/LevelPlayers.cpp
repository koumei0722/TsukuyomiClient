#include "game/LevelPlayers.h"
#include "game/BlockWrite.h"
#include "game/PlayerListMemory.h"
#include "memory/Memory.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace tsukuyomi::levelplayers {
namespace {
constexpr std::size_t kBlockSourceLevel = 0x20, kPlayersSlot = 0x9F8;

bool readPointer(const void* base, std::size_t offset, const void*& out)
{
    return memory::plausiblePointer(base)
        && memory::copyGuarded(static_cast<const char*>(base) + offset, &out, sizeof(out))
        && memory::plausiblePointer(out);
}
}

const void* map()
{
    const void* level = nullptr;
    const void* vtable = nullptr;
    const void* getter = nullptr;
    unsigned char code[8]{};
    if (!readPointer(blockwrite::renderRegion(), kBlockSourceLevel, level) || !readPointer(level, 0, vtable)
        || !readPointer(vtable, kPlayersSlot, getter) || !memory::inGameModule(getter)
        || !memory::copyGuarded(getter, code, sizeof(code)) || code[0] != 0x48 || code[1] != 0x8B || code[2] != 0x81
        || code[7] != 0xC3) return nullptr;
    std::int32_t disp = 0;
    std::memcpy(&disp, code + 3, sizeof(disp));
    const void* players = nullptr;
    if (disp <= 0 || disp >= 0x10000 || !readPointer(level, static_cast<std::size_t>(disp), players)) return nullptr;
    return players;
}

bool uuidOf(std::int64_t uniqueId, std::uint8_t out[16])
{
    return uuidOf(map(), uniqueId, out);
}
bool uuidOf(const void* players, std::int64_t uniqueId, std::uint8_t out[16])
{
    if (!players) return false;
    std::vector<playerlist::mem::RawPlayer> raw(playerlist::mem::kMaxPlayerEntries);
    const int count = playerlist::mem::readPlayerMap(players, raw.data(), static_cast<int>(raw.size()));
    for (int i = 0; i < count; ++i) {
        if (raw[i].actorId != uniqueId) continue;
        if (std::all_of(std::begin(raw[i].uuid), std::end(raw[i].uuid), [](std::uint8_t b) { return b == 0; })) return false;
        std::memcpy(out, raw[i].uuid, 16);
        return true;
    }
    return false;
}

}
