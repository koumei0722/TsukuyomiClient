#include "game/ChunkResend.h"

#include "core/Logger.h"
#include "game/BlockWrite.h"
#include "game/GameData.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>
#include <algorithm>

namespace tsukuyomi::chunkresend {

namespace {

using DimensionIdFn = void*(__fastcall*)(void* actor, std::int32_t* out);
using AskedFn = bool(__fastcall*)(void* list, const std::int32_t* dimension, const std::int32_t* position);
using AskedByFn = bool(__fastcall*)(void* list, const std::int32_t* dimension, const std::int32_t* position, void* owner);
using EnqueueFn = void(__fastcall*)(void* list, const std::int32_t* dimension, const std::int32_t* position, void* owner);

struct Layout {
    std::int32_t tableBegin = 0;
    std::int32_t tableEnd = 0;
    std::int32_t stride = 0;
    std::int32_t status = 0;
    std::int32_t index = 0;
    std::int32_t owner = 0;
    DimensionIdFn dimensionId = nullptr;
    AskedFn asked = nullptr;
    AskedByFn askedBy = nullptr;
    EnqueueFn enqueue = nullptr;
};

Layout g_layout;
std::atomic<bool> g_ready{false};

constexpr std::int32_t kAsking = 2;
constexpr std::int32_t kNotAsked = 0;
constexpr std::int32_t kLoaded = 7;
constexpr unsigned long long kAnswerWaitMs = 10000;
constexpr std::size_t kRevertBatch = 2048;
constexpr std::size_t kMaxSubChunks = 64;
constexpr int kMaxRadius = 128;
constexpr int kBatchSubChunks = 256;
constexpr unsigned long long kBatchIntervalMs = 50;
constexpr int kBatchCells = 1024;

struct Result {
    int chunks = 0;
    int marked = 0;
    int queued = 0;
    int faulted = 0;
    std::uint32_t statuses[8]{};
};

struct Sweep {
    bool active = false;
    void* player = nullptr;
    std::uint64_t generation = 0;
    std::int32_t dimension = -1;
    int centerX = 0, centerZ = 0;
    int radius = 0, step = 0, foundInRing = 0, emptyRings = 0;
    unsigned long long startedAt = 0, nextAt = 0;
    Result result;
};
Sweep g_sweep;

struct Marked {
    std::int32_t chunkX = 0, chunkZ = 0;
    std::int8_t index = 0;
    std::int8_t original = 0;
};
std::vector<Marked> g_marked;
std::size_t g_revertNext = 0;
unsigned long long g_revertAt = 0;
std::uint64_t g_markedGeneration = 0;
int g_reverted = 0;

__declspec(noinline) bool dimensionOf(DimensionIdFn fn, void* player, std::int32_t& out)
{
    __try {
        fn(player, &out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool ownerOf(void* player, std::int32_t field, void*& owner, void*& list)
{
    __try {
        owner = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(player) + field);
        list = owner != nullptr ? *reinterpret_cast<void**>(owner) : nullptr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool markChunk(const Layout& layout, std::uint8_t* chunk, void* owner, void* list,
                                    std::int32_t dimension, std::int32_t chunkX, std::int32_t chunkZ, Result& out,
                                    Marked* marked, int& markedCount)
{
    markedCount = 0;
    __try {
        auto* const begin = *reinterpret_cast<std::uint8_t**>(chunk + layout.tableBegin);
        auto* const end = *reinterpret_cast<std::uint8_t**>(chunk + layout.tableEnd);
        if (begin == nullptr || end < begin) return false;
        std::size_t count = static_cast<std::size_t>(end - begin) / static_cast<std::size_t>(layout.stride);
        if (count > kMaxSubChunks) count = kMaxSubChunks;
        for (std::size_t i = 0; i < count; ++i) {
            std::uint8_t* const sub = begin + i * static_cast<std::size_t>(layout.stride);
            auto* const status = reinterpret_cast<volatile std::int32_t*>(sub + layout.status);
            const std::int32_t was = *status;
            ++out.statuses[was >= 0 && was < 7 ? was : 7];
            if (was != kLoaded && was != kNotAsked) continue;
            *status = kAsking;
            ++out.marked;
            marked[markedCount++] = {chunkX, chunkZ, static_cast<std::int8_t>(sub[layout.index]), static_cast<std::int8_t>(was)};
            const std::int32_t position[3] = {chunkX, static_cast<std::int8_t>(sub[layout.index]), chunkZ};
            const bool first = layout.asked(list, &dimension, position);
            const bool second = layout.askedBy(list, &dimension, position, owner);
            if (first && second) continue;
            layout.enqueue(list, &dimension, position, owner);
            ++out.queued;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool askChunk(const Layout& layout, std::uint8_t* chunk, void* owner, void* list,
                                   std::int32_t dimension, std::int32_t chunkX, std::int32_t chunkZ, int room,
                                   int& pending, int& queued)
{
    pending = queued = 0;
    __try {
        auto* const begin = *reinterpret_cast<std::uint8_t**>(chunk + layout.tableBegin);
        auto* const end = *reinterpret_cast<std::uint8_t**>(chunk + layout.tableEnd);
        if (begin == nullptr || end < begin) return false;
        std::size_t count = static_cast<std::size_t>(end - begin) / static_cast<std::size_t>(layout.stride);
        if (count > kMaxSubChunks) count = kMaxSubChunks;
        for (std::size_t i = 0; i < count && queued < room; ++i) {
            std::uint8_t* const sub = begin + i * static_cast<std::size_t>(layout.stride);
            if (*reinterpret_cast<volatile std::int32_t*>(sub + layout.status) != kAsking) continue;
            ++pending;
            const std::int32_t position[3] = {chunkX, static_cast<std::int8_t>(sub[layout.index]), chunkZ};
            const bool first = layout.asked(list, &dimension, position);
            const bool second = layout.askedBy(list, &dimension, position, owner);
            if (first && second) continue;
            layout.enqueue(list, &dimension, position, owner);
            ++queued;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool revertSubChunk(const Layout& layout, std::uint8_t* chunk, std::int8_t index, std::int32_t original)
{
    __try {
        auto* const begin = *reinterpret_cast<std::uint8_t**>(chunk + layout.tableBegin);
        auto* const end = *reinterpret_cast<std::uint8_t**>(chunk + layout.tableEnd);
        if (begin == nullptr || end < begin) return false;
        std::size_t count = static_cast<std::size_t>(end - begin) / static_cast<std::size_t>(layout.stride);
        if (count > kMaxSubChunks) count = kMaxSubChunks;
        for (std::size_t i = 0; i < count; ++i) {
            std::uint8_t* const sub = begin + i * static_cast<std::size_t>(layout.stride);
            if (static_cast<std::int8_t>(sub[layout.index]) != index) continue;
            auto* const status = reinterpret_cast<volatile std::int32_t*>(sub + layout.status);
            if (*status != kAsking) return false;
            *status = original;
            return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void dropMarked()
{
    g_marked.clear();
    g_revertNext = 0;
    g_revertAt = 0;
    g_reverted = 0;
}

void revertUnanswered(unsigned long long now)
{
    if (g_marked.empty() || g_revertAt == 0 || now < g_revertAt) return;
    if (blockwrite::regionGeneration() != g_markedGeneration) {
        dropMarked();
        return;
    }
    void* const region = blockwrite::renderRegion();
    if (region == nullptr || !blockwrite::regionIsAlive(region)) return;
    std::uint8_t* chunk = nullptr;
    std::int32_t chunkX = 0, chunkZ = 0;
    bool haveChunk = false;
    const std::size_t stop = (std::min)(g_marked.size(), g_revertNext + kRevertBatch);
    for (; g_revertNext < stop; ++g_revertNext) {
        const Marked& m = g_marked[g_revertNext];
        if (!haveChunk || m.chunkX != chunkX || m.chunkZ != chunkZ) {
            chunk = static_cast<std::uint8_t*>(blockwrite::findChunk(region, m.chunkX * 16, m.chunkZ * 16));
            chunkX = m.chunkX;
            chunkZ = m.chunkZ;
            haveChunk = true;
        }
        if (chunk != nullptr && revertSubChunk(g_layout, chunk, m.index, m.original)) ++g_reverted;
    }
    if (g_revertNext < g_marked.size()) return;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
        log().info(L"F3+A: the server answered {} of {} sub chunks; {} unanswered ones were put back to their previous state",
                   g_marked.size() - g_reverted, g_marked.size(), g_reverted);
    }
    dropMarked();
}

void ringCell(int radius, int step, int& dx, int& dz)
{
    if (radius == 0) {
        dx = dz = 0;
        return;
    }
    const int side = step / (2 * radius);
    const int t = step % (2 * radius);
    switch (side) {
    case 0: dx = -radius + t; dz = -radius; break;
    case 1: dx = radius; dz = -radius + t; break;
    case 2: dx = radius - t; dz = radius; break;
    default: dx = -radius; dz = radius - t; break;
    }
}

void finish(const wchar_t* why)
{
    const Result& r = g_sweep.result;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
        log().info(L"F3+A: {} after {} ms: asked again for {} sub chunks ({} queued) in {} chunks out to {} chunks away, "
                   L"{} chunks unreadable; states before 0:{} 1:{} 2:{} 3:{} 4:{} 5:{} 6:{} 7+:{}",
                   why, GetTickCount64() - g_sweep.startedAt, r.marked, r.queued, r.chunks, g_sweep.radius, r.faulted,
                   r.statuses[0], r.statuses[1], r.statuses[2], r.statuses[3], r.statuses[4], r.statuses[5],
                   r.statuses[6], r.statuses[7]);
    }
    g_sweep.active = false;
    g_revertAt = GetTickCount64() + kAnswerWaitMs;
}

template <typename Fn>
Fn callTarget(const std::byte* site, std::size_t offset)
{
    void* const target = memory::ripTarget(site, offset);
    return memory::inGameModule(target) && memory::isExecutable(target, 1) ? reinterpret_cast<Fn>(target) : nullptr;
}

}

bool resolve()
{
    g_ready.store(false, std::memory_order_release);
    const std::byte* const site = Scanner::instance().address(Target::SubChunkRequestSite);
    std::uint8_t bytes[308]{};
    if (site == nullptr || !memory::copyGuarded(site, bytes, sizeof(bytes))) {
        log().warn(L"F3+A: SubChunkRequestSite was not found; chunks cannot be fetched from the server again");
        return false;
    }
    Layout layout;
    std::memcpy(&layout.tableBegin, bytes + 3, 4);
    std::memcpy(&layout.tableEnd, bytes + 10, 4);
    std::memcpy(&layout.owner, bytes + 221, 4);
    layout.stride = bytes[174];
    layout.status = bytes[193];
    layout.index = bytes[202];
    layout.dimensionId = callTarget<DimensionIdFn>(site, 241);
    layout.asked = callTarget<AskedFn>(site, 256);
    layout.askedBy = callTarget<AskedByFn>(site, 279);
    layout.enqueue = callTarget<EnqueueFn>(site, 304);
    const bool ok = layout.stride != 0 && bytes[183] == layout.stride && layout.tableEnd == layout.tableBegin + 8
                    && layout.tableBegin > 0 && layout.tableBegin < 0x1000 && layout.status + 4 <= layout.stride
                    && layout.index < layout.stride && layout.owner > 0 && layout.owner < 0x10000
                    && layout.dimensionId && layout.asked && layout.askedBy && layout.enqueue;
    if (!ok) {
        log().warn(L"F3+A: the sub chunk request fields could not be read (table {:#x}/{:#x}, size {:#x}, status {:#x}, "
                   L"index {:#x}, owner {:#x}); chunks cannot be fetched from the server again",
                   layout.tableBegin, layout.tableEnd, layout.stride, layout.status, layout.index, layout.owner);
        return false;
    }
    g_layout = layout;
    g_ready.store(true, std::memory_order_release);
    log().info(L"F3+A: sub chunks at chunk +{:#x} (size {:#x}, status +{:#x}, index +{:#x}), request queue at player +{:#x}",
               layout.tableBegin, layout.stride, layout.status, layout.index, layout.owner);
    return true;
}

bool ready()
{
    return g_ready.load(std::memory_order_acquire);
}

std::int32_t subChunkStatusOffset()
{
    return ready() ? g_layout.status : 0;
}

bool start()
{
    if (!ready()) return false;
    void* const player = GameData::instance().player();
    void* const region = blockwrite::renderRegion();
    if (player == nullptr || region == nullptr || !blockwrite::regionIsAlive(region)) return false;
    std::int32_t dimension = -1;
    if (!dimensionOf(g_layout.dimensionId, player, dimension) || dimension < 0) return false;
    if (g_sweep.active) finish(L"restarted");
    if (blockwrite::regionGeneration() != g_markedGeneration) dropMarked();
    g_markedGeneration = blockwrite::regionGeneration();
    g_revertAt = 0;
    const PlayerView view = GameData::instance().playerView();
    g_sweep = {};
    g_sweep.active = true;
    g_sweep.player = player;
    g_sweep.generation = blockwrite::regionGeneration();
    g_sweep.dimension = dimension;
    g_sweep.centerX = static_cast<int>(std::floor(view.x)) >> 4;
    g_sweep.centerZ = static_cast<int>(std::floor(view.z)) >> 4;
    g_sweep.startedAt = GetTickCount64();
    return true;
}

void pump()
{
    const unsigned long long now = GetTickCount64();
    if (!g_sweep.active) {
        revertUnanswered(now);
        return;
    }
    if (now < g_sweep.nextAt) return;
    g_sweep.nextAt = now + kBatchIntervalMs;
    if (GameData::instance().player() != g_sweep.player || blockwrite::regionGeneration() != g_sweep.generation) {
        finish(L"stopped (the world or the player changed)");
        return;
    }
    void* const region = blockwrite::renderRegion();
    void* owner = nullptr;
    void* list = nullptr;
    if (region == nullptr || !blockwrite::regionIsAlive(region)
        || !ownerOf(g_sweep.player, g_layout.owner, owner, list) || owner == nullptr || list == nullptr) {
        if (now - g_sweep.startedAt > 10000) finish(L"stopped (the client's chunks are not readable)");
        return;
    }
    Result& result = g_sweep.result;
    const int markedBefore = result.marked;
    for (int cells = 0; cells < kBatchCells && result.marked - markedBefore < kBatchSubChunks; ++cells) {
        const int ringSize = g_sweep.radius == 0 ? 1 : 8 * g_sweep.radius;
        if (g_sweep.step >= ringSize) {
            g_sweep.emptyRings = g_sweep.foundInRing == 0 ? g_sweep.emptyRings + 1 : 0;
            if (g_sweep.emptyRings >= 2 || g_sweep.radius >= kMaxRadius) {
                finish(L"done");
                return;
            }
            ++g_sweep.radius;
            g_sweep.step = 0;
            g_sweep.foundInRing = 0;
            continue;
        }
        int dx = 0;
        int dz = 0;
        ringCell(g_sweep.radius, g_sweep.step++, dx, dz);
        const int chunkX = g_sweep.centerX + dx;
        const int chunkZ = g_sweep.centerZ + dz;
        auto* const chunk = static_cast<std::uint8_t*>(blockwrite::findChunk(region, chunkX * 16, chunkZ * 16));
        if (chunk == nullptr) continue;
        ++g_sweep.foundInRing;
        ++result.chunks;
        Marked marked[kMaxSubChunks];
        int markedCount = 0;
        if (!markChunk(g_layout, chunk, owner, list, g_sweep.dimension, chunkX, chunkZ, result, marked, markedCount))
            ++result.faulted;
        g_marked.insert(g_marked.end(), marked, marked + markedCount);
    }
}

bool askPending(PendingChunk* chunks, std::size_t count, int budget)
{
    if (!ready()) return false;
    void* const player = GameData::instance().player();
    if (player == nullptr) return false;
    std::int32_t dimension = -1;
    void* owner = nullptr;
    void* list = nullptr;
    if (!dimensionOf(g_layout.dimensionId, player, dimension) || dimension < 0
        || !ownerOf(player, g_layout.owner, owner, list) || owner == nullptr || list == nullptr) {
        return false;
    }
    int queued = 0;
    for (std::size_t i = 0; i < count && queued < budget; ++i) {
        PendingChunk& c = chunks[i];
        c.faulted = c.chunk == nullptr
                    || !askChunk(g_layout, static_cast<std::uint8_t*>(c.chunk), owner, list, dimension, c.chunkX, c.chunkZ,
                                 budget - queued, c.pending, c.queued);
        queued += c.queued;
    }
    return true;
}

}
