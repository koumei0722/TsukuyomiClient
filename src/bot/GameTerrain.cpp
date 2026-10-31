#include "GameTerrain.h"
#include "BotNet.h"
#include "GamePacket.h"
#include "GamePath.h"
#include "core/Logger.h"
#include "core/Notice.h"
#include "game/GameData.h"
#include "memory/Memory.h"
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <intrin.h>
#include <mutex>
#include <vector>

namespace tsukuyomi::bot::gameterrain {
namespace {
using HandlerCtorFn = void*(__fastcall*)(void* self, void* deps);
using LevelDimensionFn = void*(__fastcall*)(void* level, void** out, std::int32_t dimension);
using TrackerInitFn = void(__fastcall*)(void* tracker, void* chunkSource, const void* key);
using TrackerMoveFn = void(__fastcall*)(void* tracker, const std::int32_t* center, std::uint32_t radius, const float* direction, float minimumMove);
using EntityKeyFn = void*(__fastcall*)(void* context);
using DestroyFn = void*(__fastcall*)(void* self, std::uint32_t flags);
using TickFn = void(__fastcall*)(void* self);
using DispatchFn = void(__fastcall*)(void* dispatcher, void* networkId, void* callback, void* shared);
using ExistingFn = void(__fastcall*)(void* source, void** out, const std::int32_t* position);
using RemoveFn = void(__fastcall*)(void* registry, std::uint32_t id);

HandlerCtorFn g_handlerCtor = nullptr;
const void* g_handlerVtable = nullptr;
TickFn g_tick = nullptr;
LevelDimensionFn g_levelDimension = nullptr;
TrackerInitFn g_trackerInit = nullptr;
TrackerMoveFn g_trackerMove = nullptr;
EntityKeyFn g_entityKey = nullptr;
std::uint32_t g_trackerOffset = 0;
std::uint32_t g_registryOffset = 0;
std::uint32_t g_viewDimension = 0;
std::uint32_t g_removeSlot = 0;
std::uint32_t g_providerOffset = 0;
std::byte* g_enqueue = nullptr;
std::byte* g_consume = nullptr;
std::byte* g_finish = nullptr;
std::atomic<bool> g_ready{false};

constexpr std::size_t kHandlerAlloc = 0x500, kDepsSize = 0x290;
constexpr std::size_t kHandlerClientInstance = 0x58, kTickSlot = 4;
constexpr std::size_t kGetLocalPlayerSlot = 0xf0 / 8;
constexpr std::size_t kPlayerSize = 0x1200, kPlayerDimension = 0x1f0, kPlayerDimensionControl = 0x1f8, kPlayerLevel = 0x200,
                      kPlayerPosition = 0x240, kPlayerRotation = 0x250, kPlayerConnection = 0x810;
constexpr std::size_t kTrackerId = 0xc, kTrackerRadius = 0x10, kTrackerView = 0x18;
constexpr std::uint32_t kTrackerMargin = 0x60;
constexpr std::size_t kDimensionSource = 0x1d8, kExistingSlot = 3, kChunkState = 0xf0, kSubChunks = 0x1c8, kSubChunkSize = 0x68,
                      kSubChunkStatus = 0x18, kSubChunkIndex = 0x61, kMaxSubChunks = 64;
constexpr std::size_t kPacketDispatcher = 0x20, kDispatchSlot = 1, kConnectionNetworkId = 0x18;
constexpr std::size_t kSubClientBoxes = 0x1e0;
constexpr std::size_t kJobKey = 8, kJobKey2 = 0x10, kJobDimension = 0x14, kEntrySize = 0x20, kFinishCapture = 8, kCaptureJob = 0x18;

struct Session {
    const void* owner = nullptr;
    std::int32_t dimension = -1;
    void* dimensionObject = nullptr;
    void* source = nullptr;
    bool tracker = false;
    void* realHandler = nullptr;
    void* realPlayer = nullptr;
    unsigned long long playerSerial = 0;
    void* level = nullptr;
    std::byte* player = nullptr;
    std::byte* clientInstance = nullptr;
    void** clientVtable = nullptr;
    std::byte* thunks = nullptr;
    std::byte* handler = nullptr;
    bool constructed = false;
    float position[4] = {};
    float rotation[4] = {};
};
Session g_s;
std::atomic<bool> g_active{false};
std::atomic<bool> g_divert{false};
std::atomic<int> g_logs{0};
struct SideJob { void* job; void* control; };
std::mutex g_sideMutex;
std::vector<SideJob> g_side;

void* __fastcall nothing(void* self, void*, void*, void*) { return self; }
void* g_nothingVtable[8] = {reinterpret_cast<void*>(&nothing), reinterpret_cast<void*>(&nothing), reinterpret_cast<void*>(&nothing), reinterpret_cast<void*>(&nothing),
                            reinterpret_cast<void*>(&nothing), reinterpret_cast<void*>(&nothing), reinterpret_cast<void*>(&nothing), reinterpret_cast<void*>(&nothing)};
void* g_nothingObject[2] = {g_nothingVtable, nullptr};
void* __fastcall proxyPlayer(void*) { return g_s.player; }
void* g_providerVtable[8] = {reinterpret_cast<void*>(&proxyPlayer), reinterpret_cast<void*>(&proxyPlayer), reinterpret_cast<void*>(&proxyPlayer), reinterpret_cast<void*>(&proxyPlayer),
                             reinterpret_cast<void*>(&proxyPlayer), reinterpret_cast<void*>(&proxyPlayer), reinterpret_cast<void*>(&proxyPlayer), reinterpret_cast<void*>(&proxyPlayer)};
void* g_provider[2] = {g_providerVtable, nullptr};

template <class T> T& at(void* base, std::size_t offset) { return *reinterpret_cast<T*>(static_cast<std::byte*>(base) + offset); }
int memoryException(unsigned code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
std::byte* relTarget(std::byte* site, std::size_t at, std::uint8_t opcode)
{
    std::uint8_t op = 0;
    std::int32_t rel = 0;
    if (!site || !memory::copyGuarded(site + at - 1, &op, 1) || op != opcode || !memory::copyGuarded(site + at, &rel, 4)) return nullptr;
    return site + at + 4 + rel;
}
bool readU32(std::byte* site, std::size_t at, std::uint32_t* out) { return site && memory::copyGuarded(site + at, out, 4); }
bool readU8(std::byte* site, std::size_t at, std::uint32_t* out)
{
    std::uint8_t v = 0;
    if (!site || !memory::copyGuarded(site + at, &v, 1)) return false;
    *out = v;
    return true;
}
void addStrong(void* control) { if (control) _InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<std::byte*>(control) + 8)); }
void releaseStrong(void* control)
{
    auto* const base = static_cast<std::byte*>(control);
    if (_InterlockedDecrement(reinterpret_cast<volatile long*>(base + 8)) != 0) return;
    auto* const vtable = *static_cast<void***>(control);
    reinterpret_cast<void(__fastcall*)(void*)>(vtable[0])(control);
    if (_InterlockedDecrement(reinterpret_cast<volatile long*>(base + 0xc)) == 0) reinterpret_cast<void(__fastcall*)(void*)>(vtable[1])(control);
}
void releaseWeak(void* control)
{
    auto* const base = static_cast<std::byte*>(control);
    if (_InterlockedDecrement(reinterpret_cast<volatile long*>(base + 0xc)) == 0) reinterpret_cast<void(__fastcall*)(void*)>((*static_cast<void***>(control))[1])(control);
}
bool releaseStrongGuarded(void* control)
{
    __try { releaseStrong(control); return true; } __except (memoryException(GetExceptionCode())) { return false; }
}

struct Build {
    void* realHandler; void* realPlayer; std::int32_t dimension; const std::int32_t* center; std::uint32_t radius;
    void* out[2];
    int stage;
};
bool buildSessionGuarded(Build* b)
{
    __try {
        b->stage = 1;
        g_levelDimension(g_s.level, b->out, b->dimension);
        if (!b->out[0] || !b->out[1]) return false;
        g_s.dimensionObject = b->out[0];
        g_s.source = at<void*>(b->out[0], kDimensionSource);
        if (!g_s.source) return false;
        at<void*>(g_s.player, kPlayerDimension) = b->out[0];
        at<void*>(g_s.player, kPlayerDimensionControl) = b->out[1];
        b->out[0] = b->out[1] = nullptr;
        b->stage = 2;
        void* const keyAt = g_entityKey(static_cast<std::byte*>(b->realPlayer) + 8);
        const std::uint64_t key = keyAt ? *static_cast<std::uint64_t*>(keyAt) : 0;
        void* const tracker = g_s.player + g_trackerOffset;
        g_trackerInit(tracker, g_s.source, &key);
        if (!at<void*>(tracker, kTrackerView)) return false;
        g_s.tracker = true;
        at<std::uint32_t>(tracker, kTrackerRadius) = b->radius + kTrackerMargin;
        const float direction[3] = {0, 0, 1};
        g_trackerMove(tracker, b->center, b->radius, direction, 0.0f);
        b->stage = 0;
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
bool buildHandlerGuarded(Build* b)
{
    __try {
        b->stage = 3;
        alignas(16) std::byte deps[kDepsSize] = {};
        auto* const h = static_cast<std::byte*>(b->realHandler);
        const auto copy = [&](std::size_t d, std::size_t f) { at<void*>(deps, d) = at<void*>(h, f); };
        copy(0x00, 0x80); copy(0x08, 0x88); copy(0x10, 0x90); copy(0x18, 0x98); copy(0x20, 0x78); copy(0x28, 0xa0);
        at<void*>(deps, 0x30) = g_s.clientInstance;
        copy(0x38, 0x60); copy(0x40, 0x68); copy(0x48, 0x70); copy(0x50, 0xa8);
        copy(0x58, 0x18); copy(0x60, 0x20); addStrong(at<void*>(deps, 0x60));
        copy(0x68, 0x170); copy(0x70, 0x178); copy(0x78, 0x180); addStrong(at<void*>(deps, 0x70));
        copy(0x80, 0x1a0); copy(0x88, 0x1a8); copy(0x90, 0x1b0);
        copy(0x98, 0x1b8); copy(0xa0, 0x1c0); copy(0xa8, 0x1c8); copy(0xb0, 0x1d0);
        copy(0x140, 0x260); copy(0x148, 0x268); addStrong(at<void*>(deps, 0x148));
        copy(0x150, 0x270); copy(0x158, 0x278); addStrong(at<void*>(deps, 0x158));
        copy(0x178, 0x280); copy(0x180, 0x288); addStrong(at<void*>(deps, 0x180));
        copy(0x160, 0x188); copy(0x168, 0x190); copy(0x170, 0x198);
        copy(0x1d8, 0x400); copy(0x1e0, 0x408); copy(0x1e8, 0x410);
        at<std::uint32_t>(deps, 0x1f0) = at<std::uint32_t>(h, 0x3a8);
        at<std::uint64_t>(deps, 0x210) = 15;
        copy(0x218, 0x430);
        copy(0x220, 0x480); copy(0x228, 0x488); copy(0x230, 0x490);
        copy(0x238, 0x498); copy(0x240, 0x4a0); copy(0x248, 0x4a8);
        g_handlerCtor(g_s.handler, deps);
        g_s.constructed = true;
        b->stage = 4;
        for (const std::size_t f : {std::size_t{0xb0}, std::size_t{0xb8}, std::size_t{0xc0}, std::size_t{0x168}, std::size_t{0x2b0}, std::size_t{0x2b8}, std::size_t{0x2c0}})
            at<void*>(g_s.handler, f) = at<void*>(h, f);
        addStrong(at<void*>(g_s.handler, 0xb8)); addStrong(at<void*>(g_s.handler, 0x2b8));
        for (std::size_t f = 0xc8; f < 0xcb; ++f) at<std::uint8_t>(g_s.handler, f) = at<std::uint8_t>(h, f);
        at<void*>(g_s.handler, 0x3f8) = g_nothingObject;
        b->stage = 0;
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
bool moveGuarded(const std::int32_t* center, std::uint32_t radius)
{
    __try {
        void* const tracker = g_s.player + g_trackerOffset;
        const float direction[3] = {0, 0, 1};
        g_trackerMove(tracker, center, radius, direction, 0.0f);
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
void teardownSessionGuarded()
{
    __try {
        if (g_s.tracker) {
            void* const tracker = g_s.player + g_trackerOffset;
            void* const view = at<void*>(tracker, kTrackerView);
            if (view) {
                void* const registry = at<void*>(at<void*>(view, g_viewDimension), g_registryOffset);
                reinterpret_cast<RemoveFn>((*static_cast<void***>(registry))[g_removeSlot / 8])(registry, at<std::uint32_t>(tracker, kTrackerId));
                at<void*>(tracker, kTrackerView) = nullptr;
                reinterpret_cast<DestroyFn>((*static_cast<void***>(view))[0])(view, 1);
            }
            g_s.tracker = false;
        }
        if (void* const control = at<void*>(g_s.player, kPlayerDimensionControl)) {
            at<void*>(g_s.player, kPlayerDimensionControl) = nullptr;
            releaseWeak(control);
        }
    } __except (memoryException(GetExceptionCode())) {}
}
bool tickGuarded()
{
    __try {
        reinterpret_cast<TickFn>((*reinterpret_cast<void***>(g_s.handler))[kTickSlot])(g_s.handler);
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
bool dispatchGuarded(void* object, void* control, void* networkId)
{
    __try {
        void* const dispatcher = at<void*>(object, kPacketDispatcher);
        if (!dispatcher) return false;
        void* shared[2] = {object, control};
        reinterpret_cast<DispatchFn>((*static_cast<void***>(dispatcher))[kDispatchSlot])(dispatcher, networkId, g_s.handler, shared);
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
bool statusGuarded(std::int32_t cx, std::int32_t cz, std::int32_t radius, std::int32_t minIndex, std::int32_t maxIndex, Status* out)
{
    __try {
        void** const vtable = *static_cast<void***>(g_s.source);
        for (std::int32_t dx = -radius; dx <= radius; ++dx)
            for (std::int32_t dz = -radius; dz <= radius; ++dz) {
                const std::int32_t position[2] = {cx + dx, cz + dz};
                void* got[2] = {};
                reinterpret_cast<ExistingFn>(vtable[kExistingSlot])(g_s.source, got, position);
                if (got[0]) {
                    ++out->chunks;
                    auto* const begin = at<std::byte*>(got[0], kSubChunks);
                    auto* const end = at<std::byte*>(got[0], kSubChunks + 8);
                    const std::size_t count = begin && end > begin ? (std::min)(static_cast<std::size_t>(end - begin) / kSubChunkSize, kMaxSubChunks) : 0;
                    bool known = false;
                    for (std::size_t i = 0; i < count; ++i) {
                        auto* const entry = begin + i * kSubChunkSize;
                        const auto state = at<std::int32_t>(entry, kSubChunkStatus);
                        known = known || state != 0;
                        const std::int32_t index = at<std::int8_t>(entry, kSubChunkIndex);
                        if (index < minIndex || index > maxIndex) continue;
                        if (state == 2) ++out->waiting;
                        else if (state > 2 && state < 7) ++out->loading;
                        else if (state == 7) ++out->loaded;
                    }
                    if (known && at<std::uint8_t>(got[0], kChunkState) >= 11) ++out->finished;
                }
                if (got[1]) releaseStrong(got[1]);
            }
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
bool hasChunkGuarded(std::int32_t cx, std::int32_t cz, bool* has)
{
    __try {
        const std::int32_t position[2] = {cx, cz};
        void* got[2] = {};
        reinterpret_cast<ExistingFn>((*static_cast<void***>(g_s.source))[kExistingSlot])(g_s.source, got, position);
        bool known = false;
        if (got[0]) {
            auto* const begin = at<std::byte*>(got[0], kSubChunks);
            auto* const end = at<std::byte*>(got[0], kSubChunks + 8);
            const std::size_t count = begin && end > begin ? (std::min)(static_cast<std::size_t>(end - begin) / kSubChunkSize, kMaxSubChunks) : 0;
            for (std::size_t i = 0; i < count && !known; ++i) known = at<std::int32_t>(begin + i * kSubChunkSize, kSubChunkStatus) != 0;
        }
        *has = got[0] && known && at<std::uint8_t>(got[0], kChunkState) >= 2;
        if (got[1]) releaseStrong(got[1]);
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}

struct Swap { void* list; std::byte* buffer; std::size_t count; std::byte* end; };
bool consumeSideGuarded(ConsumeFn original, Swap* s)
{
    auto* const owner = static_cast<std::byte*>(s->list);
    void* saved[3] = {};
    void* savedProvider = nullptr;
    bool swapped = false;
    __try {
        std::memcpy(saved, owner, sizeof saved);
        savedProvider = at<void*>(owner, g_providerOffset);
        at<void*>(owner, 0) = s->buffer; at<void*>(owner, 8) = s->buffer + s->count * kEntrySize; at<void*>(owner, 0x10) = s->buffer + s->count * kEntrySize;
        at<void*>(owner, g_providerOffset) = g_provider;
        swapped = true;
        original(s->list);
        s->end = at<std::byte*>(owner, 8);
        std::memcpy(owner, saved, sizeof saved);
        at<void*>(owner, g_providerOffset) = savedProvider;
        return true;
    } __except (memoryException(GetExceptionCode())) {
        if (swapped) { std::memcpy(owner, saved, sizeof saved); at<void*>(owner, g_providerOffset) = savedProvider; }
        return false;
    }
}
bool finishJobGuarded(void* task, void** owner, std::int32_t* dimension)
{
    __try {
        void* const capture = at<void*>(task, kFinishCapture);
        void* const job = capture ? at<void*>(capture, kCaptureJob) : nullptr;
        *owner = capture ? at<void*>(capture, 0) : nullptr;
        *dimension = job ? at<std::int32_t>(job, kJobDimension) : -1;
        return *owner != nullptr;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
bool jobDimensionGuarded(void** job, std::int32_t* dimension)
{
    __try {
        if (!job || !job[0] || !job[1]) return false;
        *dimension = at<std::int32_t>(job[0], kJobDimension);
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
void freeSession()
{
    if (g_s.thunks) VirtualFree(g_s.thunks, 0, MEM_RELEASE);
    _aligned_free(g_s.clientVtable);
    _aligned_free(g_s.clientInstance);
    _aligned_free(g_s.handler);
    _aligned_free(g_s.player);
    g_s = {};
}
}

bool initialize()
{
    auto* const ctor = net::site(Site::TerrainHandlerCtor);
    auto* const tick = net::site(Site::TerrainHandlerTick);
    auto* const levelDimension = net::site(Site::TerrainLevelDimension);
    auto* const prepare = net::site(Site::TerrainPrepareRegion);
    auto* const move = net::site(Site::TerrainTrackerMove);
    auto* const destroy = net::site(Site::TerrainDestroyRegion);
    g_enqueue = net::site(Site::TerrainJobEnqueue);
    g_consume = net::site(Site::TerrainJobConsume);
    g_finish = net::site(Site::TerrainJobFinish);
    g_handlerCtor = reinterpret_cast<HandlerCtorFn>(ctor);
    g_tick = reinterpret_cast<TickFn>(tick);
    g_levelDimension = reinterpret_cast<LevelDimensionFn>(levelDimension);
    g_trackerMove = reinterpret_cast<TrackerMoveFn>(move);
    static constexpr std::uint8_t kVtableStore[] = {0x49, 0x89, 0x04, 0x24};
    std::uint8_t store[sizeof kVtableStore] = {};
    std::uint8_t load[3] = {};
    std::int32_t rel = 0;
    if (ctor && memory::copyGuarded(ctor + 0xa79, load, 3) && load[0] == 0x48 && load[1] == 0x8d && load[2] == 0x05
        && memory::copyGuarded(ctor + 0xa80, store, sizeof store) && std::memcmp(store, kVtableStore, sizeof store) == 0 && memory::copyGuarded(ctor + 0xa7c, &rel, 4))
        g_handlerVtable = ctor + 0xa80 + rel;
    g_entityKey = reinterpret_cast<EntityKeyFn>(relTarget(prepare, 0x16, 0xe8));
    g_trackerInit = reinterpret_cast<TrackerInitFn>(relTarget(prepare, 0x32, 0xe8));
    std::uint32_t viewField = 0;
    const bool fields = readU32(prepare, 0x25, &g_trackerOffset) && readU32(destroy, 0x5a, &viewField) && viewField == g_trackerOffset + kTrackerView
        && readU8(destroy, 0x66, &g_viewDimension) && readU32(destroy, 0x6a, &g_registryOffset) && readU8(destroy, 0x7a, &g_removeSlot)
        && readU32(g_consume, 0x42, &g_providerOffset) && g_providerOffset > 0 && g_providerOffset < 0x400 && g_trackerOffset > 0 && g_trackerOffset < 0x2000;
    const bool ok = fields && g_handlerCtor && g_handlerVtable && g_tick && g_levelDimension && g_trackerMove && g_entityKey && g_trackerInit
        && g_enqueue && g_consume && g_finish
        && memory::inGameModule(g_handlerVtable) && static_cast<void* const*>(g_handlerVtable)[kTickSlot] == reinterpret_cast<void*>(g_tick);
    if (!ok) {
        const bool tickSlot = g_handlerVtable && memory::inGameModule(g_handlerVtable) && static_cast<void* const*>(g_handlerVtable)[kTickSlot] == reinterpret_cast<void*>(g_tick);
        log().warn(L"Bot: the game's terrain functions for far bots are unavailable (fields {} vtable {} tick {} level {} tracker {}/{}/{} jobs {}/{}/{} tick slot {}); "
                   L"walking along a path works only near you in your dimension",
            fields, g_handlerVtable != nullptr, g_tick != nullptr, g_levelDimension != nullptr, g_trackerInit != nullptr, g_trackerMove != nullptr, g_entityKey != nullptr,
            g_enqueue != nullptr, g_consume != nullptr, g_finish != nullptr, tickSlot);
        return false;
    }
    g_ready.store(true, std::memory_order_release);
    const auto* const base = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));
    log().info(L"Bot: the terrain proxy for far bots is ready (handler RVA {:#x}, tracker +{:#x}, registry +{:#x}, provider +{:#x})",
        static_cast<std::uintptr_t>(ctor - base), g_trackerOffset, g_registryOffset, g_providerOffset);
    return true;
}
bool available() { return g_ready.load(std::memory_order_acquire); }
void disable() { g_ready.store(false, std::memory_order_release); }
void* enqueueTarget() { return g_enqueue; }
void* consumeTarget() { return g_consume; }
void* finishTarget() { return g_finish; }

void onEnqueue(EnqueueFn original, void* list, void** job)
{
    if (g_divert.load(std::memory_order_acquire)) {
        std::int32_t dimension = -1;
        if (jobDimensionGuarded(job, &dimension) && dimension == g_s.dimension) {
            addStrong(job[1]);
            std::lock_guard lock(g_sideMutex);
            g_side.push_back({job[0], job[1]});
            return;
        }
    }
    original(list, job);
}
void onConsume(ConsumeFn original, void* list)
{
    original(list);
    if (!g_divert.load(std::memory_order_acquire)) return;
    std::vector<SideJob> jobs;
    { std::lock_guard lock(g_sideMutex); jobs.swap(g_side); }
    if (jobs.empty()) return;
    std::vector<std::byte> entries(jobs.size() * kEntrySize + 16);
    auto* const buffer = reinterpret_cast<std::byte*>((reinterpret_cast<std::uintptr_t>(entries.data()) + 15) & ~std::uintptr_t{15});
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        auto* const e = buffer + i * kEntrySize;
        memory::copyGuarded(static_cast<std::byte*>(jobs[i].job) + kJobKey, e + 4, 8);
        memory::copyGuarded(static_cast<std::byte*>(jobs[i].job) + kJobKey2, e + 0xc, 4);
        at<void*>(e, 0x10) = jobs[i].job;
        at<void*>(e, 0x18) = jobs[i].control;
    }
    Swap swap{list, buffer, jobs.size(), nullptr};
    if (!consumeSideGuarded(original, &swap)) {
        g_divert.store(false, std::memory_order_release);
        g_active.store(false, std::memory_order_release);
        notice::failOnce("BotTerrainConsume", L"Bot: processing far terrain faulted; walking along a path works only near you in your dimension",
            "Bot: processing far terrain faulted");
        return;
    }
    std::lock_guard lock(g_sideMutex);
    std::vector<SideJob> left;
    for (auto* e = buffer; swap.end && e < swap.end; e += kEntrySize) left.push_back({at<void*>(e, 0x10), at<void*>(e, 0x18)});
    g_side.insert(g_side.begin(), left.begin(), left.end());
}
void* onFinish(FinishFn original, void* task, void* a, void* b, void* c)
{
    if (g_divert.load(std::memory_order_acquire)) {
        void* owner = nullptr;
        std::int32_t dimension = -1;
        if (finishJobGuarded(task, &owner, &dimension) && dimension == g_s.dimension) {
            void* const saved = at<void*>(owner, g_providerOffset);
            at<void*>(owner, g_providerOffset) = g_provider;
            void* const result = original(task, a, b, c);
            at<void*>(owner, g_providerOffset) = saved;
            return result;
        }
    }
    return original(task, a, b, c);
}

bool begin(const void* owner, std::int32_t dimension, protocol::Vec3 feet, std::string& error)
{
    if (!available()) { error = "the game's terrain functions are unavailable"; return false; }
    if (dimension < 0 || dimension > 2) { error = "the bot's dimension is unknown"; return false; }
    const std::int32_t center[3] = {static_cast<std::int32_t>(std::floor(feet.x)), static_cast<std::int32_t>(std::floor(feet.y)), static_cast<std::int32_t>(std::floor(feet.z))};
    constexpr std::uint32_t kRadius = 6 * 16;
    if (g_active.load(std::memory_order_acquire) && g_s.owner == owner && g_s.dimension == dimension) {
        g_s.position[0] = feet.x; g_s.position[1] = feet.y; g_s.position[2] = feet.z;
        if (!moveGuarded(center, kRadius)) { end(true); error = "moving the terrain window faulted"; return false; }
        return true;
    }
    if (g_active.load(std::memory_order_acquire)) end(true);
    auto* const system = static_cast<std::byte*>(net::networkSystem());
    void* box = nullptr;
    void* handler = nullptr;
    void* vtable = nullptr;
    if (!system || !memory::copyGuarded(system + kSubClientBoxes, &box, 8) || !box || !memory::copyGuarded(box, &handler, 8) || !handler
        || !memory::copyGuarded(handler, &vtable, 8) || vtable != g_handlerVtable) {
        error = "the game's chunk receiver was not found"; return false;
    }
    void* const player = GameData::instance().player();
    const auto serial = GameData::instance().playerSerial();
    const void* const playerVtable = GameData::instance().playerVtable();
    void* level = nullptr;
    if (!player || !memory::copyGuarded(player, &vtable, 8) || (playerVtable && vtable != playerVtable)
        || !memory::copyGuarded(static_cast<std::byte*>(player) + kPlayerLevel, &level, 8) || !level) {
        error = "the main player is not available"; return false;
    }
    if (g_s.player && (g_s.realHandler != handler || g_s.realPlayer != player || g_s.playerSerial != serial || g_s.level != level)) g_s = {};
    if (!g_s.constructed) {
        void* clientInstance = nullptr;
        if (!memory::copyGuarded(static_cast<std::byte*>(handler) + kHandlerClientInstance, &clientInstance, 8) || !clientInstance
            || !memory::copyGuarded(clientInstance, &vtable, 8) || !memory::inGameModule(vtable)) {
            error = "the game's client instance was not found"; return false;
        }
        g_s = {};
        g_s.realHandler = handler; g_s.realPlayer = player; g_s.playerSerial = serial; g_s.level = level;
        g_s.player = static_cast<std::byte*>(_aligned_malloc(kPlayerSize, 16));
        g_s.handler = static_cast<std::byte*>(_aligned_malloc(kHandlerAlloc, 16));
        g_s.clientInstance = static_cast<std::byte*>(_aligned_malloc(0x40, 16));
        if (!g_s.player || !g_s.handler || !g_s.clientInstance) { freeSession(); error = "out of memory"; return false; }
        std::memset(g_s.player, 0, kPlayerSize); std::memset(g_s.handler, 0, kHandlerAlloc); std::memset(g_s.clientInstance, 0, 0x40);
        memory::copyGuarded(player, g_s.player, 8);
        at<void*>(g_s.player, kPlayerLevel) = level;
        memory::copyGuarded(static_cast<std::byte*>(player) + kPlayerConnection, g_s.player + kPlayerConnection, 8);
        at<void*>(g_s.player, kPlayerPosition) = g_s.position;
        at<void*>(g_s.player, kPlayerRotation) = g_s.rotation;
        void** const realVtable = static_cast<void**>(vtable);
        std::size_t slots = 0;
        for (void* slot = nullptr; slots < 2048 && memory::copyGuarded(realVtable + slots, &slot, 8) && memory::inGameModule(slot); ++slots) {}
        if (slots <= kGetLocalPlayerSlot) { freeSession(); error = "the game's client instance vtable is unexpected"; return false; }
        g_s.thunks = static_cast<std::byte*>(VirtualAlloc(nullptr, slots * 32, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        g_s.clientVtable = static_cast<void**>(_aligned_malloc(slots * 8, 16));
        if (!g_s.thunks || !g_s.clientVtable) { freeSession(); error = "out of memory"; return false; }
        for (std::size_t i = 0; i < slots; ++i) {
            void* target = nullptr;
            memory::copyGuarded(realVtable + i, &target, 8);
            auto* const t = g_s.thunks + i * 32;
            t[0] = std::byte{0x48}; t[1] = std::byte{0xb9}; std::memcpy(t + 2, &clientInstance, 8);
            t[10] = std::byte{0xff}; t[11] = std::byte{0x25}; std::memset(t + 12, 0, 4);
            std::memcpy(t + 16, &target, 8);
            g_s.clientVtable[i] = t;
        }
        DWORD old = 0;
        VirtualProtect(g_s.thunks, slots * 32, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), g_s.thunks, slots * 32);
        g_s.clientVtable[kGetLocalPlayerSlot] = reinterpret_cast<void*>(&proxyPlayer);
        at<void*>(g_s.clientInstance, 0) = g_s.clientVtable;
        Build build{handler, player, dimension, center, kRadius, {}, 0};
        if (!buildHandlerGuarded(&build)) {
            if (build.stage == 3 && !g_s.constructed) freeSession(); else g_s = {};
            error = std::format("preparing the bot's chunk receiver faulted (stage {})", build.stage);
            notice::failOnce("BotTerrainBuild", std::wstring(L"Bot: ") + std::wstring(error.begin(), error.end()), "Bot: " + error);
            return false;
        }
    }
    g_s.owner = owner; g_s.dimension = dimension;
    g_s.position[0] = feet.x; g_s.position[1] = feet.y; g_s.position[2] = feet.z;
    Build build{handler, player, dimension, center, kRadius, {}, 0};
    if (!buildSessionGuarded(&build)) {
        if (build.out[1]) releaseWeak(build.out[1]);
        teardownSessionGuarded();
        g_s.owner = nullptr; g_s.dimension = -1; g_s.dimensionObject = g_s.source = nullptr;
        error = std::format("preparing the bot's terrain faulted (stage {})", build.stage);
        notice::failOnce("BotTerrainBuild", std::wstring(L"Bot: ") + std::wstring(error.begin(), error.end()), "Bot: " + error);
        return false;
    }
    g_active.store(true, std::memory_order_release);
    if (g_logs.fetch_add(1, std::memory_order_relaxed) < 32)
        log().info(L"Bot: borrowed the game's terrain parts for a bot in dimension {} around ({:.1f}, {:.1f}, {:.1f})", dimension, feet.x, feet.y, feet.z);
    return true;
}
bool active() { return g_active.load(std::memory_order_acquire); }
const void* owner() { return active() ? g_s.owner : nullptr; }
std::int32_t dimension() { return active() ? g_s.dimension : -1; }

bool hasChunk(std::int32_t chunkX, std::int32_t chunkZ)
{
    bool has = false;
    return active() && hasChunkGuarded(chunkX, chunkZ, &has) && has;
}
namespace {
bool hand(std::uint16_t id, protocol::Bytes body)
{
    if (!active()) return false;
    auto packet = gamepacket::create(id);
    if (!packet || !gamepacket::read(packet, body)) return false;
    void* connection = nullptr;
    if (!memory::copyGuarded(static_cast<std::byte*>(g_s.realPlayer) + kPlayerConnection, &connection, 8) || !connection) return false;
    if (!dispatchGuarded(packet.object(), packet.control(), static_cast<std::byte*>(connection) + kConnectionNetworkId)) {
        packet.abandon();
        g_divert.store(false, std::memory_order_release);
        g_active.store(false, std::memory_order_release);
        notice::failOnce("BotTerrainHand", L"Bot: handing far terrain to the game faulted; walking along a path works only near you in your dimension",
            "Bot: handing far terrain to the game faulted");
        return false;
    }
    return true;
}
}
bool handLevelChunk(protocol::Bytes body) { return hand(0x3a, body); }
bool handPacket(protocol::Bytes whole)
{
    const auto header = protocol::readHeader(whole);
    return header && hand(static_cast<std::uint16_t>(header->id), whole.subspan(header->bodyOffset));
}

void pump()
{
    if (!active()) return;
    if (GameData::instance().player() != g_s.realPlayer || GameData::instance().playerSerial() != g_s.playerSerial) { end(false); return; }
    const auto mine = gamepath::playerDimension();
    g_divert.store(mine && *mine != g_s.dimension, std::memory_order_release);
    if (!tickGuarded()) {
        g_divert.store(false, std::memory_order_release);
        g_active.store(false, std::memory_order_release);
        notice::failOnce("BotTerrainTick", L"Bot: the borrowed chunk receiver faulted; walking along a path works only near you in your dimension",
            "Bot: the borrowed chunk receiver faulted");
    }
}

std::optional<Status> status(std::int32_t chunkX, std::int32_t chunkZ, std::int32_t radius, std::int32_t minIndex, std::int32_t maxIndex)
{
    if (!active()) return std::nullopt;
    Status s;
    if (!statusGuarded(chunkX, chunkZ, radius, minIndex, maxIndex, &s)) return std::nullopt;
    return s;
}
std::optional<World> world()
{
    if (!active()) return std::nullopt;
    return World{g_s.level, g_s.dimensionObject, g_s.source};
}

void end(bool worldAlive)
{
    if (worldAlive && g_s.player && (GameData::instance().player() != g_s.realPlayer || GameData::instance().playerSerial() != g_s.playerSerial)) worldAlive = false;
    const bool wasActive = g_active.exchange(false, std::memory_order_acq_rel);
    g_divert.store(false, std::memory_order_release);
    std::vector<SideJob> jobs;
    { std::lock_guard lock(g_sideMutex); jobs.swap(g_side); }
    if (!g_s.player) return;
    if (worldAlive) {
        for (const auto& job : jobs) releaseStrongGuarded(job.control);
        teardownSessionGuarded();
        g_s.owner = nullptr; g_s.dimension = -1; g_s.dimensionObject = g_s.source = nullptr;
    } else {
        g_s = {};
    }
    if (wasActive && g_logs.fetch_add(1, std::memory_order_relaxed) < 32)
        log().info(L"Bot: returned the borrowed terrain parts{}", worldAlive ? L"" : L" (the world was gone; left them)");
}

}
