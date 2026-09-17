#include "game/BlockWrite.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <Windows.h>

#include "core/Logger.h"
#include "game/BlockRegistry.h"
#include "game/GameData.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

namespace tsukuyomi::blockwrite {

namespace {

using RegionSetBlockFn = bool(__fastcall*)(void*, const void*, const void*, unsigned int,
                                           unsigned int, void*);

using SubChunkSetFn = void(__fastcall*)(void*, unsigned int, unsigned int, const void*);

std::atomic<void*> g_region{nullptr};
std::atomic<unsigned long long> g_regionSeen{0};
std::atomic<void*> g_regionPlayer{nullptr};
std::atomic<void*> g_regionVtable{nullptr};

enum class Refusal : int {
    None = 0,
    NoRegion,
    NoPlayer,
    NotedNoPlayer,
    PlayerChanged,
    BadVtable,
    VtableChanged,
};
std::atomic<Refusal> g_refusal{Refusal::None};
std::atomic<unsigned int> g_mode{0};
std::atomic<unsigned int> g_updateFlags{0};
std::atomic<void*> g_actor{nullptr};

std::atomic<bool> g_wroteGhost{false};

std::atomic<void*> g_renderRegion{nullptr};
std::atomic<unsigned long long> g_renderRegionSeen{0};

std::atomic<std::uint64_t> g_regionGeneration{0};

bool onCurrentStack(const void* address)
{
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    const auto at = reinterpret_cast<ULONG_PTR>(address);
    return at >= low && at < high;
}

__declspec(noinline) bool safeRead(void* to, const void* from, std::size_t size) noexcept
{
    __try {
        std::memcpy(to, from, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}

void noteRegion(void* region, unsigned int mode, unsigned int updateFlags, void* actor)
{
    if (region == nullptr) {
        return;
    }
    if (onCurrentStack(region)) {
        return;
    }
    static std::atomic<bool> announced{false};
    g_region.store(region, std::memory_order_relaxed);
    g_regionSeen.store(GetTickCount64(), std::memory_order_relaxed);
    g_regionPlayer.store(GameData::instance().player(), std::memory_order_relaxed);
    {
        void* vtable = nullptr;
        if (memory::isReadable(region, sizeof(vtable))) {
            std::memcpy(&vtable, region, sizeof(vtable));
        }
        g_regionVtable.store(vtable, std::memory_order_relaxed);
    }
    g_mode.store(mode > 1 ? 0U : mode, std::memory_order_relaxed);
    g_updateFlags.store(updateFlags, std::memory_order_relaxed);
    g_actor.store(actor, std::memory_order_relaxed);
}

void* region()
{
    return g_region.load(std::memory_order_relaxed);
}

void noteRenderRegion(void* region)
{
    if (region == nullptr || onCurrentStack(region)) {
        return;
    }
    static std::atomic<void*> announced{nullptr};
    if (void* const previous = g_renderRegion.exchange(region, std::memory_order_relaxed);
        previous != region && previous != nullptr) {
        g_regionGeneration.fetch_add(1, std::memory_order_release);
    }
    g_renderRegionSeen.store(GetTickCount64(), std::memory_order_relaxed);
}

void* renderRegion()
{
    constexpr unsigned long long kFreshMs = 1000;
    const auto seen = g_renderRegionSeen.load(std::memory_order_relaxed);
    if (seen == 0 || GetTickCount64() - seen > kFreshMs) {
        return nullptr;
    }
    return g_renderRegion.load(std::memory_order_relaxed);
}

std::uint64_t regionGeneration()
{
    return g_regionGeneration.load(std::memory_order_acquire);
}

__declspec(noinline) bool callRegionSetBlock(RegionSetBlockFn fn, void* region,
                                             const void* pos, const void* block,
                                             unsigned int mode, unsigned int flags,
                                             void* actor)
{
    __try {
        return fn(region, pos, block, mode, flags, actor);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool regionIsAlive(void* region)
{
    if (region == nullptr) {
        return false;
    }
    __try {
        auto* const bs = static_cast<std::uint8_t*>(region);
        void* const vtable = *reinterpret_cast<void**>(bs);
        if (!memory::inGameModule(vtable)) {
            return false;
        }
        void* const chunkSource = *reinterpret_cast<void**>(bs + 0x28);
        if (chunkSource == nullptr) {
            return false;
        }
        void* const sourceVtable = *reinterpret_cast<void**>(chunkSource);
        return memory::inGameModule(sourceVtable);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool placeAtRegion(void* region, const BlockPos& at, const void* block)
{
    if (region == nullptr || block == nullptr) {
        return false;
    }
    if (!regionIsAlive(region)) {
        return false;
    }
    if (findSubChunk(region, at.x, at.y, at.z) == nullptr) {
        return false;
    }
    const Scanner& scanner = Scanner::instance();
    if (!scanner.found(Target::BlockSourceSetBlock)) {
        return false;
    }
    const auto setBlock = scanner.addressAs<RegionSetBlockFn>(Target::BlockSourceSetBlock);
    if (setBlock == nullptr) {
        return false;
    }
    const int pos[3] = {at.x, at.y, at.z};
    beginSelfWrite();
    beginRenderWrite();
    const bool ok = callRegionSetBlock(setBlock, region, pos, block,
                                       g_mode.load(std::memory_order_relaxed),
                                       g_updateFlags.load(std::memory_order_relaxed),
                                       g_actor.load(std::memory_order_relaxed));
    endRenderWrite();
    endSelfWrite();
    return ok;
}

bool placeAt(const BlockPos& at, const void* block, bool* wroteGhost)
{
    if (wroteGhost != nullptr) {
        *wroteGhost = false;
    }
    void* const target = g_region.load(std::memory_order_relaxed);
    if (target == nullptr || block == nullptr) {
        g_refusal.store(Refusal::NoRegion, std::memory_order_relaxed);
        return false;
    }
    void* const player = GameData::instance().player();
    void* const notedPlayer = g_regionPlayer.load(std::memory_order_relaxed);
    if (player == nullptr || notedPlayer == nullptr || player != notedPlayer) {
        constexpr unsigned long long kFreshMs = 60000;
        const auto seen = g_regionSeen.load(std::memory_order_relaxed);
        if (seen == 0 || GetTickCount64() - seen > kFreshMs) {
            g_refusal.store(player == nullptr        ? Refusal::NoPlayer
                            : notedPlayer == nullptr ? Refusal::NotedNoPlayer
                                                     : Refusal::PlayerChanged,
                            std::memory_order_relaxed);
            return false;
        }
    } else {
        void* const noted = g_regionVtable.load(std::memory_order_relaxed);
        void* vtable = nullptr;
        if (noted == nullptr || !memory::isReadable(target, 0x40)
            || !memory::inGameModule(noted)) {
            g_refusal.store(Refusal::BadVtable, std::memory_order_relaxed);
            return false;
        }
        std::memcpy(&vtable, target, sizeof(vtable));
        if (vtable != noted) {
            g_refusal.store(Refusal::VtableChanged, std::memory_order_relaxed);
            return false;
        }
    }
    g_refusal.store(Refusal::None, std::memory_order_relaxed);
    const Scanner& scanner = Scanner::instance();
    if (!scanner.found(Target::BlockSourceSetBlock)) {
        return false;
    }
    const auto setBlock = scanner.addressAs<RegionSetBlockFn>(Target::BlockSourceSetBlock);
    if (setBlock == nullptr) {
        return false;
    }
    const int pos[3] = {at.x, at.y, at.z};
    g_wroteGhost.store(false, std::memory_order_relaxed);
    const bool ok = setBlock(target, pos, block, g_mode.load(std::memory_order_relaxed),
                             g_updateFlags.load(std::memory_order_relaxed),
                             g_actor.load(std::memory_order_relaxed));
    if (wroteGhost != nullptr) {
        *wroteGhost = g_wroteGhost.load(std::memory_order_relaxed);
    }
    return ok;
}

namespace {

using StorageGetFn = const void*(__fastcall*)(void* storage, unsigned int index);

constexpr std::ptrdiff_t kStorageBase = 0x20;
constexpr std::size_t kGetSlot = 3;

struct Placed {
    void* subChunk;
    unsigned int layer;
    unsigned int index;
    const void* before;
};

std::mutex g_placedLock;
std::vector<Placed> g_placed;

std::atomic<bool> g_placing{false};
std::atomic<const void*> g_air{nullptr};
std::atomic<unsigned long> g_placingThread{0};

std::mutex g_knownLock;
std::unordered_set<const void*> g_known;

constexpr std::size_t kStorageVtableSlots = 32;
std::atomic<const void*> g_storageVtables[kStorageVtableSlots] = {};
std::atomic<std::size_t> g_storageVtableCount{0};
std::mutex g_storageVtableLock;
std::atomic<std::size_t> g_storageVtableRejected{0};

bool isKnownStorageVtable(const void* vtable)
{
    const std::size_t count =
        std::min(g_storageVtableCount.load(std::memory_order_acquire), kStorageVtableSlots);
    for (std::size_t i = 0; i < count; ++i) {
        if (g_storageVtables[i].load(std::memory_order_relaxed) == vtable) {
            return true;
        }
    }
    return false;
}

void noteStorageVtablesOf(void* subChunk)
{
    if (subChunk == nullptr) {
        return;
    }
    for (std::ptrdiff_t layer = 0; layer < 2; ++layer) {
        void* storage = nullptr;
        if (!safeRead(&storage, static_cast<const std::uint8_t*>(subChunk) + kStorageBase
                                    + layer * 8,
                      sizeof(storage))
            || storage == nullptr) {
            continue;
        }
        void* vtable = nullptr;
        if (!safeRead(&vtable, storage, sizeof(vtable)) || vtable == nullptr
            || !memory::inGameModule(vtable) || isKnownStorageVtable(vtable)) {
            continue;
        }
        std::lock_guard<std::mutex> lock(g_storageVtableLock);
        if (isKnownStorageVtable(vtable)) {
            continue;
        }
        const std::size_t count = g_storageVtableCount.load(std::memory_order_relaxed);
        if (count >= kStorageVtableSlots) {
            static std::atomic<bool> told{false};
            if (!told.exchange(true, std::memory_order_relaxed)) {
                log().warn(L"BlockWrite: ran out of storage vtable samples ({}); chunks of an "
                           L"unseen type will read as unreadable",
                           kStorageVtableSlots);
            }
            continue;
        }
        g_storageVtables[count].store(vtable, std::memory_order_relaxed);
        g_storageVtableCount.store(count + 1, std::memory_order_release);
    }
}

__declspec(noinline) const void* callStorageGet(void* fn, void* storage, unsigned int index)
{
    __try {
        return reinterpret_cast<StorageGetFn>(fn)(storage, index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

const void* readBlock(void* subChunk, unsigned int layer, unsigned int index)
{
    if (layer > 1 || index >= 4096) {
        return nullptr;
    }
    void* storage = nullptr;
    if (!safeRead(&storage, static_cast<const std::uint8_t*>(subChunk) + kStorageBase
                                + static_cast<std::ptrdiff_t>(layer) * 8,
                  sizeof(storage))
        || storage == nullptr) {
        return nullptr;
    }
    void* vtable = nullptr;
    if (!safeRead(&vtable, storage, sizeof(vtable)) || vtable == nullptr
        || !memory::inGameModule(vtable)) {
        return nullptr;
    }
    if (!isKnownStorageVtable(vtable)) {
        g_storageVtableRejected.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    void* fn = nullptr;
    if (!safeRead(&fn, static_cast<const std::uint8_t*>(vtable) + kGetSlot * 8, sizeof(fn))
        || fn == nullptr || !memory::inGameModule(fn)) {
        return nullptr;
    }

    const void* got = callStorageGet(fn, storage, index);

    std::lock_guard<std::mutex> guard(g_knownLock);
    if (g_known.empty() || g_known.find(got) == g_known.end()) {
        return nullptr;
    }
    return got;
}

}

void setKnownBlocks(const void* const* blocks, std::size_t count)
{
    std::lock_guard<std::mutex> guard(g_knownLock);
    g_known.clear();
    for (std::size_t i = 0; i < count; ++i) {
        if (blocks[i] != nullptr) {
            g_known.insert(blocks[i]);
        }
    }
}

namespace {

constexpr unsigned int subChunkIndex(int x, int y, int z)
{
    return static_cast<unsigned int>(((x & 15) << 8) | ((z & 15) << 4) | (y & 15));
}

constexpr std::size_t kSubChunkSlots = 4096;

struct SubChunkSlot {
    std::atomic<int> baseX{0};
    std::atomic<int> baseY{0};
    std::atomic<int> baseZ{0};
    std::atomic<void*> chunk{nullptr};
    std::atomic<void*> chunkAlt{nullptr};
    std::atomic<bool> used{false};
};

SubChunkSlot g_subChunks[kSubChunkSlots];
std::atomic<std::size_t> g_subChunkCount{0};

thread_local int t_writePos[3] = {0, 0, 0};
thread_local bool t_writePosValid = false;

thread_local void* t_renderSubChunk = nullptr;
thread_local unsigned int t_renderIndex = 0;

thread_local bool t_nudgeGuard = false;

constexpr std::size_t kSlotIndexSize = 8192;
std::atomic<std::uint64_t> g_slotIndexKey[kSlotIndexSize] = {};
std::atomic<std::uint32_t> g_slotIndexVal[kSlotIndexSize] = {};

std::uint64_t slotIndexKeyOf(int baseX, int baseY, int baseZ)
{
    const std::uint64_t ux = static_cast<std::uint32_t>(baseX >> 4) & 0x1FFFFFu;
    const std::uint64_t uy = static_cast<std::uint32_t>(baseY >> 4) & 0x1FFFFFu;
    const std::uint64_t uz = static_cast<std::uint32_t>(baseZ >> 4) & 0x1FFFFFu;
    return (std::uint64_t{1} << 63) | (ux << 42) | (uy << 21) | uz;
}

std::size_t slotIndexHash(std::uint64_t key)
{
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return static_cast<std::size_t>(key) & (kSlotIndexSize - 1);
}

void slotIndexInsert(int baseX, int baseY, int baseZ, std::size_t slot)
{
    const std::uint64_t key = slotIndexKeyOf(baseX, baseY, baseZ);
    std::size_t h = slotIndexHash(key);
    for (std::size_t n = 0; n < kSlotIndexSize; ++n, h = (h + 1) & (kSlotIndexSize - 1)) {
        std::uint64_t cur = g_slotIndexKey[h].load(std::memory_order_acquire);
        if (cur == 0
            && g_slotIndexKey[h].compare_exchange_strong(cur, key, std::memory_order_acq_rel)) {
            g_slotIndexVal[h].store(static_cast<std::uint32_t>(slot + 1),
                                    std::memory_order_release);
            return;
        }
        if (cur == key) {
            g_slotIndexVal[h].store(static_cast<std::uint32_t>(slot + 1),
                                    std::memory_order_release);
            return;
        }
    }
}

std::size_t findSubChunkSlot(int baseX, int baseY, int baseZ)
{
    const std::uint64_t key = slotIndexKeyOf(baseX, baseY, baseZ);
    std::size_t h = slotIndexHash(key);
    for (std::size_t n = 0; n < kSlotIndexSize; ++n, h = (h + 1) & (kSlotIndexSize - 1)) {
        const std::uint64_t cur = g_slotIndexKey[h].load(std::memory_order_acquire);
        if (cur == 0) {
            return kSubChunkSlots;
        }
        if (cur != key) {
            continue;
        }
        const std::uint32_t v = g_slotIndexVal[h].load(std::memory_order_acquire);
        if (v == 0 || v > kSubChunkSlots) {
            return kSubChunkSlots;
        }
        const std::size_t i = v - 1;
        if (g_subChunks[i].used.load(std::memory_order_acquire)
            && g_subChunks[i].baseX.load(std::memory_order_relaxed) == baseX
            && g_subChunks[i].baseY.load(std::memory_order_relaxed) == baseY
            && g_subChunks[i].baseZ.load(std::memory_order_relaxed) == baseZ) {
            return i;
        }
        return kSubChunkSlots;
    }
    return kSubChunkSlots;
}

void storeSubChunkPointer(SubChunkSlot& slot, void* subChunk)
{
    void* const was = slot.chunk.exchange(subChunk, std::memory_order_acq_rel);
    if (was != nullptr && was != subChunk) {
        slot.chunkAlt.store(was, std::memory_order_release);
    }
}

void learnSubChunk(void* subChunk, unsigned int index)
{
    if (!t_writePosValid || subChunk == nullptr || index >= 4096) {
        return;
    }
    const int x = t_writePos[0];
    const int y = t_writePos[1];
    const int z = t_writePos[2];
    if (subChunkIndex(x, y, z) != index) {
        return;
    }
    if (!blocks::ghostBoxTouchesSubChunk(x >> 4 << 4, y >> 4 << 4, z >> 4 << 4)) {
        return;
    }
    const int baseX = x >> 4 << 4;
    const int baseY = y >> 4 << 4;
    const int baseZ = z >> 4 << 4;

    noteStorageVtablesOf(subChunk);

    const std::size_t at = findSubChunkSlot(baseX, baseY, baseZ);
    if (at < kSubChunkSlots) {
        storeSubChunkPointer(g_subChunks[at], subChunk);
        return;
    }
    if (g_subChunkCount.load(std::memory_order_acquire) >= kSubChunkSlots) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true, std::memory_order_relaxed)) {
            log().warn(L"BlockWrite: ran out of position-to-SubChunk slots ({} chunks)",
                       kSubChunkSlots);
        }
        return;
    }
    const std::size_t slot = g_subChunkCount.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= kSubChunkSlots) {
        return;
    }
    g_subChunks[slot].baseX.store(baseX, std::memory_order_relaxed);
    g_subChunks[slot].baseY.store(baseY, std::memory_order_relaxed);
    g_subChunks[slot].baseZ.store(baseZ, std::memory_order_relaxed);
    g_subChunks[slot].chunk.store(subChunk, std::memory_order_relaxed);
    g_subChunks[slot].chunkAlt.store(nullptr, std::memory_order_relaxed);
    g_subChunks[slot].used.store(true, std::memory_order_release);
    slotIndexInsert(baseX, baseY, baseZ, slot);
}

}

void noteWritePos(const int pos[3])
{
    if (pos == nullptr) {
        t_writePosValid = false;
        return;
    }
    t_writePos[0] = pos[0];
    t_writePos[1] = pos[1];
    t_writePos[2] = pos[2];
    t_writePosValid = true;
}

const void* readWorldAt(int x, int y, int z)
{
    const std::size_t at = findSubChunkSlot(x >> 4 << 4, y >> 4 << 4, z >> 4 << 4);
    if (at >= kSubChunkSlots) {
        return nullptr;
    }
    void* const chunk = g_subChunks[at].chunk.load(std::memory_order_acquire);
    if (chunk == nullptr) {
        return nullptr;
    }
    return readBlock(chunk, 0, subChunkIndex(x, y, z));
}

bool readWorldExtraAt(int x, int y, int z, const void*& out)
{
    out = nullptr;
    const std::size_t at = findSubChunkSlot(x >> 4 << 4, y >> 4 << 4, z >> 4 << 4);
    if (at >= kSubChunkSlots) {
        return false;
    }
    void* const chunk = g_subChunks[at].chunk.load(std::memory_order_acquire);
    if (chunk == nullptr) {
        return false;
    }
    void* storage = nullptr;
    if (!safeRead(&storage, static_cast<const std::uint8_t*>(chunk) + kStorageBase + 8,
                  sizeof(storage))) {
        return false;
    }
    if (storage == nullptr) {
        return true;
    }
    const void* const got = readBlock(chunk, 1, subChunkIndex(x, y, z));
    if (got == nullptr) {
        return false;
    }
    out = got;
    return true;
}

void noteSubChunkAt(int baseX, int baseY, int baseZ, void* subChunk)
{
    if (subChunk == nullptr) {
        return;
    }
    noteStorageVtablesOf(subChunk);
    const std::size_t at = findSubChunkSlot(baseX, baseY, baseZ);
    if (at < kSubChunkSlots) {
        storeSubChunkPointer(g_subChunks[at], subChunk);
        return;
    }
    if (g_subChunkCount.load(std::memory_order_acquire) >= kSubChunkSlots) {
        return;
    }
    const std::size_t slot = g_subChunkCount.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= kSubChunkSlots) {
        return;
    }
    g_subChunks[slot].baseX.store(baseX, std::memory_order_relaxed);
    g_subChunks[slot].baseY.store(baseY, std::memory_order_relaxed);
    g_subChunks[slot].baseZ.store(baseZ, std::memory_order_relaxed);
    g_subChunks[slot].chunk.store(subChunk, std::memory_order_relaxed);
    g_subChunks[slot].chunkAlt.store(nullptr, std::memory_order_relaxed);
    g_subChunks[slot].used.store(true, std::memory_order_release);
    slotIndexInsert(baseX, baseY, baseZ, slot);
}

void forgetSubChunkAt(int baseX, int baseY, int baseZ)
{
    const std::size_t at = findSubChunkSlot(baseX, baseY, baseZ);
    if (at >= kSubChunkSlots) {
        return;
    }
    g_subChunks[at].chunk.store(nullptr, std::memory_order_release);
    g_subChunks[at].chunkAlt.store(nullptr, std::memory_order_release);
}

void storageVtableStats(std::size_t& known, std::size_t& rejected)
{
    known = std::min(g_storageVtableCount.load(std::memory_order_acquire), kStorageVtableSlots);
    rejected = g_storageVtableRejected.exchange(0, std::memory_order_relaxed);
}

void noteSubChunkAlt(int baseX, int baseY, int baseZ, void* subChunk)
{
    if (subChunk == nullptr) {
        return;
    }
    const std::size_t at = findSubChunkSlot(baseX, baseY, baseZ);
    if (at >= kSubChunkSlots) {
        return;
    }
    if (g_subChunks[at].chunk.load(std::memory_order_acquire) == subChunk) {
        return;
    }
    g_subChunks[at].chunkAlt.store(subChunk, std::memory_order_release);
}

bool worldPosOfSubChunkWrite(void* subChunk, unsigned int index, int out[3])
{
    if (subChunk == nullptr || out == nullptr || index >= 4096) {
        return false;
    }
    const std::size_t used = std::min(g_subChunkCount.load(std::memory_order_acquire),
                                      kSubChunkSlots);
    for (std::size_t i = 0; i < used; ++i) {
        if (!g_subChunks[i].used.load(std::memory_order_acquire)) {
            continue;
        }
        if (g_subChunks[i].chunk.load(std::memory_order_relaxed) != subChunk
            && g_subChunks[i].chunkAlt.load(std::memory_order_relaxed) != subChunk) {
            continue;
        }
        out[0] = g_subChunks[i].baseX.load(std::memory_order_relaxed)
                 + static_cast<int>((index >> 8) & 15u);
        out[1] = g_subChunks[i].baseY.load(std::memory_order_relaxed)
                 + static_cast<int>(index & 15u);
        out[2] = g_subChunks[i].baseZ.load(std::memory_order_relaxed)
                 + static_cast<int>((index >> 4) & 15u);
        return true;
    }
    return false;
}

void* subChunkAt(int baseX, int baseY, int baseZ)
{
    const std::size_t at = findSubChunkSlot(baseX, baseY, baseZ);
    if (at >= kSubChunkSlots) {
        return nullptr;
    }
    return g_subChunks[at].chunk.load(std::memory_order_acquire);
}

std::size_t knownSubChunkCount()
{
    std::size_t live = 0;
    const std::size_t used = std::min(g_subChunkCount.load(std::memory_order_acquire),
                                      kSubChunkSlots);
    for (std::size_t i = 0; i < used; ++i) {
        if (g_subChunks[i].used.load(std::memory_order_acquire)
            && g_subChunks[i].chunk.load(std::memory_order_relaxed) != nullptr) {
            ++live;
        }
    }
    return live;
}

void forgetSubChunks()
{
    for (std::size_t i = 0; i < kSubChunkSlots; ++i) {
        g_subChunks[i].used.store(false, std::memory_order_release);
        g_subChunks[i].chunk.store(nullptr, std::memory_order_relaxed);
        g_subChunks[i].chunkAlt.store(nullptr, std::memory_order_relaxed);
    }
    for (std::size_t h = 0; h < kSlotIndexSize; ++h) {
        g_slotIndexVal[h].store(0, std::memory_order_relaxed);
        g_slotIndexKey[h].store(0, std::memory_order_release);
    }
    g_subChunkCount.store(0, std::memory_order_release);
}

void noteWorldChanged()
{
    forgetSubChunks();
    g_renderRegion.store(nullptr, std::memory_order_relaxed);
    g_renderRegionSeen.store(0, std::memory_order_relaxed);
    g_region.store(nullptr, std::memory_order_relaxed);
    g_regionVtable.store(nullptr, std::memory_order_relaxed);
    g_regionPlayer.store(nullptr, std::memory_order_relaxed);
    g_actor.store(nullptr, std::memory_order_relaxed);
    g_regionGeneration.fetch_add(1, std::memory_order_release);
}

namespace {
std::atomic<int> g_selfWrite{0};
std::atomic<int> g_renderWrite{0};
}

void beginRenderWrite()
{
    g_renderWrite.fetch_add(1, std::memory_order_acq_rel);
}

void endRenderWrite()
{
    if (g_renderWrite.fetch_sub(1, std::memory_order_acq_rel) <= 0) {
        g_renderWrite.store(0, std::memory_order_release);
    }
}

bool renderWriting()
{
    return g_renderWrite.load(std::memory_order_acquire) > 0;
}

void beginSelfWrite()
{
    g_selfWrite.fetch_add(1, std::memory_order_acq_rel);
}

void endSelfWrite()
{
    if (g_selfWrite.fetch_sub(1, std::memory_order_acq_rel) <= 0) {
        g_selfWrite.store(0, std::memory_order_release);
    }
}

bool selfWriting()
{
    return g_selfWrite.load(std::memory_order_acquire) > 0;
}

void beginPlacement(const void* air)
{
    g_air.store(air, std::memory_order_relaxed);
    g_placingThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_placing.store(true, std::memory_order_release);
}

void endPlacement()
{
    g_placing.store(false, std::memory_order_release);
}

std::size_t placedCount()
{
    std::lock_guard<std::mutex> guard(g_placedLock);
    std::size_t count = 0;
    for (const Placed& one : g_placed) {
        if (one.layer == 0) {
            ++count;
        }
    }
    return count;
}

const void* onSubChunkWrite(void* subChunk, unsigned int layer, unsigned int index,
                            const void* block)
{
    if (layer == 0) {
        if (renderWriting()) {
            if (t_renderSubChunk == nullptr) {
                t_renderSubChunk = subChunk;
                t_renderIndex = index;
            }
            if (t_nudgeGuard) {
                const void* const air = g_air.load(std::memory_order_relaxed);
                const void* const before = readBlock(subChunk, layer, index);
                if (before == nullptr || before != air) {
                    return before;
                }
                g_wroteGhost.store(true, std::memory_order_relaxed);
            }
        } else {
            learnSubChunk(subChunk, index);
        }
    }

    if (!g_placing.load(std::memory_order_acquire) || subChunk == nullptr || renderWriting()) {
        return block;
    }
    if (GetCurrentThreadId() != g_placingThread.load(std::memory_order_relaxed)) {
        return block;
    }
    const void* const air = g_air.load(std::memory_order_relaxed);
    const void* const before = readBlock(subChunk, layer, index);
    if (before == nullptr) {
        return nullptr;
    }
    if (before != air) {
        return before;
    }

    {
        std::lock_guard<std::mutex> guard(g_placedLock);
        g_placed.push_back(Placed{subChunk, layer, index, before});
    }
    g_wroteGhost.store(true, std::memory_order_relaxed);

    return block;
}

bool placeGhostActor(void* region, const BlockPos& at, const void* block, const void* air,
                     StorageSpot* spot)
{
    if (spot != nullptr) {
        *spot = StorageSpot{};
    }
    if (region == nullptr || block == nullptr || air == nullptr) {
        return false;
    }
    if (!regionIsAlive(region)) {
        return false;
    }
    if (findSubChunk(region, at.x, at.y, at.z) == nullptr) {
        return false;
    }
    const Scanner& scanner = Scanner::instance();
    if (!scanner.found(Target::BlockSourceSetBlock) || !scanner.found(Target::SubChunkSetBlock)) {
        return false;
    }
    const auto setBlock = scanner.addressAs<RegionSetBlockFn>(Target::BlockSourceSetBlock);
    const auto setStorage = scanner.addressAs<SubChunkSetFn>(Target::SubChunkSetBlock);
    if (setBlock == nullptr || setStorage == nullptr) {
        return false;
    }

    const int pos[3] = {at.x, at.y, at.z};
    t_renderSubChunk = nullptr;
    t_renderIndex = 0;

    beginSelfWrite();
    beginRenderWrite();
    const bool ok = callRegionSetBlock(setBlock, region, pos, block,
                                       g_mode.load(std::memory_order_relaxed),
                                       g_updateFlags.load(std::memory_order_relaxed),
                                       g_actor.load(std::memory_order_relaxed));
    void* const sub = t_renderSubChunk;
    const unsigned int index = t_renderIndex;
    if (ok && sub != nullptr && index == subChunkIndex(at.x, at.y, at.z)) {
        setStorage(sub, 0, index, air);
        if (spot != nullptr) {
            spot->subChunk = sub;
            spot->index = index;
        }
    }
    endRenderWrite();
    endSelfWrite();
    t_renderSubChunk = nullptr;
    return ok;
}

bool removeGhostActor(void* region, const BlockPos& at, const void* block, const void* air)
{
    if (region == nullptr || block == nullptr || air == nullptr) {
        return false;
    }
    if (!regionIsAlive(region)) {
        return false;
    }
    void* const sub = findSubChunk(region, at.x, at.y, at.z);
    if (sub == nullptr) {
        return false;
    }
    const Scanner& scanner = Scanner::instance();
    if (!scanner.found(Target::BlockSourceSetBlock) || !scanner.found(Target::SubChunkSetBlock)) {
        return false;
    }
    const auto setBlock = scanner.addressAs<RegionSetBlockFn>(Target::BlockSourceSetBlock);
    const auto setStorage = scanner.addressAs<SubChunkSetFn>(Target::SubChunkSetBlock);
    if (setBlock == nullptr || setStorage == nullptr) {
        return false;
    }
    const unsigned int index = subChunkIndex(at.x, at.y, at.z);
    const int pos[3] = {at.x, at.y, at.z};
    beginSelfWrite();
    beginRenderWrite();
    setStorage(sub, 0, index, block);
    const bool ok = callRegionSetBlock(setBlock, region, pos, air,
                                       g_mode.load(std::memory_order_relaxed),
                                       g_updateFlags.load(std::memory_order_relaxed),
                                       g_actor.load(std::memory_order_relaxed));
    if (!ok) {
        setStorage(sub, 0, index, air);
    }
    endRenderWrite();
    endSelfWrite();
    return ok;
}

namespace {
using GetChunkAtFn = void*(__fastcall*)(void*, const int*);

std::atomic<std::size_t> g_findWhy[kFindWhyCount] = {};

void noteFindWhy(std::size_t which)
{
    if (which < kFindWhyCount) {
        g_findWhy[which].fetch_add(1, std::memory_order_relaxed);
    }
}

thread_local bool t_lastWasMissingChunk = false;
thread_local bool t_lastWasOutsideWorld = false;
}

bool lastFindWasMissingChunk()
{
    return t_lastWasMissingChunk;
}

bool lastFindWasOutsideWorld()
{
    return t_lastWasOutsideWorld;
}

void findSubChunkStats(std::size_t out[kFindWhyCount])
{
    for (std::size_t i = 0; i < kFindWhyCount; ++i) {
        out[i] = g_findWhy[i].exchange(0, std::memory_order_relaxed);
    }
}

__declspec(noinline) void* findSubChunk(void* region, int x, int y, int z)
{
    t_lastWasMissingChunk = false;
    t_lastWasOutsideWorld = false;
    if (region == nullptr) {
        noteFindWhy(0);
        return nullptr;
    }
    if (!regionIsAlive(region)) {
        noteFindWhy(1);
        return nullptr;
    }
    __try {
        auto* const bs = static_cast<std::uint8_t*>(region);
        void** const vtable = *reinterpret_cast<void***>(bs);
        if (vtable == nullptr) {
            noteFindWhy(2);
            return nullptr;
        }
        const auto getChunkAt = reinterpret_cast<GetChunkAtFn>(vtable[0x140 / 8]);
        if (getChunkAt == nullptr
            || !memory::inGameModule(reinterpret_cast<const void*>(getChunkAt))) {
            noteFindWhy(3);
            return nullptr;
        }
        const int base = y >> 4 << 4;
        const int at[3] = {x >> 4 << 4, base, z >> 4 << 4};
        auto* const chunk = static_cast<std::uint8_t*>(getChunkAt(bs, at));
        if (chunk == nullptr) {
            noteFindWhy(4);
            t_lastWasMissingChunk = true;
            return nullptr;
        }
        auto* const meta = *reinterpret_cast<std::uint8_t**>(chunk + 0x58);
        if (meta == nullptr) {
            noteFindWhy(5);
            return nullptr;
        }
        std::int16_t low = 0;
        std::int16_t high = 0;
        std::memcpy(&low, meta + 0xc8, sizeof(low));
        std::memcpy(&high, meta + 0xca, sizeof(high));
        if (base < low || base >= high) {
            noteFindWhy(6);
            t_lastWasOutsideWorld = true;
            return nullptr;
        }
        auto* const table = *reinterpret_cast<std::uint8_t**>(chunk + 0x140);
        if (table == nullptr) {
            noteFindWhy(7);
            return nullptr;
        }
        const int index = (base >> 4) - (static_cast<int>(low) >> 4);
        if (index < 0) {
            noteFindWhy(8);
            return nullptr;
        }
        std::uint8_t* const sub = table + static_cast<std::size_t>(index) * 0x68;
        std::int8_t tag = 0;
        std::memcpy(&tag, sub + 0x61, sizeof(tag));
        if (tag != static_cast<std::int8_t>(base >> 4)) {
            noteFindWhy(9);
            return nullptr;
        }
        noteFindWhy(11);
        return sub;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        noteFindWhy(10);
        return nullptr;
    }
}

bool nudgeRegion(void* region, const BlockPos& at, const void* block, const void* air,
                 void** outSubChunk, StorageSpot* hold)
{
    if (outSubChunk != nullptr) {
        *outSubChunk = nullptr;
    }
    if (hold != nullptr) {
        *hold = StorageSpot{};
    }
    if (region == nullptr || block == nullptr || air == nullptr) {
        return false;
    }
    if (!regionIsAlive(region)) {
        return false;
    }
    const Scanner& scanner = Scanner::instance();
    if (!scanner.found(Target::BlockSourceSetBlock) || !scanner.found(Target::SubChunkSetBlock)) {
        return false;
    }
    const auto setBlock = scanner.addressAs<RegionSetBlockFn>(Target::BlockSourceSetBlock);
    const auto setStorage = scanner.addressAs<SubChunkSetFn>(Target::SubChunkSetBlock);
    if (setBlock == nullptr || setStorage == nullptr) {
        return false;
    }

    const int pos[3] = {at.x, at.y, at.z};
    t_renderSubChunk = nullptr;
    t_renderIndex = 0;
    g_air.store(air, std::memory_order_relaxed);
    g_wroteGhost.store(false, std::memory_order_relaxed);

    beginSelfWrite();
    beginRenderWrite();
    t_nudgeGuard = true;
    callRegionSetBlock(setBlock, region, pos, block, g_mode.load(std::memory_order_relaxed),
                       g_updateFlags.load(std::memory_order_relaxed),
                       g_actor.load(std::memory_order_relaxed));
    t_nudgeGuard = false;
    void* const sub = t_renderSubChunk;
    const unsigned int index = t_renderIndex;
    const bool wrote = g_wroteGhost.load(std::memory_order_relaxed);
    if (sub != nullptr && index == subChunkIndex(at.x, at.y, at.z)
        && outSubChunk != nullptr) {
        *outSubChunk = sub;
    }
    if (wrote && sub != nullptr && index == subChunkIndex(at.x, at.y, at.z)) {
        if (hold != nullptr) {
            hold->subChunk = sub;
            hold->index = index;
        } else {
            setStorage(sub, 0, index, air);
        }
    }
    endRenderWrite();
    endSelfWrite();
    t_renderSubChunk = nullptr;
    return wrote && sub != nullptr;
}

void clearSpot(const StorageSpot& spot, const void* air)
{
    if (spot.subChunk == nullptr || air == nullptr || spot.index >= 4096) {
        return;
    }
    const Scanner& scanner = Scanner::instance();
    if (!scanner.found(Target::SubChunkSetBlock)) {
        return;
    }
    const auto setStorage = scanner.addressAs<SubChunkSetFn>(Target::SubChunkSetBlock);
    if (setStorage == nullptr) {
        return;
    }
    beginSelfWrite();
    beginRenderWrite();
    setStorage(spot.subChunk, 0, spot.index, air);
    endRenderWrite();
    endSelfWrite();
}

std::size_t restoreAll()
{
    std::vector<Placed> mine;
    {
        std::lock_guard<std::mutex> guard(g_placedLock);
        mine.swap(g_placed);
    }
    if (mine.empty()) {
        return 0;
    }
    const Scanner& scanner = Scanner::instance();
    if (!scanner.found(Target::SubChunkSetBlock)) {
        return 0;
    }
    const auto setBlock = scanner.addressAs<SubChunkSetFn>(Target::SubChunkSetBlock);
    if (setBlock == nullptr) {
        return 0;
    }
    for (const Placed& one : mine) {
        setBlock(one.subChunk, one.layer, one.index, one.before);
    }
    return mine.size();
}

}
