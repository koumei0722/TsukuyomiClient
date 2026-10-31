#include "GamePath.h"
#include "BotNet.h"
#include "core/Logger.h"
#include "core/Notice.h"
#include "game/BlockRegistry.h"
#include "game/BlockWrite.h"
#include "game/GameData.h"
#include "memory/Memory.h"
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <intrin.h>

namespace tsukuyomi::bot::gamepath {
namespace {
using CtorFn = void*(__fastcall*)(void* finder, void* region, const std::uint8_t* capabilities);
using ExtractFn = void*(__fastcall*)(void* out, void* actor, std::uint8_t purpose, const void* target);
using FindFn = void*(__fastcall*)(void* finder, void** path, const void* data, float x, float y, float z, float radius);
using DestroyFn = void(__fastcall*)(void* self);
using DeleteFn = void(__fastcall*)(void* pointer, std::size_t size);
using RegionCtorFn = void*(__fastcall*)(void* self, void* level, void* dimension, void* source, bool publicSource, bool allowUnpopulated, bool flag7, bool flag8);
using RegionDestroyFn = void*(__fastcall*)(void* self, std::uint32_t flags);
using DimensionIdFn = void*(__fastcall*)(void* actor, std::int32_t* out);
using GetBlockFn = const void*(__fastcall*)(void* region, const std::int32_t* position);
CtorFn g_ctor = nullptr;
ExtractFn g_extract = nullptr;
FindFn g_find = nullptr;
DestroyFn g_destroyData = nullptr, g_destroyFinder = nullptr;
DeleteFn g_delete = nullptr;
RegionCtorFn g_regionCtor = nullptr;
const void* g_regionVtable = nullptr;
std::uint32_t g_chunkSourceOffset = 0;
DimensionIdFn g_dimensionId = nullptr;
std::atomic<bool> g_ready{false};
std::atomic<int> g_logs{0};

constexpr std::size_t kFinderSize = 0x200, kDataSize = 0x200, kPathSize = 0x28, kNodeSize = 16;
constexpr std::size_t kRegionSize = 0x300;
constexpr std::size_t kRegionLevel = 0x20, kRegionDimension = 0x30;
constexpr std::uint8_t kCapabilities[2] = {0x0b, 0x09};
constexpr std::uint8_t kCanFloat = 0x20;
constexpr std::size_t kDataInWater = 0x31;
constexpr std::size_t kGetBlockSlot = 2;
constexpr float kWidth = 0.6f, kHeight = 1.8f;
constexpr std::size_t kMaxNodes = 4096;

std::byte* callTarget(std::byte* site, std::size_t at)
{
    std::uint8_t opcode = 0;
    std::int32_t rel = 0;
    if (!memory::copyGuarded(site + at - 1, &opcode, 1) || opcode != 0xe8 || !memory::copyGuarded(site + at, &rel, 4)) return nullptr;
    return site + at + 4 + rel;
}
std::byte* functionStart(std::byte* site)
{
    if (!site) return nullptr;
    DWORD64 imageBase = 0;
    const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(site), &imageBase, nullptr);
    return entry ? reinterpret_cast<std::byte*>(imageBase + entry->BeginAddress) : nullptr;
}
int memoryException(unsigned code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
struct World { void* level; void* dimension; void* source; };
bool worldGuarded(void* region, World* out)
{
    __try {
        auto* const r = static_cast<std::byte*>(region);
        if (*reinterpret_cast<const void**>(r) != g_regionVtable) return false;
        out->level = *reinterpret_cast<void**>(r + kRegionLevel);
        out->dimension = *reinterpret_cast<void**>(r + kRegionDimension);
        if (!out->level || !out->dimension) return false;
        out->source = *reinterpret_cast<void**>(static_cast<std::byte*>(out->dimension) + g_chunkSourceOffset);
        return out->source && memory::inGameModule(*static_cast<void**>(out->source));
    } __except (memoryException(GetExceptionCode())) { return false; }
}
std::int32_t dimensionGuarded(void* actor)
{
    __try {
        std::int32_t dimension = -1;
        g_dimensionId(actor, &dimension);
        return dimension;
    } __except (memoryException(GetExceptionCode())) { return -1; }
}
struct Raw {
    PathNode* nodes;
    std::size_t capacity, count;
    std::uint8_t completion;
    int stage;
};
void freeBuffer(std::byte* begin, std::size_t bytes)
{
    if (!begin) return;
    if (bytes >= 0x1000) {
        void* const raw = *reinterpret_cast<void**>(begin - 8);
        g_delete(raw, bytes + 0x27);
    } else g_delete(begin, bytes);
}
void findGuarded(const World* world, void* actor, const float* feet, const float* target, float radius, bool inLiquid, Raw* out)
{
    alignas(16) std::byte region[kRegionSize] = {};
    alignas(16) std::byte finder[kFinderSize] = {};
    alignas(16) std::byte data[kDataSize] = {};
    struct alignas(4) OptionalVec3 { float x, y, z; bool has; } wanted{target[0], target[1], target[2], true};
    void* path = nullptr;
    out->stage = 1;
    __try {
        g_regionCtor(region, world->level, world->dimension, world->source, false, true, false, false);
        out->stage = 2;
        const std::uint8_t capabilities[2] = {static_cast<std::uint8_t>(kCapabilities[0] | (inLiquid ? kCanFloat : 0)), kCapabilities[1]};
        g_ctor(finder, region, capabilities);
        out->stage = 3;
        g_extract(data, actor, 0, &wanted);
        data[kDataInWater] = std::byte{inLiquid ? std::uint8_t{1} : std::uint8_t{0}};
        auto* const f = reinterpret_cast<float*>(data);
        f[0] = feet[0] - kWidth / 2; f[1] = feet[1]; f[2] = feet[2] - kWidth / 2;
        f[3] = feet[0] + kWidth / 2; f[4] = feet[1] + kHeight; f[5] = feet[2] + kWidth / 2;
        f[6] = kWidth; f[7] = kHeight;
        f[8] = feet[0]; f[9] = feet[1]; f[10] = feet[2];
        f[11] = std::ceil(kWidth);
        data[0x118] = std::byte{0}; data[0x119] = std::byte{0}; data[0x11c] = std::byte{1};
        out->stage = 4;
        g_find(finder, &path, data, target[0], target[1], target[2], radius);
        out->stage = 5;
        if (path) {
            auto* const p = static_cast<std::byte*>(path);
            auto* const begin = *reinterpret_cast<std::byte**>(p);
            auto* const end = *reinterpret_cast<std::byte**>(p + 8);
            auto* const capacity = *reinterpret_cast<std::byte**>(p + 0x10);
            out->completion = *reinterpret_cast<std::uint8_t*>(p + 0x20);
            const std::size_t count = begin && end > begin ? static_cast<std::size_t>(end - begin) / kNodeSize : 0;
            out->count = 0;
            for (std::size_t i = 0; i < count && out->count < out->capacity; ++i) {
                const auto* node = reinterpret_cast<const std::int32_t*>(begin + i * kNodeSize);
                out->nodes[out->count++] = PathNode{node[0], node[1], node[2]};
            }
            out->stage = 6;
            freeBuffer(begin, static_cast<std::size_t>(capacity - begin));
            g_delete(path, kPathSize);
        }
        out->stage = 6;
        g_destroyData(data);
        g_destroyFinder(finder);
        reinterpret_cast<RegionDestroyFn>((*reinterpret_cast<void***>(region))[0])(region, 0);
        out->stage = 0;
    } __except (memoryException(GetExceptionCode())) {}
}
}

bool initialize()
{
    auto* const site = net::site(Site::PathFinderSite);
    auto* const regionSite = net::site(Site::BlockSourceCtorSite);
    auto* const dimensionSite = net::site(Site::DimensionChunkSourceSite);
    if (!site || !regionSite || !dimensionSite) {
        log().warn(L"Bot: the game's pathfinder signatures are unavailable; walking along a path is unavailable");
        return false;
    }
    g_ctor = reinterpret_cast<CtorFn>(callTarget(site, 0xb));
    g_extract = reinterpret_cast<ExtractFn>(callTarget(site, 0x86));
    g_find = reinterpret_cast<FindFn>(callTarget(site, 0xb7));
    g_destroyData = reinterpret_cast<DestroyFn>(callTarget(site, 0xc4));
    auto* const destroyFinder = callTarget(site, 0xcd);
    g_destroyFinder = reinterpret_cast<DestroyFn>(destroyFinder);
    static constexpr std::uint8_t kBeforeDelete[] = {0x48, 0x8d, 0x7e, 0x68};
    std::uint8_t before[sizeof kBeforeDelete] = {};
    if (destroyFinder && memory::copyGuarded(destroyFinder + 0x3e, before, sizeof before) && std::memcmp(before, kBeforeDelete, sizeof before) == 0)
        g_delete = reinterpret_cast<DeleteFn>(callTarget(destroyFinder, 0x43));
    g_regionCtor = reinterpret_cast<RegionCtorFn>(functionStart(regionSite));
    std::int32_t rel = 0;
    if (memory::copyGuarded(regionSite + 0xa, &rel, 4)) g_regionVtable = regionSite + 0xe + rel;
    if (!memory::copyGuarded(dimensionSite + 11, &g_chunkSourceOffset, 4) || g_chunkSourceOffset == 0 || g_chunkSourceOffset > 0x1000) g_chunkSourceOffset = 0;
    if (auto* const askSite = net::site(Site::SubChunkAskSite)) g_dimensionId = reinterpret_cast<DimensionIdFn>(callTarget(askSite, 0x34));
    if (!g_dimensionId) log().warn(L"Bot: the game's getDimensionId is unavailable; far bots and bots in other dimensions cannot walk along a path");
    const bool ok = g_ctor && g_extract && g_find && g_destroyData && g_destroyFinder && g_delete && g_regionCtor && g_regionVtable && g_chunkSourceOffset;
    if (!ok) {
        log().warn(L"Bot: the game's pathfinder functions are unavailable; walking along a path is unavailable");
        return false;
    }
    g_ready.store(true, std::memory_order_release);
    const auto* const base = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));
    log().info(L"Bot: the game's pathfinder is ready (find RVA {:#x}, BlockSource RVA {:#x}, chunk source +{:#x})",
        static_cast<std::uintptr_t>(reinterpret_cast<const std::byte*>(g_find) - base),
        static_cast<std::uintptr_t>(reinterpret_cast<const std::byte*>(g_regionCtor) - base), g_chunkSourceOffset);
    return true;
}
bool available() { return g_ready.load(std::memory_order_acquire); }

