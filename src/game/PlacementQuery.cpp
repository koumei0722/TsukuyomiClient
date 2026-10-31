#include "game/PlacementQuery.h"
#include "game/BlockRegistry.h"
#include "core/Notice.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include <Windows.h>
#include <atomic>
#include <cstring>

namespace tsukuyomi::placementquery {
namespace {
std::atomic<bool> g_ready{false}, g_failed{false};
constexpr std::size_t kPickSlot = 0x270, kPlacementSlot = 0x280;
using PlacementFn = const void* (__fastcall*)(const void*, const void*, const void*,
                                            std::uint8_t, const void*, int);
void stop()
{
    g_failed.store(true); g_ready.store(false);
    notice::failOnce("EasyPlace.placementQuery", L"Easy Place: stopped because the placement query slot could not be verified or the call failed.",
                     "Easy Place: placement query is unavailable.");
}
bool slotsRaw(const void* block, const void*& pick, const void*& placement)
{
    if (!memory::isReadable(block, 0x70)) return false;
    __try {
        const auto* type = *reinterpret_cast<const std::byte* const*>(static_cast<const std::byte*>(block) + 0x68);
        if (!memory::isReadable(type, sizeof(void*))) return false;
        const auto* vt = *reinterpret_cast<const std::byte* const*>(type);
        if (!memory::isReadable(vt, kPlacementSlot + sizeof(void*))) return false;
        pick = *reinterpret_cast<const void* const*>(vt + kPickSlot);
        placement = *reinterpret_cast<const void* const*>(vt + kPlacementSlot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool executableOnce(const void* fn)
{
    static std::atomic<const void*> known[16]{};
    for (const auto& one : known) if (one.load(std::memory_order_relaxed) == fn) return true;
    if (!memory::inGameModule(fn) || !memory::isExecutable(fn, 1)) return false;
    static std::atomic<unsigned> next{0};
    known[next.fetch_add(1, std::memory_order_relaxed) % 16].store(fn, std::memory_order_relaxed);
    return true;
}
bool readPlacementFn(const void* type, PlacementFn& fn)
{
    __try {
        const auto* vt = *reinterpret_cast<const std::byte* const*>(type);
        fn = *reinterpret_cast<PlacementFn const*>(vt + kPlacementSlot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool callPlacementFn(PlacementFn fn, const HeldBlock& held, const void* actor, const std::int32_t* pos,
                     std::uint8_t face, const float* click, const void*& out)
{
    __try {
        out = fn(held.type, actor, pos, face, click, held.itemValue);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool queryRaw(const HeldBlock& held, const void* actor, const std::int32_t* pos,
              std::uint8_t face, const float* click, const void*& out)
{
    PlacementFn fn = nullptr;
    if (!memory::plausiblePointer(held.type) || !readPlacementFn(held.type, fn)
        || !executableOnce(reinterpret_cast<const void*>(fn))) return false;
    return callPlacementFn(fn, held, actor, pos, face, click, out);
}
}
bool initialize()
{
    if (g_failed.load()) return false;
    if (g_ready.load()) return true;
    const auto& scanner = Scanner::instance();
    const auto* base = scanner.address(Target::BlockGetPlacementBlock);
    const auto* wrapper = scanner.address(Target::BlockAsItemInstance);
    unsigned char instruction[7]{};
    std::int32_t pickSlot = 0;
    if (wrapper && memory::isReadable(wrapper + 0x84, sizeof(instruction)))
        std::memcpy(instruction, wrapper + 0x84, sizeof(instruction));
    std::memcpy(&pickSlot, instruction + 3, sizeof(pickSlot));
    if (!base || !memory::isExecutable(base, 1) || instruction[0] != 0x48
        || instruction[1] != 0x8B || instruction[2] != 0x80 || pickSlot != kPickSlot) { stop(); return false; }
    blocks::Table table;
    if (!blocks::resolve({}, table)) { stop(); return false; }
    for (const auto& [name, block] : table) {
        (void)name;
        const void* pick = nullptr; const void* placement = nullptr;
        if (slotsRaw(block, pick, placement) && placement == base && memory::inGameModule(pick)
            && memory::isExecutable(pick, 1)) { g_ready.store(true); return true; }
    }
    stop(); return false;
}
bool readHeld(const void* stack, HeldBlock& out)
{
    out = {};
    if (!stack || !memory::isReadable(stack, 0x24)) return false;
    __try {
        const auto* bytes = static_cast<const std::byte*>(stack);
        const auto* block = *reinterpret_cast<const std::byte* const*>(bytes + 0x18);
        if (!block) { out.itemValue = *reinterpret_cast<const std::uint16_t*>(bytes + 0x20); return true; }
        if (!memory::isReadable(block, 0x122)) return false;
        out.type = *reinterpret_cast<const void* const*>(block + 0x68);
        out.itemValue = *reinterpret_cast<const std::uint16_t*>(block + 0x120);
        return out.type != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool query(const HeldBlock& held, const void* actor, const std::int32_t pos[3],
           std::uint8_t face, const float click[3], const void*& out)
{
    out = nullptr;
    if (!g_ready.load() || g_failed.load() || !held.type || !actor || !pos || !click || face > 5) return false;
    if (!queryRaw(held, actor, pos, face, click, out)) { stop(); return false; }
    return true;
}
}
