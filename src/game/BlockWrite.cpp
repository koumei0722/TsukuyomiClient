#include "game/SchematicWorld.h"
#include "game/BlockWrite.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <Windows.h>

#include "core/Logger.h"
#include "game/BlockRegistry.h"
#include "game/SubChunkTable.h"
#include "memory/Memory.h"

namespace tsukuyomi::blockwrite {

namespace {

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

void noteRenderRegion(void* region)
{
    if (region == nullptr || onCurrentStack(region)) {
        return;
    }
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

__declspec(noinline) bool regionIsAlive(void* region)
{
    if (!memory::plausiblePointer(region)) {
        return false;
    }
    __try {
        auto* const bs = static_cast<std::uint8_t*>(region);
        void* const vtable = *reinterpret_cast<void**>(bs);
        if (!memory::inGameModule(vtable)) {
            return false;
        }
        void* const chunkSource = *reinterpret_cast<void**>(bs + 0x28);
        if (!memory::plausiblePointer(chunkSource)) {
            return false;
        }
        void* const sourceVtable = *reinterpret_cast<void**>(chunkSource);
        return memory::inGameModule(sourceVtable);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace {

using StorageGetFn = const void*(__fastcall*)(void* storage, unsigned int index);

constexpr std::ptrdiff_t kStorageBase = 0x20;
constexpr std::size_t kGetSlot = 3;

using KnownSet = std::unordered_set<const void*>;
std::mutex g_knownLock;
std::atomic<const KnownSet*> g_known{nullptr};
std::atomic<unsigned> g_knownReaders{0};
std::unique_ptr<KnownSet> g_knownOwned;
std::vector<std::unique_ptr<KnownSet>> g_knownRetired;

void publishKnownLocked(std::unique_ptr<KnownSet> next)
{
    g_known.store(next.get(), std::memory_order_seq_cst);
    if (g_knownOwned) {
        g_knownRetired.push_back(std::move(g_knownOwned));
    }
    g_knownOwned = std::move(next);
    if (g_knownReaders.load(std::memory_order_seq_cst) == 0) g_knownRetired.clear();
}

constexpr std::size_t kStorageVtableSlots = 32;
std::atomic<const void*> g_storageVtables[kStorageVtableSlots] = {};
std::atomic<std::size_t> g_storageVtableCount{0};
std::mutex g_storageVtableLock;

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
        return nullptr;
    }
    void* fn = nullptr;
    if (!safeRead(&fn, static_cast<const std::uint8_t*>(vtable) + kGetSlot * 8, sizeof(fn))
        || fn == nullptr || !memory::inGameModule(fn)) {
        return nullptr;
    }

    const void* got = callStorageGet(fn, storage, index);

    g_knownReaders.fetch_add(1, std::memory_order_seq_cst);
    const KnownSet* const known = g_known.load(std::memory_order_seq_cst);
    const bool valid = known != nullptr && known->find(got) != known->end();
    g_knownReaders.fetch_sub(1, std::memory_order_seq_cst);
    return valid ? got : nullptr;
}

}

void setKnownBlocks(const void* const* blocks, std::size_t count)
{
    auto next = std::make_unique<KnownSet>();
    for (std::size_t i = 0; i < count; ++i) {
        if (blocks[i] != nullptr) {
            next->insert(blocks[i]);
        }
    }
    std::lock_guard<std::mutex> guard(g_knownLock);
    publishKnownLocked(next->empty() ? nullptr : std::move(next));
}

namespace {

subchunks::Table g_subChunks;
constexpr std::size_t kNoSlot = subchunks::Table::kNone;

std::size_t findSubChunkSlot(int baseX, int baseY, int baseZ)
{
    return g_subChunks.find(baseX, baseY, baseZ);
}

void warnSlotsFull()
{
    static std::atomic<bool> told{false};
    if (!told.exchange(true, std::memory_order_relaxed)) {
        log().warn(L"BlockWrite: ran out of position-to-SubChunk slots ({} chunks) while "
                   L"learning the chunks the schematic covers; the blocks of the chunks "
                   L"past this cannot be read, so no ghost or color box is decided there",
                   subchunks::Table::kMaxSlots);
    }
}

void warnPointerNotesFull()
{
    static std::atomic<bool> told{false};
    if (!told.exchange(true, std::memory_order_relaxed)) {
        log().warn(L"BlockWrite: ran out of SubChunk-to-position notes ({}); writes to chunks "
                   L"loaded after this do not update the ghost until the schematic is redrawn",
                   subchunks::Table::kMaxSlots);
    }
}

std::size_t addSubChunkSlot(int baseX, int baseY, int baseZ)
{
    const std::size_t slot = g_subChunks.add(baseX, baseY, baseZ);
    if (slot == kNoSlot) {
        warnSlotsFull();
    }
    return slot;
}

void storeSubChunkPointer(std::size_t at, void* subChunk)
{
    subchunks::Table::Slot* const slot = g_subChunks.slot(at);
    if (slot == nullptr) {
        return;
    }
    void* const alt = slot->ptr[1].load(std::memory_order_acquire);
    void* const was = slot->ptr[0].exchange(subChunk, std::memory_order_acq_rel);
    if (was != nullptr && was != subChunk) {
        slot->ptr[1].store(was, std::memory_order_release);
    }
    if (subChunk != nullptr && subChunk != was && subChunk != alt
        && !g_subChunks.notePointer(subChunk, at)) {
        warnPointerNotesFull();
    }
}

}

bool readWorldExtraAt(int x, int y, int z, const void*& out)
{
    out = nullptr;
    const std::size_t at = findSubChunkSlot(x >> 4 << 4, y >> 4 << 4, z >> 4 << 4);
    if (at == kNoSlot) {
        return false;
    }
    void* const chunk = g_subChunks.slot(at)->ptr[0].load(std::memory_order_acquire);
    if (chunk == nullptr) {
        return false;
    }
    return readExtraOf(chunk, beIndexOf(x, y, z), out);
}

const void* readBlockOf(void* subChunk, unsigned layer, unsigned beIndex)
{
    if (!subChunk) return nullptr;
    return readBlock(subChunk, layer, beIndex);
}

bool readExtraOf(void* chunk, unsigned beIndex, const void*& out)
{
    out = nullptr;
    if (!chunk || beIndex >= 4096) return false;
    void* storage = nullptr;
    if (!safeRead(&storage, static_cast<const std::uint8_t*>(chunk) + kStorageBase + 8,
                  sizeof(storage))) {
        return false;
    }
    if (storage == nullptr) {
        return true;
    }
    const void* const got = readBlockOf(chunk, 1, beIndex);
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
    std::size_t at = findSubChunkSlot(baseX, baseY, baseZ);
    if (at == kNoSlot) {
        at = addSubChunkSlot(baseX, baseY, baseZ);
        if (at == kNoSlot) {
            return;
        }
    }
    storeSubChunkPointer(at, subChunk);
}

void forgetSubChunkAt(int baseX, int baseY, int baseZ)
{
    const std::size_t at = findSubChunkSlot(baseX, baseY, baseZ);
    if (at == kNoSlot) {
        return;
    }
    subchunks::Table::Slot* const slot = g_subChunks.slot(at);
    slot->ptr[0].store(nullptr, std::memory_order_release);
    slot->ptr[1].store(nullptr, std::memory_order_release);
}

void noteLiveSubChunk(void* subChunk)
{
    noteStorageVtablesOf(subChunk);
}

bool worldPosOfSubChunkWrite(void* subChunk, unsigned int index, int out[3])
{
    if (subChunk == nullptr || out == nullptr || index >= 4096) {
        return false;
    }
    const std::size_t at = g_subChunks.findByPointer(subChunk);
    if (at == kNoSlot) {
        return false;
    }
    const subchunks::Table::Slot* const slot = g_subChunks.slot(at);
    out[0] = slot->baseX.load(std::memory_order_relaxed) + static_cast<int>((index >> 8) & 15u);
    out[1] = slot->baseY.load(std::memory_order_relaxed) + static_cast<int>(index & 15u);
    out[2] = slot->baseZ.load(std::memory_order_relaxed) + static_cast<int>((index >> 4) & 15u);
    return true;
}

void forgetSubChunks()
{
    g_subChunks.clear();
}

void noteWorldChanged(bool leftAWorld)
{
    schematicworld::onWorldChanged();
    blocks::forgetWorldLookups();
    if (leftAWorld) {
        blocks::setAirBlock(nullptr);
        std::lock_guard<std::mutex> guard(g_knownLock);
        publishKnownLocked(nullptr);
    }
    forgetSubChunks();
    g_renderRegion.store(nullptr, std::memory_order_relaxed);
    g_renderRegionSeen.store(0, std::memory_order_relaxed);
    g_regionGeneration.fetch_add(1, std::memory_order_release);
}

namespace {
using GetChunkAtFn = void*(__fastcall*)(void*, const int*);

thread_local bool t_lastWasMissingChunk = false;
thread_local bool t_lastWasOutsideWorld = false;
thread_local bool t_lastWasAboveTop = false;
}

bool lastFindWasMissingChunk()
{
    return t_lastWasMissingChunk;
}

bool lastFindWasOutsideWorld()
{
    return t_lastWasOutsideWorld;
}

bool lastFindWasAboveTop()
{
    return t_lastWasAboveTop;
}

constexpr std::ptrdiff_t kSourceLoose = 0x1c;

__declspec(noinline) void* getChunkLoose(GetChunkAtFn fn, std::uint8_t* bs, const int at[3])
{
    const std::uint8_t was = bs[kSourceLoose];
    if (was != 0) {
        return nullptr;
    }
    bs[kSourceLoose] = 1;
    void* got = nullptr;
    __try {
        got = fn(bs, at);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        got = nullptr;
    }
    bs[kSourceLoose] = was;
    return got;
}

__declspec(noinline) void* findChunk(void* region, int x, int z)
{
    t_lastWasMissingChunk = false;
    if (region == nullptr) {
        return nullptr;
    }
    if (!regionIsAlive(region)) {
        return nullptr;
    }
    __try {
        auto* const bs = static_cast<std::uint8_t*>(region);
        void** const vtable = *reinterpret_cast<void***>(bs);
        if (vtable == nullptr) {
            return nullptr;
        }
        const auto getChunkAt = reinterpret_cast<GetChunkAtFn>(vtable[0x140 / 8]);
        if (getChunkAt == nullptr
            || !memory::inGameModule(reinterpret_cast<const void*>(getChunkAt))) {
            return nullptr;
        }
        const int base = 0;
        const int at[3] = {x >> 4 << 4, base, z >> 4 << 4};
        auto* chunk = static_cast<std::uint8_t*>(getChunkAt(bs, at));
        if (chunk == nullptr) {
            chunk = static_cast<std::uint8_t*>(getChunkLoose(getChunkAt, bs, at));
        }
        if (chunk == nullptr) {
            t_lastWasMissingChunk = true;
            return nullptr;
        }
        return chunk;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

__declspec(noinline) void* findSubChunk(void* region, int x, int y, int z)
{
    t_lastWasOutsideWorld = false;
    t_lastWasAboveTop = false;
    auto* chunk = static_cast<std::uint8_t*>(findChunk(region, x, z));
    if (!chunk) return nullptr;
    const int base = y >> 4 << 4;
    __try {
        auto* const meta = *reinterpret_cast<std::uint8_t**>(chunk + 0x58);
        if (meta == nullptr) {
            return nullptr;
        }
        std::int16_t low = 0;
        std::int16_t high = 0;
        std::memcpy(&low, meta + 0xc8, sizeof(low));
        std::memcpy(&high, meta + 0xca, sizeof(high));
        if (base < low) {
            t_lastWasOutsideWorld = true;
            return nullptr;
        }
        if (base >= high) {
            t_lastWasAboveTop = true;
            return nullptr;
        }
        auto* const table = *reinterpret_cast<std::uint8_t**>(chunk + 0x1c8);
        if (table == nullptr) {
            return nullptr;
        }
        const int index = (base >> 4) - (static_cast<int>(low) >> 4);
        if (index < 0) {
            return nullptr;
        }
        std::uint8_t* const sub = table + static_cast<std::size_t>(index) * 0x68;
        std::int8_t tag = 0;
        std::memcpy(&tag, sub + 0x61, sizeof(tag));
        if (tag != static_cast<std::int8_t>(base >> 4)) {
            return nullptr;
        }
        return sub;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

}