std::optional<std::int32_t> playerDimension()
{
    if (!g_dimensionId) return std::nullopt;
    void* const actor = GameData::instance().player();
    const void* const playerVtable = GameData::instance().playerVtable();
    void* vtable = nullptr;
    if (!actor || !memory::copyGuarded(actor, &vtable, sizeof vtable) || !memory::inGameModule(vtable) || (playerVtable && vtable != playerVtable))
        return std::nullopt;
    const auto dimension = dimensionGuarded(actor);
    if (dimension < 0 || dimension > 2) return std::nullopt;
    return dimension;
}

namespace {
std::optional<Found> findWorld(const World* given, protocol::Vec3 feet, PathNode target, float radius, bool inLiquid, std::string& error);
std::optional<bool> liquidWorld(const World* given, protocol::Vec3 feet);
}
std::optional<Found> find(protocol::Vec3 feet, PathNode target, float radius, bool inLiquid, std::string& error)
{
    return findWorld(nullptr, feet, target, radius, inLiquid, error);
}
std::optional<Found> findIn(void* level, void* dimension, void* source, protocol::Vec3 feet, PathNode target, float radius, bool inLiquid,
                            std::string& error)
{
    if (!level || !dimension || !source) { error = "the bot's terrain is not loaded"; return std::nullopt; }
    const World world{level, dimension, source};
    return findWorld(&world, feet, target, radius, inLiquid, error);
}
std::optional<bool> inLiquid(protocol::Vec3 feet) { return liquidWorld(nullptr, feet); }
std::optional<bool> inLiquidIn(void* level, void* dimension, void* source, protocol::Vec3 feet)
{
    if (!level || !dimension || !source) return std::nullopt;
    const World world{level, dimension, source};
    return liquidWorld(&world, feet);
}
namespace {
int liquidGuarded(const World* world, const std::int32_t (*positions)[3], int count)
{
    alignas(16) std::byte region[kRegionSize] = {};
    __try {
        g_regionCtor(region, world->level, world->dimension, world->source, false, true, false, false);
        auto** const vtable = *reinterpret_cast<void***>(region);
        int result = 0;
        for (int i = 0; i < count && result == 0; ++i) {
            const void* const block = reinterpret_cast<GetBlockFn>(vtable[kGetBlockSlot])(region, positions[i]);
            bool motion = false, liquid = false;
            if (!block || !blocks::materialFlags(block, motion, liquid)) result = -1;
            else if (liquid) result = 1;
        }
        reinterpret_cast<RegionDestroyFn>(vtable[0])(region, 0);
        return result;
    } __except (memoryException(GetExceptionCode())) { return -2; }
}
std::optional<bool> liquidWorld(const World* given, protocol::Vec3 feet)
{
    const auto inWorld = [](float v) { return std::isfinite(v) && std::fabs(v) < 3.0e7f; };
    if (!available() || !inWorld(feet.x) || !inWorld(feet.y) || !inWorld(feet.z)) return std::nullopt;
    World world{};
    if (given) world = *given;
    else {
        void* const region = blockwrite::renderRegion();
        if (!region || !blockwrite::regionIsAlive(region) || !worldGuarded(region, &world)) return std::nullopt;
    }
    std::int32_t positions[8][3] = {};
    int count = 0;
    for (const float dx : {-kWidth / 2, kWidth / 2})
        for (const float dz : {-kWidth / 2, kWidth / 2})
            for (const float dy : {0.1f, 1.0f}) {
                auto& p = positions[count++];
                p[0] = static_cast<std::int32_t>(std::floor(feet.x + dx));
                p[1] = static_cast<std::int32_t>(std::floor(feet.y + dy));
                p[2] = static_cast<std::int32_t>(std::floor(feet.z + dz));
            }
    const int result = liquidGuarded(&world, positions, count);
    if (result == -2) {
        g_ready.store(false, std::memory_order_release);
        notice::failOnce("BotPathLiquid", L"Bot: reading the block around a bot faulted; walking along a path is disabled",
            "Bot: reading the block around a bot faulted; walking along a path is disabled");
    }
    if (result < 0) return std::nullopt;
    return result == 1;
}
std::optional<Found> findWorld(const World* given, protocol::Vec3 feet, PathNode target, float radius, bool inLiquid, std::string& error)
{
    if (!available()) { error = "the game's pathfinder is unavailable"; return std::nullopt; }
    World world{};
    if (given) world = *given;
    else {
        void* const region = blockwrite::renderRegion();
        if (!region || !blockwrite::regionIsAlive(region) || !worldGuarded(region, &world)) { error = "the world is not loaded"; return std::nullopt; }
    }
    void* const actor = GameData::instance().player();
    const void* const playerVtable = GameData::instance().playerVtable();
    void* vtable = nullptr;
    if (!actor || !memory::copyGuarded(actor, &vtable, sizeof vtable) || !memory::inGameModule(vtable) || (playerVtable && vtable != playerVtable)) {
        error = "the main player is not available"; return std::nullopt;
    }
    Found found;
    found.nodes.resize(kMaxNodes);
    Raw raw{found.nodes.data(), found.nodes.size(), 0, 0, 0};
    const float from[3] = {feet.x, feet.y, feet.z};
    const float to[3] = {float(target.x) + 0.5f, float(target.y) + 0.05f, float(target.z) + 0.5f};
    LARGE_INTEGER start{}, stop{}, frequency{};
    QueryPerformanceCounter(&start);
    findGuarded(&world, actor, from, to, radius, inLiquid, &raw);
    QueryPerformanceCounter(&stop); QueryPerformanceFrequency(&frequency);
    found.ms = double(stop.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart);
    if (raw.stage) {
        g_ready.store(false, std::memory_order_release);
        notice::failOnce("BotPathFinder", std::format(L"Bot: the game's pathfinder faulted (stage {}); walking along a path is disabled", raw.stage),
            "Bot: the game's pathfinder faulted; walking along a path is disabled");
        error = "the game's pathfinder faulted";
        return std::nullopt;
    }
    found.nodes.resize(raw.count);
    found.complete = raw.completion == 2;
    if (g_logs.fetch_add(1, std::memory_order_relaxed) < 48)
        log().info(L"Bot: path from ({:.2f}, {:.2f}, {:.2f}){} to ({}, {}, {}) radius {:.0f}: {} nodes, completion {}, {:.2f} ms",
            feet.x, feet.y, feet.z, inLiquid ? L" in liquid" : L"", target.x, target.y, target.z, radius, raw.count, raw.completion, found.ms);
    return found;
}

}

}
