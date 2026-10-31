#include "LocalPlayerIds.h"
#include "game/GameData.h"
#include "memory/Memory.h"
#include <Windows.h>

namespace tsukuyomi::bot {
namespace {
constexpr std::ptrdiff_t kActorRegistry = 0x10, kActorEntityId = 0x18;
constexpr std::ptrdiff_t kPoolsBegin = 0x68, kPoolsEnd = 0x70, kPoolBytes = 0x20, kPoolType = 0x08, kPoolStore = 0x10;
constexpr std::ptrdiff_t kSparseBegin = 0x08, kSparseEnd = 0x10, kPayload = 0x50;
constexpr std::uint32_t kIndexMask = 0x3ffffu, kVersionMask = 0xfffc0000u;
constexpr std::uint32_t kUniqueIdType = 0x18F957AFu, kRuntimeIdType = 0xFC0DBBB5u;
int filter(unsigned code) { return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH; }
bool readComponent(std::uintptr_t registry, std::uint32_t typeId, std::uint32_t id, std::uint64_t& value)
{
    __try {
        const auto first = *reinterpret_cast<const std::uintptr_t*>(registry + kPoolsBegin);
        const auto last = *reinterpret_cast<const std::uintptr_t*>(registry + kPoolsEnd);
        if (!first || last <= first || (last - first) % kPoolBytes != 0) return false;
        std::uintptr_t store = 0;
        for (std::uintptr_t entry = first; entry < last && entry < first + 4096 * kPoolBytes; entry += kPoolBytes)
            if (*reinterpret_cast<const std::uint32_t*>(entry + kPoolType) == typeId) { store = *reinterpret_cast<const std::uintptr_t*>(entry + kPoolStore); break; }
        if (!store) return false;
        const std::uint32_t low = id & kIndexMask, page = low >> 11;
        const auto pagesBegin = *reinterpret_cast<const std::uintptr_t*>(store + kSparseBegin);
        const auto pagesEnd = *reinterpret_cast<const std::uintptr_t*>(store + kSparseEnd);
        if (!pagesBegin || pagesEnd <= pagesBegin || page >= (pagesEnd - pagesBegin) / 8) return false;
        const auto sparse = *reinterpret_cast<const std::uintptr_t*>(pagesBegin + page * 8);
        if (!sparse) return false;
        const std::uint32_t slot = *reinterpret_cast<const std::uint32_t*>(sparse + (low & 0x7ffu) * 4);
        if (((id & kVersionMask) ^ slot) > 0x3fffeu) return false;
        const std::uint32_t position = slot & kIndexMask;
        const auto table = *reinterpret_cast<const std::uintptr_t*>(store + kPayload);
        if (!table) return false;
        const auto dense = *reinterpret_cast<const std::uintptr_t*>(table + (position >> 7) * 8);
        if (!dense) return false;
        value = *reinterpret_cast<const std::uint64_t*>(dense + (position & 0x7fu) * 8);
        return true;
    } __except (filter(GetExceptionCode())) { return false; }
}
bool readActor(const void* player, std::uintptr_t& registry, std::uint32_t& id)
{
    __try {
        const auto* p = static_cast<const std::uint8_t*>(player);
        registry = *reinterpret_cast<const std::uintptr_t*>(p + kActorRegistry);
        id = *reinterpret_cast<const std::uint32_t*>(p + kActorEntityId);
        return memory::plausiblePointer(reinterpret_cast<const void*>(registry));
    } __except (filter(GetExceptionCode())) { return false; }
}
}
std::optional<std::uint64_t> readActorRuntimeId(const void* actor)
{
    std::uintptr_t registry = 0;
    std::uint32_t id = 0;
    std::uint64_t runtime = 0;
    if (!actor || !readActor(actor, registry, id) || !readComponent(registry, kRuntimeIdType, id, runtime) || runtime == 0) return std::nullopt;
    return runtime;
}
std::optional<LocalPlayerIds> readLocalPlayerIds()
{
    void* const player = GameData::instance().player();
    std::uintptr_t registry = 0;
    std::uint32_t id = 0;
    if (!player || !readActor(player, registry, id)) return std::nullopt;
    std::uint64_t runtime = 0, unique = 0;
    if (!readComponent(registry, kRuntimeIdType, id, runtime) || !readComponent(registry, kUniqueIdType, id, unique) || runtime == 0)
        return std::nullopt;
    return LocalPlayerIds{runtime, static_cast<std::int64_t>(unique)};
}
}
