#include "game/DebugScreenWorld.h"

#include "core/Logger.h"
#include "core/Strings.h"
#include "game/BlockRegistry.h"
#include "game/BlockWrite.h"
#include "game/DebugScreenSurface.h"
#include "game/DebugScreenText.h"
#include "game/GameData.h"
#include "hooks/Detours.h"
#include "memory/Memory.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <unordered_set>

extern "C" float tsukuyomiCallCaptureXmm3(void* fn, void* level, int dim, float* xmm3Out);

namespace tsukuyomi::debugworld {

namespace {

LONG accessFilter(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

bool readPointer(const void* at, void*& out)
{
    __try {
        out = *static_cast<void* const*>(at);
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

bool readInt64(const void* at, std::int64_t& out)
{
    __try {
        out = *static_cast<const std::int64_t*>(at);
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

bool readInt32(const void* at, std::int32_t& out)
{
    __try {
        out = *static_cast<const std::int32_t*>(at);
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

bool readByte(const void* at, std::uint8_t& out)
{
    __try {
        out = *static_cast<const std::uint8_t*>(at);
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

bool looksLikeGameObject(void* candidate)
{
    if (!memory::plausiblePointer(candidate)) {
        return false;
    }
    void* vtable = nullptr;
    return readPointer(candidate, vtable) && memory::inGameModule(vtable);
}

bool readLightNibble(const void* subChunk, std::ptrdiff_t at, int index, int& out)
{
    void* array = nullptr;
    if (!readPointer(static_cast<const std::uint8_t*>(subChunk) + at, array)) {
        return false;
    }
    if (array == nullptr) {
        out = 0;
        return true;
    }
    std::uint8_t packed = 0;
    if (!memory::plausiblePointer(array)
        || !readByte(static_cast<std::uint8_t*>(array) + index / 2, packed)) {
        return false;
    }

    out = (index & 1) != 0 ? (packed >> 4) : (packed & 0x0F);
    return true;
}

constexpr std::ptrdiff_t kChunkSourceOriginX = 0x140;
constexpr std::ptrdiff_t kChunkSourceOriginZ = 0x148;
constexpr std::ptrdiff_t kChunkSourceWidth = 0x158;
constexpr std::ptrdiff_t kLevelChunkPosition = 0x78;

const void* storageGet(void* storage, unsigned int index);

bool chunkPositionIs(const std::uint8_t* chunk, int cx, int cz)
{
    const auto* const pos = reinterpret_cast<const std::int32_t*>(chunk + kLevelChunkPosition);
    return pos[0] == cx && pos[1] == cz;
}

void* gridChunkAt(void* region, int x, int z)
{
    __try {
        const auto* const source = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(region) + 0x28);
        if (source == nullptr) {
            return nullptr;
        }
        const std::int32_t originX = *reinterpret_cast<const std::int32_t*>(source + kChunkSourceOriginX);
        const std::int32_t originZ = *reinterpret_cast<const std::int32_t*>(source + kChunkSourceOriginZ);
        const std::int32_t width = *reinterpret_cast<const std::int32_t*>(source + kChunkSourceWidth);
        const auto begin = *reinterpret_cast<const std::uintptr_t*>(source + 0x170);
        const auto end = *reinterpret_cast<const std::uintptr_t*>(source + 0x178);
        if (width <= 0 || width > 128 || end <= begin || (end - begin) / 16 != static_cast<std::uintptr_t>(width) * width) {
            return nullptr;
        }
        const int cx = x >> 4;
        const int cz = z >> 4;
        const int dx = cx - originX;
        const int dz = cz - originZ;
        if (dx < 0 || dz < 0 || dx >= width || dz >= width) {
            return nullptr;
        }
        for (const int index : {dz * width + dx, dx * width + dz}) {
            const auto* const chunk = *reinterpret_cast<const std::uint8_t* const*>(begin + static_cast<std::uintptr_t>(index) * 16);
            if (chunk != nullptr && chunkPositionIs(chunk, cx, cz)) {
                return const_cast<std::uint8_t*>(chunk);
            }
        }
        return nullptr;
    } __except (accessFilter(GetExceptionCode())) {
        return nullptr;
    }
}

void* chunkAt(void* region, int x, int , int z)
{
    return gridChunkAt(region, x, z);
}

void* subChunkAt(void* region, int x, int y, int z)
{
    auto* const chunk = static_cast<std::uint8_t*>(gridChunkAt(region, x, z));
    if (chunk == nullptr) {
        return nullptr;
    }
    __try {
        const std::int32_t minY = *reinterpret_cast<const std::int32_t*>(chunk + 0x60 + 4);
        const std::int32_t maxY = *reinterpret_cast<const std::int32_t*>(chunk + 0x70);
        if (y < minY || y > maxY) {
            return nullptr;
        }
        auto* const table = *reinterpret_cast<std::uint8_t**>(chunk + 0x140);
        auto* const tableEnd = *reinterpret_cast<std::uint8_t**>(chunk + 0x148);
        const std::ptrdiff_t index = (y >> 4) - (minY >> 4);
        if (table == nullptr || tableEnd < table || index < 0 || index >= (tableEnd - table) / 0x68) {
            return nullptr;
        }
        return table + index * 0x68;
    } __except (accessFilter(GetExceptionCode())) {
        return nullptr;
    }
}

const void* blockAt(void* region, int x, int y, int z, int layer = 0)
{
    void* const sub = subChunkAt(region, x, y, z);
    void* storage = nullptr;
    if (sub == nullptr || !readPointer(static_cast<std::uint8_t*>(sub) + 0x20 + layer * 8, storage)
        || !looksLikeGameObject(storage)) {
        return nullptr;
    }
    const unsigned int index = ((x & 15) << 8) | ((z & 15) << 4) | (y & 15);
    return storageGet(storage, index);
}

Sample g_sample{};
std::atomic<std::uint64_t> g_generation{0};

void publish(const Sample& in)
{

    g_generation.fetch_add(1, std::memory_order_acq_rel);
    g_sample = in;
    g_generation.fetch_add(1, std::memory_order_acq_rel);
}

constexpr std::ptrdiff_t kBlockSourceLevel = 0x20;
constexpr std::ptrdiff_t kBlockSourceChunkSource = 0x28;
constexpr std::ptrdiff_t kLevelData = 0x90;
constexpr std::ptrdiff_t kLevelDataTime = 0x340;
constexpr std::ptrdiff_t kLevelDataSimulationDistance = 0x368;
constexpr std::ptrdiff_t kLevelDataDifficulty = 0x3b8;
std::atomic<std::int32_t> g_regionalDifficultySlot{0};
std::atomic<const void*> g_regionalDifficultyTail{nullptr};
constexpr std::ptrdiff_t kSubChunkSkyLight = 0x08;
constexpr std::ptrdiff_t kSubChunkBlockLight = 0x10;

constexpr std::ptrdiff_t kLevelChunkMinY = 0x60 + 4;
constexpr std::ptrdiff_t kLevelChunkMaxY = 0x70;

constexpr std::ptrdiff_t kChunkSourceSlotCount = 0x164;
constexpr std::ptrdiff_t kChunkSourceGridBegin = 0x170;
constexpr std::ptrdiff_t kChunkSourceGridEnd = 0x178;
constexpr std::ptrdiff_t kGridEntryBytes = 16;
constexpr int kMaxGridSlots = 4096;

constexpr std::size_t kHitKind = 0x18;
constexpr std::size_t kHitBlockPos = 0x20;

constexpr std::int32_t kHitKindBlock = 0;

static_assert(kTextBytes == static_cast<int>(blocks::kStateTextBytes));
static_assert(std::is_trivially_copyable_v<Sample>);

void collectSpeed(Sample& out)
{
    void* const player = GameData::instance().player();
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    if (player == nullptr || !GameData::rawPosOf(player, x, y, z)
        || !GameData::previousPosOf(player, px, py, pz)) {
        return;
    }
    out.hasSpeed = true;
    out.dx = x - px;
    out.dy = y - py;
    out.dz = z - pz;
}

void collectChunkGrid(void* region, Sample& out)
{
    void* source = nullptr;
    if (!readPointer(static_cast<std::uint8_t*>(region) + kBlockSourceChunkSource, source)
        || !looksLikeGameObject(source)) {
        return;
    }
    auto* const bytes = static_cast<std::uint8_t*>(source);
    std::int32_t slots = 0;
    if (!readInt32(bytes + kChunkSourceSlotCount, slots) || slots <= 0 || slots > kMaxGridSlots) {
        return;
    }
    void* begin = nullptr;
    void* end = nullptr;
    if (!readPointer(bytes + kChunkSourceGridBegin, begin)
        || !readPointer(bytes + kChunkSourceGridEnd, end) || !memory::plausiblePointer(begin)
        || !memory::plausiblePointer(end)) {
        return;
    }
    const std::ptrdiff_t span = static_cast<std::uint8_t*>(end) - static_cast<std::uint8_t*>(begin);
    if (span <= 0 || span % kGridEntryBytes != 0 || span / kGridEntryBytes != slots) {
        return;
    }
    int live = 0;
    for (int i = 0; i < slots; ++i) {
        void* chunk = nullptr;
        if (readPointer(static_cast<std::uint8_t*>(begin) + i * kGridEntryBytes, chunk)
            && memory::plausiblePointer(chunk)) {
            ++live;
        }
    }
    out.hasChunkGrid = true;
    out.chunkSlots = slots;
    out.chunkLive = live;
}

void collectTarget(void* region, bool wantTags, Sample& out)
{
    unsigned char raw[hooks::kHitResultBytes] = {};
    if (!hooks::readHitResult(raw)) {
        return;
    }
    std::int32_t cell[3] = {0, 0, 0};
    std::int32_t kind = 0;
    std::memcpy(cell, raw + kHitBlockPos, sizeof(cell));
    std::memcpy(&kind, raw + kHitKind, sizeof(kind));

    if (kind != kHitKindBlock) {
        return;
    }
    const int pos[3] = {cell[0], cell[1], cell[2]};
    const void* const block = blockAt(region, pos[0], pos[1], pos[2]);
    char name[kTextBytes] = {};
    if (block == nullptr || !blocks::nameOfBlock(block, name, sizeof(name))) {
        return;
    }
    out.hasTarget = true;
    out.targetX = cell[0];
    out.targetY = cell[1];
    out.targetZ = cell[2];
    std::snprintf(out.targetName, sizeof(out.targetName), "%s", name);
    char states[kMaxStates][blocks::kStateTextBytes] = {};
    const int rows = blocks::statesOfBlock(block, states, kMaxStates);
    out.targetStateCount = rows;
    for (int i = 0; i < rows && i < kMaxStates; ++i) {
        std::snprintf(out.targetStates[i], sizeof(out.targetStates[i]), "%s", states[i]);
    }
    if (wantTags) {
        char tags[kMaxTags][blocks::kStateTextBytes] = {};
        const int count = blocks::tagsOfBlock(block, tags, kMaxTags);
        out.targetTagCount = count;
        for (int i = 0; i < count && i < kMaxTags; ++i) {
            std::snprintf(out.targetTags[i], sizeof(out.targetTags[i]), "%s", tags[i]);
        }
    }
}

constexpr std::ptrdiff_t kActorRegistry = 0x10;
constexpr std::ptrdiff_t kActorEntityId = 0x18;
constexpr std::ptrdiff_t kRegistryPoolsBegin = 0x68;
constexpr std::ptrdiff_t kRegistryPoolsEnd = 0x70;
constexpr std::ptrdiff_t kPoolEntryBytes = 0x20;
constexpr std::ptrdiff_t kPoolEntryType = 0x08;
constexpr std::ptrdiff_t kPoolEntryStore = 0x10;
constexpr std::ptrdiff_t kStoreSparseBegin = 0x08;
constexpr std::ptrdiff_t kStoreSparseEnd = 0x10;
constexpr std::ptrdiff_t kStorePackedBegin = 0x20;
constexpr std::ptrdiff_t kStorePackedEnd = 0x28;
constexpr std::ptrdiff_t kStorePayload = 0x50;
constexpr std::uint32_t kEntityIndexMask = 0x3ffffu;
constexpr std::uint32_t kEntityVersionMask = 0xfffc0000u;

constexpr std::uint32_t kActorOwnerTypeId = 0x85B93800u;

constexpr std::uint32_t kAabbTypeId = 0xBAC1B3CFu;
constexpr std::size_t kAabbStride = 0x20;
constexpr int kMaxEntities = 4096;

constexpr std::uint32_t kActorDefinitionTypeId = 0xDEB6534Fu;
constexpr std::size_t kActorDefinitionStride = 0xb0;
constexpr std::ptrdiff_t kActorDefinitionName = 0x88;

bool readGameString(const void* at, char* out, std::size_t cap);

bool findStore(std::uintptr_t registry, std::uint32_t typeId, std::uintptr_t& store)
{
    __try {
        const auto first = *reinterpret_cast<const std::uintptr_t*>(registry + kRegistryPoolsBegin);
        const auto last = *reinterpret_cast<const std::uintptr_t*>(registry + kRegistryPoolsEnd);
        if (first == 0 || last <= first || (last - first) % kPoolEntryBytes != 0) {
            return false;
        }
        const std::uintptr_t entries = (last - first) / kPoolEntryBytes;
        for (std::uintptr_t i = 0; i < entries && i < 4096; ++i) {
            const std::uintptr_t entry = first + i * kPoolEntryBytes;
            if (*reinterpret_cast<const std::uint32_t*>(entry + kPoolEntryType) == typeId) {
                store = *reinterpret_cast<const std::uintptr_t*>(entry + kPoolEntryStore);
                return store != 0;
            }
        }
        return false;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

bool packedIndexOf(std::uintptr_t store, std::uint32_t id, std::uint32_t& position)
{
    __try {
        const std::uint32_t low = id & kEntityIndexMask;
        const std::uint32_t page = low >> 11;
        const auto pagesBegin = *reinterpret_cast<const std::uintptr_t*>(store + kStoreSparseBegin);
        const auto pagesEnd = *reinterpret_cast<const std::uintptr_t*>(store + kStoreSparseEnd);
        if (pagesBegin == 0 || pagesEnd <= pagesBegin || page >= (pagesEnd - pagesBegin) / 8) {
            return false;
        }
        const auto sparse = *reinterpret_cast<const std::uintptr_t*>(pagesBegin + page * 8);
        if (sparse == 0) {
            return false;
        }
        const std::uint32_t value = *reinterpret_cast<const std::uint32_t*>(sparse + (low & 0x7ffu) * 4);
        if (((id & kEntityVersionMask) ^ value) > 0x3fffeu) {
            return false;
        }
        position = value & kEntityIndexMask;
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

bool componentAt(std::uintptr_t store, std::uint32_t id, std::size_t stride, std::uintptr_t& at)
{
    std::uint32_t position = 0;
    if (!packedIndexOf(store, id, position)) {
        return false;
    }
    __try {
        const auto table = *reinterpret_cast<const std::uintptr_t*>(store + kStorePayload);
        if (table == 0) {
            return false;
        }
        const auto dense = *reinterpret_cast<const std::uintptr_t*>(table + (position >> 7) * 8);
        if (dense == 0) {
            return false;
        }
        at = dense + (position & 0x7fu) * stride;
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

bool readPacked(std::uintptr_t store, std::uint32_t* out, int cap, int& count)
{
    __try {
        const auto begin = *reinterpret_cast<const std::uintptr_t*>(store + kStorePackedBegin);
        const auto end = *reinterpret_cast<const std::uintptr_t*>(store + kStorePackedEnd);
        if (begin == 0 || end < begin || (end - begin) % 4 != 0) {
            return false;
        }
        const std::uintptr_t n = (end - begin) / 4;
        if (n > static_cast<std::uintptr_t>(cap)) {
            return false;
        }
        for (std::uintptr_t i = 0; i < n; ++i) {
            out[i] = reinterpret_cast<const std::uint32_t*>(begin)[i];
        }
        count = static_cast<int>(n);
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

bool readAabbCenter(std::uintptr_t at, float center[3])
{
    __try {
        const float* const box = reinterpret_cast<const float*>(at);
        for (int axis = 0; axis < 3; ++axis) {
            const float low = box[axis];
            const float high = box[axis + 3];
            if (!(low == low) || !(high == high) || low > high || high - low > 64.0f
                || low < -3.0e7f || high > 3.0e7f) {
                return false;
            }
            center[axis] = (low + high) * 0.5f;
        }
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

bool readHitBox(std::uintptr_t at, std::uint32_t id, debuglines::HitBox& out)
{
    __try {
        const float* const box = reinterpret_cast<const float*>(at);
        out.id = id;
        for (int axis = 0; axis < 3; ++axis) {
            const float low = box[axis];
            const float high = box[axis + 3];
            if (!std::isfinite(low) || !std::isfinite(high) || low > high
                || high - low > 64.0F || low < -3.0e7F || high > 3.0e7F) return false;
            out.min[axis] = low;
            out.max[axis] = high;
        }
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

void collectEntities(Sample& out)
{
    void* const player = GameData::instance().player();
    void* registryPointer = nullptr;
    std::int32_t selfId = 0;
    if (player == nullptr
        || !readPointer(static_cast<std::uint8_t*>(player) + kActorRegistry, registryPointer)
        || !readInt32(static_cast<std::uint8_t*>(player) + kActorEntityId, selfId)
        || !memory::plausiblePointer(registryPointer)) {
        return;
    }
    const auto registry = reinterpret_cast<std::uintptr_t>(registryPointer);
    std::uintptr_t owners = 0;
    if (!findStore(registry, kActorOwnerTypeId, owners)) {
        return;
    }
    static std::uint32_t packed[kMaxEntities];
    int count = 0;
    if (!readPacked(owners, packed, kMaxEntities, count) || count <= 0) {
        return;
    }
    std::uintptr_t boxes = 0;
    const bool haveBoxes = findStore(registry, kAabbTypeId, boxes);
    std::unordered_set<std::uint64_t> sections;
    int valid = 0;
    bool sawSelf = false;
    for (int i = 0; i < count; ++i) {
        std::uint32_t position = 0;
        if (!packedIndexOf(owners, packed[i], position) || position != static_cast<std::uint32_t>(i)) {
            continue;
        }
        ++valid;
        sawSelf = sawSelf || packed[i] == static_cast<std::uint32_t>(selfId);
        std::uintptr_t box = 0;
        float center[3] = {};
        if (haveBoxes && componentAt(boxes, packed[i], kAabbStride, box) && readAabbCenter(box, center)) {
            const auto cell = [](float v) {
                return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(v / 16.0f)) & 0x1fffff);
            };
            sections.insert(cell(center[0]) | (cell(center[1]) << 21) | (cell(center[2]) << 42));
        }
    }
    if (!sawSelf) {
        return;
    }
    out.hasEntities = true;
    out.entityCount = valid;
    out.entitySections = static_cast<int>(sections.size());
}

bool readGameString(const void* at, char* out, std::size_t cap)
{
    __try {
        const auto* const bytes = static_cast<const std::uint8_t*>(at);
        const std::uint64_t size = *reinterpret_cast<const std::uint64_t*>(bytes + 0x10);
        const std::uint64_t capacity = *reinterpret_cast<const std::uint64_t*>(bytes + 0x18);
        if (size == 0 || size >= cap || capacity < size || capacity > 0x100000) {
            return false;
        }
        const char* const text =
            capacity <= 15 ? reinterpret_cast<const char*>(bytes) : *reinterpret_cast<const char* const*>(bytes);
        for (std::uint64_t i = 0; i < size; ++i) {
            const auto c = static_cast<unsigned char>(text[i]);
            if (c < 0x20 || c >= 0x7f) {
                return false;
            }
            out[i] = static_cast<char>(c);
        }
        out[size] = '\0';
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

using StorageGetFn = const void*(__fastcall*)(void* storage, unsigned int index);

const void* storageGet(void* storage, unsigned int index)
{
    __try {
        void** const vtable = *reinterpret_cast<void***>(storage);
        const auto get = reinterpret_cast<StorageGetFn>(vtable[3]);
        if (!memory::inGameModule(reinterpret_cast<const void*>(get))) {
            return nullptr;
        }
        return get(storage, index);
    } __except (accessFilter(GetExceptionCode())) {
        return nullptr;
    }
}

constexpr std::ptrdiff_t kHitEntityId = 0x48;
constexpr std::int32_t kHitKindEntity = 1;

constexpr std::uint32_t kActorUniqueIdTypeId = 0x18F957AFu;
constexpr std::size_t kActorUniqueIdStride = 8;
constexpr std::size_t kActorOwnerStride = 8;
constexpr std::ptrdiff_t kActorDefinition = 0x98;
constexpr std::ptrdiff_t kDefinitionPartsBegin = 0x1f0;
constexpr std::ptrdiff_t kDefinitionPartsEnd = 0x1f8;
constexpr std::ptrdiff_t kPartName = 0x08;
constexpr std::ptrdiff_t kFamilySetHead = 0x28;
constexpr std::ptrdiff_t kFamilySetSize = 0x30;
constexpr int kMaxDefinitionParts = 1024;
constexpr int kMaxFamilies = 64;

bool readInt64At(std::uintptr_t at, std::int64_t& out)
{
    return readInt64(reinterpret_cast<const void*>(at), out);
}

int readFamilySet(const std::uint8_t* head, std::uint64_t size, char out[][kTextBytes], int maxRows)
{
    __try {
        if (head == nullptr || head[0x19] != 1 || size == 0 || size > kMaxFamilies) {
            return 0;
        }
        const std::uint8_t* stack[kMaxFamilies];
        int top = 0;
        int rows = 0;
        int visited = 0;
        const std::uint8_t* node = *reinterpret_cast<const std::uint8_t* const*>(head + 0x08);
        while ((node != head || top > 0) && rows < maxRows && visited < kMaxFamilies * 2) {
            if (node != head && node != nullptr && node[0x19] == 0) {
                if (top >= kMaxFamilies) {
                    return 0;
                }
                stack[top++] = node;
                node = *reinterpret_cast<const std::uint8_t* const*>(node);
                continue;
            }
            if (top == 0) {
                break;
            }
            node = stack[--top];
            ++visited;
            char name[kTextBytes] = {};
            if (!readGameString(node + 0x28, name, sizeof(name))) {
                return 0;
            }
            std::snprintf(out[rows], kTextBytes, std::strchr(name, ':') == nullptr ? "#minecraft:%s" : "#%s", name);
            ++rows;
            node = *reinterpret_cast<const std::uint8_t* const*>(node + 0x10);
        }
        return rows;
    } __except (accessFilter(GetExceptionCode())) {
        return 0;
    }
}

bool findFamilyPart(const std::uint8_t* definition, const std::uint8_t*& head, std::uint64_t& size)
{
    void* begin = nullptr;
    void* end = nullptr;
    if (!readPointer(definition + kDefinitionPartsBegin, begin) || !readPointer(definition + kDefinitionPartsEnd, end)
        || !memory::plausiblePointer(begin) || end < begin
        || (static_cast<std::uint8_t*>(end) - static_cast<std::uint8_t*>(begin)) / 8 > kMaxDefinitionParts) {
        return false;
    }
    for (auto* at = static_cast<std::uint8_t*>(begin); at < static_cast<std::uint8_t*>(end); at += 8) {
        void* part = nullptr;
        char name[kTextBytes] = {};
        if (!readPointer(at, part) || !looksLikeGameObject(part)
            || !readGameString(static_cast<std::uint8_t*>(part) + kPartName, name, sizeof(name))
            || std::strcmp(name, "minecraft:type_family") != 0) {
            continue;
        }
        void* set = nullptr;
        std::int64_t count = 0;
        if (!readPointer(static_cast<std::uint8_t*>(part) + kFamilySetHead, set)
            || !readInt64(static_cast<std::uint8_t*>(part) + kFamilySetSize, count) || !memory::plausiblePointer(set)) {
            return false;
        }
        head = static_cast<const std::uint8_t*>(set);
        size = static_cast<std::uint64_t>(count);
        return true;
    }
    return false;
}

void* serverActorWithUniqueId(std::int64_t uniqueId)
{
    void* const serverPlayer = GameData::instance().playerAlt();
    void* registryPointer = nullptr;
    if (serverPlayer == nullptr
        || !readPointer(static_cast<std::uint8_t*>(serverPlayer) + kActorRegistry, registryPointer)
        || !memory::plausiblePointer(registryPointer)) {
        return nullptr;
    }
    const auto registry = reinterpret_cast<std::uintptr_t>(registryPointer);
    std::uintptr_t uniques = 0;
    std::uintptr_t owners = 0;
    if (!findStore(registry, kActorUniqueIdTypeId, uniques) || !findStore(registry, kActorOwnerTypeId, owners)) {
        return nullptr;
    }
    static std::uint32_t packed[kMaxEntities];
    int count = 0;
    if (!readPacked(uniques, packed, kMaxEntities, count)) {
        return nullptr;
    }
    for (int i = 0; i < count; ++i) {
        std::uint32_t position = 0;
        std::uintptr_t at = 0;
        std::int64_t value = 0;
        if (!packedIndexOf(uniques, packed[i], position) || position != static_cast<std::uint32_t>(i)
            || !componentAt(uniques, packed[i], kActorUniqueIdStride, at) || !readInt64At(at, value)
            || value != uniqueId) {
            continue;
        }
        std::uintptr_t owner = 0;
        void* actor = nullptr;
        if (componentAt(owners, packed[i], kActorOwnerStride, owner)
            && readPointer(reinterpret_cast<const void*>(owner), actor) && looksLikeGameObject(actor)) {
            return actor;
        }
        return nullptr;
    }
    return nullptr;
}

void collectEntityTags(std::uintptr_t clientRegistry, std::uint32_t clientId, Sample& out)
{
    std::uintptr_t uniques = 0;
    std::uintptr_t at = 0;
    std::int64_t uniqueId = 0;
    if (!findStore(clientRegistry, kActorUniqueIdTypeId, uniques)
        || !componentAt(uniques, clientId, kActorUniqueIdStride, at) || !readInt64At(at, uniqueId)) {
        return;
    }
    void* const actor = serverActorWithUniqueId(uniqueId);
    void* definition = nullptr;
    if (actor == nullptr || !readPointer(static_cast<std::uint8_t*>(actor) + kActorDefinition, definition)
        || !memory::plausiblePointer(definition)) {
        return;
    }
    const std::uint8_t* head = nullptr;
    std::uint64_t size = 0;
    if (!findFamilyPart(static_cast<const std::uint8_t*>(definition), head, size)) {
        return;
    }
    char rows[kMaxTags][kTextBytes] = {};
    const int count = readFamilySet(head, size, rows, kMaxTags);
    if (count <= 0) {
        return;
    }
    out.entityTagCount = count;
    for (int i = 0; i < count && i < kMaxTags; ++i) {
        std::snprintf(out.entityTags[i], sizeof(out.entityTags[i]), "%s", rows[i]);
    }
}

void collectEntityTarget(bool wantTags, Sample& out)
{
    unsigned char raw[hooks::kHitResultBytes] = {};
    if (!hooks::readHitResult(raw)) {
        return;
    }
    std::int32_t kind = 0;
    std::uint32_t id = 0;
    std::memcpy(&kind, raw + kHitKind, sizeof(kind));
    std::memcpy(&id, raw + kHitEntityId, sizeof(id));
    if (kind != kHitKindEntity || id == 0xffffffffu) {
        return;
    }
    void* const player = GameData::instance().player();
    void* registryPointer = nullptr;
    if (player == nullptr || !readPointer(static_cast<std::uint8_t*>(player) + kActorRegistry, registryPointer)
        || !memory::plausiblePointer(registryPointer)) {
        return;
    }
    std::uintptr_t store = 0;
    std::uintptr_t at = 0;
    char name[kTextBytes] = {};
    if (!findStore(reinterpret_cast<std::uintptr_t>(registryPointer), kActorDefinitionTypeId, store)
        || !componentAt(store, id, kActorDefinitionStride, at)
        || !readGameString(reinterpret_cast<const void*>(at + kActorDefinitionName), name, sizeof(name))
        || std::strchr(name, ':') == nullptr) {
        return;
    }
    out.hasEntityTarget = true;
    std::snprintf(out.entityName, sizeof(out.entityName), "%s", name);

    float box[GameData::kAabbShapeStride / sizeof(float)]{};
    std::uintptr_t shapes = 0;
    std::uintptr_t shape = 0;
    if (findStore(reinterpret_cast<std::uintptr_t>(registryPointer), GameData::kAabbShapeTypeId, shapes)
        && componentAt(shapes, id, GameData::kAabbShapeStride, shape)
        && memory::copyGuarded(reinterpret_cast<const void*>(shape), box, sizeof(box))) {
        const float width = box[3] - box[0];
        const float height = box[4] - box[1];
        const float depth = box[5] - box[2];
        constexpr float kTolerance = 0.01f;
        bool finite = true;
        for (const float value : box) finite = finite && std::isfinite(value) && std::fabs(value) < 3.0e7f;

        if (finite && width > 0.0f && height > 0.0f && std::fabs(width - depth) <= kTolerance
            && std::fabs(width - box[6]) <= kTolerance && std::fabs(height - box[7]) <= kTolerance) {
            out.hasEntityHit = true;
            out.entityHitX = (box[0] + box[3]) * 0.5f;
            out.entityHitY = box[1];
            out.entityHitZ = (box[2] + box[5]) * 0.5f;
        }
    }
    if (!out.hasEntityHit) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            log().warn(L"DebugKeys: the targeted entity's bounding box could not be read (no /summon copy)");
        }
    }
    if (wantTags) {
        collectEntityTags(reinterpret_cast<std::uintptr_t>(registryPointer), id, out);
    }
}

constexpr std::size_t kHitLiquid = 0x68;
constexpr std::size_t kHitLiquidPos = 0x6c;

bool isLiquid(const void* block)
{
    bool motion = false;
    bool liquid = false;
    return block != nullptr && blocks::materialFlags(block, motion, liquid) && liquid;
}

void collectFluid(void* region, Sample& out)
{
    unsigned char raw[hooks::kHitResultBytes] = {};
    if (!hooks::readLiquidHitResult(raw)) {
        return;
    }
    if (raw[kHitLiquid] != 1) {

        std::int32_t kind = 0;
        std::memcpy(&kind, raw + kHitKind, sizeof(kind));
        if (kind != kHitKindBlock) {
            return;
        }
        std::int32_t cell[3] = {0, 0, 0};
        std::memcpy(cell, raw + kHitBlockPos, sizeof(cell));
        out.hasFluid = true;
        out.fluidX = cell[0];
        out.fluidY = cell[1];
        out.fluidZ = cell[2];
        std::snprintf(out.fluidName, sizeof(out.fluidName), "%s", "minecraft:empty");
        return;
    }
    std::int32_t cell[3] = {0, 0, 0};
    std::memcpy(cell, raw + kHitLiquidPos, sizeof(cell));

    const int pos[3] = {cell[0], cell[1], cell[2]};
    const void* block = blockAt(region, pos[0], pos[1], pos[2], 0);
    if (!isLiquid(block)) {
        const void* const extra = blockAt(region, pos[0], pos[1], pos[2], 1);
        block = isLiquid(extra) ? extra : nullptr;
    }
    char name[kTextBytes] = {};
    if (block == nullptr || !blocks::nameOfBlock(block, name, sizeof(name))) {
        return;
    }
    out.hasFluid = true;
    out.fluidX = cell[0];
    out.fluidY = cell[1];
    out.fluidZ = cell[2];
    std::snprintf(out.fluidName, sizeof(out.fluidName), "%s", name);
    char lines[kMaxFluidStates][blocks::kStateTextBytes] = {};
    const int states = blocks::statesOfBlock(block, lines, kMaxFluidStates);
    out.fluidStateCount = states;
    for (int i = 0; i < states && i < kMaxFluidStates; ++i) {
        std::snprintf(out.fluidStates[i], sizeof(out.fluidStates[i]), "%s", lines[i]);
    }
    char tagsText[kMaxTags][blocks::kStateTextBytes] = {};
    const int tags = blocks::tagsOfBlock(block, tagsText, kMaxTags);
    out.fluidTagCount = tags;
    for (int i = 0; i < tags && i < kMaxTags; ++i) {
        std::snprintf(out.fluidTags[i], sizeof(out.fluidTags[i]), "%s", tagsText[i]);
    }
}

constexpr std::ptrdiff_t kLevelChunkBiomesBegin = 0x358;
constexpr std::ptrdiff_t kLevelChunkBiomesEnd = 0x360;
constexpr std::ptrdiff_t kBiomeName = 0x198;

void collectBiome(void* region, int x, int y, int z, Sample& out)
{
    void* const chunk = chunkAt(region, x, y, z);
    std::int32_t minY = 0;
    void* begin = nullptr;
    void* end = nullptr;
    if (!memory::plausiblePointer(chunk)
        || !readInt32(static_cast<std::uint8_t*>(chunk) + kLevelChunkMinY, minY)
        || !readPointer(static_cast<std::uint8_t*>(chunk) + kLevelChunkBiomesBegin, begin)
        || !readPointer(static_cast<std::uint8_t*>(chunk) + kLevelChunkBiomesEnd, end)
        || !memory::plausiblePointer(begin) || end <= begin) {
        return;
    }
    const std::ptrdiff_t count = (static_cast<std::uint8_t*>(end) - static_cast<std::uint8_t*>(begin)) / 8;
    std::ptrdiff_t section = (y >> 4) - (minY >> 4);
    if (count <= 0 || count > 64 || section < 0) {
        return;
    }
    if (section >= count) {
        section = count - 1;
    }
    void* storage = nullptr;
    for (; section >= 0; --section) {
        if (readPointer(static_cast<std::uint8_t*>(begin) + section * 8, storage) && storage != nullptr) {
            break;
        }
    }
    if (storage == nullptr || !looksLikeGameObject(storage)) {
        return;
    }
    const unsigned int index = ((x & 15) << 8) | ((z & 15) << 4) | (y & 15);
    void* const biome = const_cast<void*>(storageGet(storage, index));
    char name[kTextBytes] = {};
    if (!looksLikeGameObject(biome)
        || !readGameString(static_cast<const std::uint8_t*>(biome) + kBiomeName, name, sizeof(name))
        || std::strchr(name, ':') == nullptr) {
        return;
    }
    out.hasBiome = true;
    std::snprintf(out.biome, sizeof(out.biome), "%s", name);
}

constexpr std::ptrdiff_t kLevelChunkSubChunks = 0x140;
constexpr std::ptrdiff_t kSubChunkBytes = 0x68;
constexpr std::ptrdiff_t kSubChunkStorage = 0x20;
constexpr unsigned char kUniformGet[] = {0x48, 0x8B, 0x41, 0x08, 0xC3};

bool countChunkSections(const std::uint8_t* chunk, const void* air, int& total, int& nonEmpty)
{
    __try {
        const std::int32_t minY = *reinterpret_cast<const std::int32_t*>(chunk + kLevelChunkMinY);
        const std::int32_t maxY = *reinterpret_cast<const std::int32_t*>(chunk + kLevelChunkMaxY);
        const int n = (maxY - minY + 1) / 16;
        const std::uint8_t* const table = *reinterpret_cast<const std::uint8_t* const*>(chunk + kLevelChunkSubChunks);
        if (n <= 0 || n > 64 || table == nullptr) {
            return false;
        }
        for (int i = 0; i < n; ++i) {
            ++total;
            const std::uint8_t* const storage =
                *reinterpret_cast<const std::uint8_t* const*>(table + i * kSubChunkBytes + kSubChunkStorage);
            if (storage == nullptr) {
                continue;
            }
            const unsigned char* const* const vtable = *reinterpret_cast<const unsigned char* const* const*>(storage);
            const unsigned char* const get = vtable != nullptr ? vtable[3] : nullptr;
            const bool uniform = get != nullptr && std::memcmp(get, kUniformGet, sizeof(kUniformGet)) == 0;
            if (uniform && *reinterpret_cast<const void* const*>(storage + 8) == air) {
                continue;
            }
            ++nonEmpty;
        }
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

const void* g_airBlock = nullptr;

const void* airBlock()
{
    const void*& air = g_airBlock;
    if (air == nullptr) {
        blocks::Table table;
        if (blocks::resolve({"minecraft:air"}, table)) {
            const auto found = table.find("minecraft:air");
            air = found != table.end() ? found->second : nullptr;
        }
    }
    return air;
}

void collectSections(void* region, Sample& out)
{
    const void* const air = airBlock();
    void* source = nullptr;
    void* begin = nullptr;
    void* end = nullptr;
    if (air == nullptr
        || !readPointer(static_cast<std::uint8_t*>(region) + kBlockSourceChunkSource, source)
        || !looksLikeGameObject(source)
        || !readPointer(static_cast<std::uint8_t*>(source) + kChunkSourceGridBegin, begin)
        || !readPointer(static_cast<std::uint8_t*>(source) + kChunkSourceGridEnd, end)
        || !memory::plausiblePointer(begin) || end <= begin) {
        return;
    }
    const std::ptrdiff_t slots = (static_cast<std::uint8_t*>(end) - static_cast<std::uint8_t*>(begin)) / kGridEntryBytes;
    if (slots <= 0 || slots > kMaxGridSlots) {
        return;
    }
    int total = 0;
    int nonEmpty = 0;
    for (std::ptrdiff_t i = 0; i < slots; ++i) {
        void* chunk = nullptr;
        if (!readPointer(static_cast<std::uint8_t*>(begin) + i * kGridEntryBytes, chunk)
            || !memory::plausiblePointer(chunk)) {
            continue;
        }
        if (!countChunkSections(static_cast<const std::uint8_t*>(chunk), air, total, nonEmpty)) {
            return;
        }
    }
    out.hasSections = true;
    out.sectionsNonEmpty = nonEmpty;
    out.sectionsTotal = total;
}

constexpr std::ptrdiff_t kActorDimension = 0x1c8;
constexpr std::ptrdiff_t kDimensionBlockSource = 0xf0;
constexpr std::ptrdiff_t kServerChunkMapHead = 0x78;
constexpr std::ptrdiff_t kServerChunkMapSize = 0x80;
constexpr std::ptrdiff_t kChunkMapNodeKey = 0x10;
constexpr std::ptrdiff_t kChunkMapNodeChunk = 0x18;

constexpr std::ptrdiff_t kLevelChunkLoading = 0x10;
constexpr std::uint64_t kMaxServerChunks = 16384;

struct ServerView {
    void* region = nullptr;
    void* chunkSource = nullptr;
    void* level = nullptr;
    void* dimension = nullptr;
};

void noteServerMiss(int reason, void* a, void* b)
{
    static std::atomic<std::uint32_t> told{0};
    const std::uint32_t bit = 1u << (reason & 31);
    if ((told.fetch_or(bit) & bit) == 0) {
        log().info(L"DebugScreen: the server-side view is not available (reason {}: {} / {})", reason, a, b);
    }
}

bool tickingManagerOffset(void* level, std::int32_t& disp)
{
    __try {
        void** const vtable = *reinterpret_cast<void***>(level);
        const auto* const getter = static_cast<const std::uint8_t*>(vtable[0x5c0 / 8]);
        if (!memory::inGameModule(getter) || getter[0] != 0x48 || getter[1] != 0x8B || getter[2] != 0x81
            || getter[7] != 0xC3) {
            return false;
        }
        std::memcpy(&disp, getter + 3, sizeof(disp));
        return disp > 0 && disp < 0x10000;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

bool serverView(void* clientRegion, ServerView& out)
{
    void* const serverPlayer = GameData::instance().playerAlt();
    void* dimension = nullptr;
    void* region = nullptr;
    void* level = nullptr;
    void* clientLevel = nullptr;
    void* source = nullptr;
    void* serverVtable = nullptr;
    void* clientVtable = nullptr;
    std::int32_t disp = 0;
    if (serverPlayer == nullptr) {
        noteServerMiss(0, nullptr, nullptr);
        return false;
    }
    if (!readPointer(static_cast<std::uint8_t*>(serverPlayer) + kActorDimension, dimension)
        || !memory::plausiblePointer(dimension)
        || !readPointer(static_cast<std::uint8_t*>(dimension) + kDimensionBlockSource, region)
        || !memory::plausiblePointer(region) || !readPointer(region, serverVtable)) {
        noteServerMiss(1, dimension, region);
        return false;
    }
    if (!readPointer(clientRegion, clientVtable) || serverVtable != clientVtable) {
        noteServerMiss(2, serverVtable, clientVtable);
        return false;
    }

    if (!readPointer(static_cast<std::uint8_t*>(region) + kBlockSourceLevel, level)
        || !readPointer(static_cast<std::uint8_t*>(clientRegion) + kBlockSourceLevel, clientLevel)
        || level == clientLevel || !looksLikeGameObject(level) || !tickingManagerOffset(level, disp)) {
        noteServerMiss(3, level, clientLevel);
        return false;
    }
    if (!readPointer(static_cast<std::uint8_t*>(region) + kBlockSourceChunkSource, source)
        || !looksLikeGameObject(source)) {
        noteServerMiss(4, source, nullptr);
        return false;
    }
    out.region = region;
    out.chunkSource = source;
    out.level = level;
    out.dimension = dimension;
    return true;
}

bool callRegionalDifficulty(void* level, std::int32_t slot, int dimensionId, float& out,
                            float& xmm3, bool& captured)
{
    void* vtable = nullptr;
    void* function = nullptr;
    if (!looksLikeGameObject(level) || !readPointer(level, vtable)
        || !memory::isReadable(static_cast<const std::uint8_t*>(vtable) + slot, sizeof(void*))
        || !readPointer(static_cast<const std::uint8_t*>(vtable) + slot, function)
        || !memory::inGameModule(function) || !memory::isExecutable(function, 1)) {
        return false;
    }
    const void* const tail = g_regionalDifficultyTail.load(std::memory_order_acquire);
    const auto fnAddress = reinterpret_cast<std::uintptr_t>(function);
    const auto tailAddress = reinterpret_cast<std::uintptr_t>(tail);

    const bool capture = tail != nullptr && tailAddress >= fnAddress && tailAddress - fnAddress < 0x400;
    __try {
        if (capture) {
            out = tsukuyomiCallCaptureXmm3(function, level, dimensionId, &xmm3);
            captured = true;
        } else {
            out = reinterpret_cast<float(__fastcall*)(void*, int)>(function)(level, dimensionId);
        }
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

void collectLocalDifficulty(void* region, void* clientLevel, int dimensionId, int difficulty,
                            bool localServer, Sample& out)
{
    const std::int32_t slot = g_regionalDifficultySlot.load(std::memory_order_acquire);
    if (slot == 0) return;
    void* level = nullptr;
    ServerView view{};

    if (localServer && serverView(region, view)) level = view.level;
    else level = clientLevel;
    float value = 0.0f;
    float xmm3 = 0.0f;
    bool captured = false;
    if (level != nullptr && callRegionalDifficulty(level, slot, dimensionId, value, xmm3, captured)
        && std::isfinite(value) && value >= 0.0f && value <= 1.0f) {
        out.hasLocalDifficulty = true;
        out.localDifficulty = value;
        float raw = 0.0f;
        if (captured && dbgtext::regionalRaw(difficulty, value, xmm3, raw)) {
            out.hasLocalDifficultyRaw = true;
            out.localDifficultyRaw = raw;
        }
    }
}

bool walkServerChunks(const std::uint8_t* source, int wantX, int wantZ, bool findOne, std::uint64_t& count,
                      const std::uint8_t*& found, int& loading)
{
    __try {
        const auto* const head = *reinterpret_cast<const std::uint8_t* const*>(source + kServerChunkMapHead);
        const std::uint64_t size = *reinterpret_cast<const std::uint64_t*>(source + kServerChunkMapSize);
        if (head == nullptr || size > kMaxServerChunks) {
            return false;
        }
        std::uint64_t walked = 0;
        const std::uint8_t* node = *reinterpret_cast<const std::uint8_t* const*>(head);
        while (node != head) {
            if (node == nullptr || ++walked > size) {
                return false;
            }
            const auto* const chunk = *reinterpret_cast<const std::uint8_t* const*>(node + kChunkMapNodeChunk);
            if (chunk != nullptr && *reinterpret_cast<const std::int32_t*>(chunk + kLevelChunkLoading) == 1) {
                ++loading;
            }
            if (findOne) {
                const auto* const key = reinterpret_cast<const std::int32_t*>(node + kChunkMapNodeKey);
                if (key[0] == wantX && key[1] == wantZ) {
                    found = *reinterpret_cast<const std::uint8_t* const*>(node + kChunkMapNodeChunk);
                }
            }
            node = *reinterpret_cast<const std::uint8_t* const*>(node);
        }
        if (walked != size) {
            return false;
        }
        count = size;
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

void collectServerEntities(int playerChunkX, int playerChunkZ, int simulationDistance, Sample& out)
{
    void* const serverPlayer = GameData::instance().playerAlt();
    void* registryPointer = nullptr;
    std::int32_t selfId = 0;
    if (serverPlayer == nullptr
        || !readPointer(static_cast<std::uint8_t*>(serverPlayer) + kActorRegistry, registryPointer)
        || !readInt32(static_cast<std::uint8_t*>(serverPlayer) + kActorEntityId, selfId)
        || !memory::plausiblePointer(registryPointer)) {
        return;
    }
    const auto registry = reinterpret_cast<std::uintptr_t>(registryPointer);
    std::uintptr_t owners = 0;
    std::uintptr_t boxes = 0;
    if (!findStore(registry, kActorOwnerTypeId, owners) || !findStore(registry, kAabbTypeId, boxes)) {
        return;
    }
    static std::uint32_t packed[kMaxEntities];
    int count = 0;
    if (!readPacked(owners, packed, kMaxEntities, count) || count <= 0) {
        return;
    }
    std::unordered_set<std::uint64_t> sections;
    int valid = 0;
    int visible = 0;
    bool sawSelf = false;
    for (int i = 0; i < count; ++i) {
        std::uint32_t position = 0;
        if (!packedIndexOf(owners, packed[i], position) || position != static_cast<std::uint32_t>(i)) {
            continue;
        }
        ++valid;
        sawSelf = sawSelf || packed[i] == static_cast<std::uint32_t>(selfId);
        std::uintptr_t box = 0;
        float center[3] = {};
        if (componentAt(boxes, packed[i], kAabbStride, box) && readAabbCenter(box, center)) {
            const auto cell = [](float v) { return static_cast<std::int64_t>(std::floor(v / 16.0f)); };
            const std::int64_t cx = cell(center[0]);
            const std::int64_t cy = cell(center[1]);
            const std::int64_t cz = cell(center[2]);
            sections.insert(static_cast<std::uint64_t>(cx & 0x1fffff) | (static_cast<std::uint64_t>(cy & 0x1fffff) << 21)
                            | (static_cast<std::uint64_t>(cz & 0x1fffff) << 42));
            if (std::abs(cx - playerChunkX) <= simulationDistance && std::abs(cz - playerChunkZ) <= simulationDistance) {
                ++visible;
            }
        }
    }
    if (!sawSelf) {
        return;
    }
    out.hasServerEntities = true;
    out.serverEntityCount = valid;
    out.serverVisibleEntities = visible;
    out.serverEntitySections = static_cast<int>(sections.size());
}

constexpr std::ptrdiff_t kDimensionTickingAreas = 0x300;
constexpr std::ptrdiff_t kAreaListBegin = 0x08;
constexpr std::ptrdiff_t kAreaListEnd = 0x10;
constexpr std::ptrdiff_t kAreaEntity = 0x38;
constexpr std::ptrdiff_t kAreaView = 0x240;

bool countForcedChunks(const std::uint8_t* list, const void* viewVtable, int& chunks)
{
    __try {
        const auto begin = *reinterpret_cast<const std::uintptr_t*>(list + kAreaListBegin);
        const auto end = *reinterpret_cast<const std::uintptr_t*>(list + kAreaListEnd);
        if (end < begin || (end - begin) % 16 != 0 || (end - begin) / 16 > 256) {
            return false;
        }
        chunks = 0;
        for (std::uintptr_t at = begin; at < end; at += 16) {
            const auto* const area = *reinterpret_cast<const std::uint8_t* const*>(at);
            if (area == nullptr) {
                continue;
            }
            if (*reinterpret_cast<const std::int64_t*>(area + kAreaEntity) != -1) {
                continue;
            }
            const auto* const view = *reinterpret_cast<const std::uint8_t* const*>(area + kAreaView);
            if (view == nullptr || *reinterpret_cast<const void* const*>(view) != viewVtable) {
                return false;
            }
            const std::int32_t slots = *reinterpret_cast<const std::int32_t*>(view + kChunkSourceSlotCount);
            const auto gridBegin = *reinterpret_cast<const std::uintptr_t*>(view + kChunkSourceGridBegin);
            const auto gridEnd = *reinterpret_cast<const std::uintptr_t*>(view + kChunkSourceGridEnd);
            if (slots < 0 || slots > kMaxGridSlots || gridEnd < gridBegin
                || static_cast<std::int64_t>((gridEnd - gridBegin) / kGridEntryBytes) != slots) {
                return false;
            }
            chunks += slots;
        }
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

void collectForcedChunks(void* clientRegion, void* dimension, Sample& out)
{
    void* list = nullptr;
    void* clientSource = nullptr;
    void* viewVtable = nullptr;
    int chunks = 0;
    if (!readPointer(static_cast<std::uint8_t*>(dimension) + kDimensionTickingAreas, list) || !looksLikeGameObject(list)
        || !readPointer(static_cast<std::uint8_t*>(clientRegion) + kBlockSourceChunkSource, clientSource)
        || !readPointer(clientSource, viewVtable)
        || !countForcedChunks(static_cast<const std::uint8_t*>(list), viewVtable, chunks)) {
        noteServerMiss(6, list, nullptr);
        return;
    }
    out.hasForcedChunks = true;
    out.forcedChunks = chunks;
}

constexpr std::ptrdiff_t kChunkSourceLevel = 0x20;
constexpr std::ptrdiff_t kChunkSourceDimension = 0x28;
constexpr std::ptrdiff_t kChunkSourceParent = 0x30;
constexpr int kMaxParentHops = 8;
constexpr std::ptrdiff_t kSamplerSplineRoot = 0x188;
constexpr std::ptrdiff_t kSplineNodeBytes = 0xa8;
constexpr std::ptrdiff_t kSplineMode = 0x80;
constexpr std::ptrdiff_t kSplineCoordinate = 0x20;
constexpr std::ptrdiff_t kSplineLocations = 0x38;
constexpr std::ptrdiff_t kSplineValues = 0x50;
constexpr std::ptrdiff_t kSplineDerivatives = 0x68;

using ClimateSampleFn = void*(__fastcall*)(void* sampler, std::int64_t* out, const int* blockPos);

struct ClimateTargets {
    ClimateSampleFn sample = nullptr;
    const void* generatorVtable = nullptr;
    std::int32_t biomeSourceOffset = 0;
};
ClimateTargets g_climate{};

void noteClimateMiss(int reason, const void* a)
{
    static std::atomic<std::uint32_t> told{0};
    const std::uint32_t bit = 1u << (reason & 31);
    if ((told.fetch_or(bit) & bit) == 0) {
        log().info(L"DebugScreen: the world generator noise is not available (reason {}: {})", reason, a);
    }
}

bool splineNodeOk(const std::uint8_t* node, int depth, int& budget)
{
    if (depth > 8 || --budget < 0) {
        return false;
    }
    const std::uint8_t mode = node[kSplineMode];
    if (mode == 0) {
        return true;
    }
    if (mode == 1) {
        const auto* const fn = *reinterpret_cast<const std::uint8_t* const*>(node);
        if (!memory::inGameModule(fn) || fn[0] != 0xF3 || fn[1] != 0x0F || fn[2] != 0x10) {
            return false;
        }

        return (fn[3] == 0x01 && fn[4] == 0xC3)
               || (fn[3] == 0x41 && (fn[4] == 0x04 || fn[4] == 0x08 || fn[4] == 0x0C) && fn[5] == 0xC3);
    }
    if (mode != 2) {
        return false;
    }
    const auto* const coordinate = *reinterpret_cast<const std::uint8_t* const*>(node + kSplineCoordinate);
    if (coordinate == nullptr || !splineNodeOk(coordinate, depth + 1, budget)) {
        return false;
    }
    const auto vec = [node](std::ptrdiff_t at, std::uintptr_t& begin, std::uintptr_t& end) {
        begin = *reinterpret_cast<const std::uintptr_t*>(node + at);
        end = *reinterpret_cast<const std::uintptr_t*>(node + at + 8);
        return end >= begin;
    };
    std::uintptr_t lb = 0, le = 0, vb = 0, ve = 0, db = 0, de = 0;
    if (!vec(kSplineLocations, lb, le) || !vec(kSplineValues, vb, ve) || !vec(kSplineDerivatives, db, de)
        || (le - lb) % 4 != 0 || (de - db) % 4 != 0 || (ve - vb) % kSplineNodeBytes != 0
        || (le - lb) / 4 > 64 || (ve - vb) / kSplineNodeBytes != (le - lb) / 4) {
        return false;
    }
    for (std::uintptr_t at = vb; at < ve; at += kSplineNodeBytes) {
        if (!splineNodeOk(reinterpret_cast<const std::uint8_t*>(at), depth + 1, budget)) {
            return false;
        }
    }
    return true;
}

int findSampler(const std::uint8_t* chunkSource, const void* level, const void* dimension, const std::uint8_t*& genOut,
                const std::uint8_t*& samplerOut)
{

    const std::uint8_t* gen = chunkSource;
    for (int hop = 0; hop < kMaxParentHops; ++hop) {
        const auto* const parent = *reinterpret_cast<const std::uint8_t* const*>(gen + kChunkSourceParent);
        if (parent == nullptr) {
            break;
        }
        gen = parent;
    }
    if (*reinterpret_cast<const void* const*>(gen) != g_climate.generatorVtable) {
        return 2;
    }
    if (*reinterpret_cast<const void* const*>(gen + kChunkSourceLevel) != level
        || *reinterpret_cast<const void* const*>(gen + kChunkSourceDimension) != dimension) {
        return 3;
    }

    const auto* const source = *reinterpret_cast<const std::uint8_t* const*>(gen + g_climate.biomeSourceOffset);
    if (source == nullptr) {
        return 4;
    }
    const auto inside = [gen](const std::uint8_t* p) { return p >= gen + 0x100 && p < gen + 0x4000; };
    const auto* const direct = *reinterpret_cast<const std::uint8_t* const*>(source + 8);
    const std::uint8_t* sampler = nullptr;
    if (inside(direct)) {
        sampler = direct;
    } else if (direct != nullptr) {
        const auto* const wrapped = *reinterpret_cast<const std::uint8_t* const*>(direct + 8);
        sampler = inside(wrapped) ? wrapped : nullptr;
    }
    if (sampler == nullptr) {
        return 5;
    }

    int budget = 512;
    if (!splineNodeOk(sampler + kSamplerSplineRoot, 0, budget)) {
        return 6;
    }
    genOut = gen;
    samplerOut = sampler;
    return 0;
}

int sampleClimate(const std::uint8_t* chunkSource, const void* level, const void* dimension, const int pos[3],
                  std::int64_t out[13])
{
    __try {
        const std::uint8_t* gen = nullptr;
        const std::uint8_t* sampler = nullptr;
        if (const int reason = findSampler(chunkSource, level, dimension, gen, sampler); reason != 0) {
            return reason;
        }
        g_climate.sample(const_cast<std::uint8_t*>(sampler), out, pos);
        return 0;
    } __except (accessFilter(GetExceptionCode())) {
        return 7;
    }
}

void collectClimate(const ServerView& view, int feetX, int feetY, int feetZ, Sample& out)
{
    if (g_climate.sample == nullptr || g_climate.generatorVtable == nullptr || g_climate.biomeSourceOffset <= 0) {
        noteClimateMiss(1, nullptr);
        return;
    }
    alignas(8) std::int64_t values[13] = {};
    const int pos[3] = {feetX, feetY, feetZ};
    const int reason = sampleClimate(static_cast<const std::uint8_t*>(view.chunkSource), view.level, view.dimension,
                                     pos, values);
    if (reason != 0) {
        noteClimateMiss(reason, view.chunkSource);
        return;
    }

    for (int i = 0; i < 6; ++i) {
        if (values[i] != values[i + 6] || values[i] < -200000 || values[i] > 200000) {
            noteClimateMiss(8, nullptr);
            return;
        }
    }
    if (values[12] != 0) {
        noteClimateMiss(9, nullptr);
        return;
    }
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) {
        log().info(L"DebugScreen: world generator noise T={} H={} C={} E={} D={} W={} (x10000)", values[0], values[1],
                   values[2], values[3], values[4], values[5]);
    }
    out.hasClimate = true;
    for (int i = 0; i < 6; ++i) {
        out.climate[i] = values[i];
    }
}

constexpr std::ptrdiff_t kGeneratorSubObject = 0x70;
constexpr std::ptrdiff_t kSamplerSplineFactor = 0x230;

constexpr std::ptrdiff_t kDimensionMinY = 0xc8;
constexpr std::ptrdiff_t kDimensionMaxY = 0xca;
constexpr double kGridBudgetMs = 4.0;
constexpr int kMaxVtableSlots = 96;

using ColumnSampleFn = float*(__fastcall*)(void* sampler, float* out3, const std::int32_t* quartPos);
using DensityGridFn = float*(__fastcall*)(void* generator, float* out, const std::int32_t* chunkPos);

struct SurfaceTargets {
    ColumnSampleFn column = nullptr;
    std::int32_t samplerFromSub = 0;
    std::int32_t factoryFromSub = 0;
    std::int32_t noBlendFlag = 0;
    const void* densityGrid = nullptr;
    dbgsurface::SurfaceConstants k{};
    bool ready = false;
};
SurfaceTargets g_surface{};

void noteSurfaceMiss(int reason, const void* a)
{
    static std::atomic<std::uint32_t> told{0};
    const std::uint32_t bit = 1u << (reason & 31);
    if ((told.fetch_or(bit) & bit) == 0) {
        log().info(L"DebugScreen: the preliminary surface level / density is not available (reason {}: {})", reason, a);
    }
}

struct SurfaceProbe {
    const void* gen = nullptr;
    const void* sampler = nullptr;
    const void* factory = nullptr;
    int noBlend = -1;
    int minY = 0;
    int maxY = 0;
    float offset = 0.0f;
    float factor = 0.0f;
    int gridSlot = -1;
};

int sampleSurfaceColumn(const std::uint8_t* chunkSource, const void* level, const void* dimension, int feetX, int feetZ,
                        float out3[3], SurfaceProbe& probe)
{
    __try {
        const std::uint8_t* gen = nullptr;
        const std::uint8_t* sampler = nullptr;
        if (const int reason = findSampler(chunkSource, level, dimension, gen, sampler); reason != 0) {
            return reason;
        }

        if (sampler != gen + kGeneratorSubObject + g_surface.samplerFromSub) {
            return 10;
        }

        int budget = 512;
        if (!splineNodeOk(sampler + kSamplerSplineFactor, 0, budget)) {
            return 11;
        }

        const int minY = *reinterpret_cast<const std::int16_t*>(static_cast<const std::uint8_t*>(dimension) + kDimensionMinY);
        const int maxY = *reinterpret_cast<const std::int16_t*>(static_cast<const std::uint8_t*>(dimension) + kDimensionMaxY);
        if (!(maxY > minY && minY >= -512 && minY <= 0 && maxY > 0 && maxY <= 1024)) {
            return 12;
        }
        const std::int32_t quart[2] = {feetX >> 2, feetZ >> 2};
        g_surface.column(const_cast<std::uint8_t*>(sampler), out3, quart);
        probe.gen = gen;
        probe.sampler = sampler;
        probe.minY = minY;
        probe.maxY = maxY;
        const auto* const factory =
            *reinterpret_cast<const std::uint8_t* const*>(gen + kGeneratorSubObject + g_surface.factoryFromSub);
        probe.factory = factory;
        probe.noBlend = factory != nullptr ? factory[g_surface.noBlendFlag] : -1;
        return 0;
    } __except (accessFilter(GetExceptionCode())) {
        return 13;
    }
}

constexpr std::ptrdiff_t kFactoryListHead = 0x08;
constexpr std::ptrdiff_t kFactoryListSize = 0x10;
constexpr std::ptrdiff_t kFactoryNodeKey = 0x10;
constexpr std::uint64_t kMaxFactoryEntries = 65536;

bool blendTableCovers(const std::uint8_t* factory, int chunkX, int chunkZ)
{
    const auto* const head = *reinterpret_cast<const std::uint8_t* const*>(factory + kFactoryListHead);
    const std::uint64_t size = *reinterpret_cast<const std::uint64_t*>(factory + kFactoryListSize);
    if (head == nullptr || size == 0 || size > kMaxFactoryEntries) {
        return false;
    }
    std::uint32_t found = 0;
    std::uint64_t walked = 0;
    for (const auto* node = *reinterpret_cast<const std::uint8_t* const*>(head); node != head;
         node = *reinterpret_cast<const std::uint8_t* const*>(node)) {
        if (node == nullptr || ++walked > size) {
            return false;
        }
        const auto* const key = reinterpret_cast<const std::int32_t*>(node + kFactoryNodeKey);
        const int dx = key[0] - chunkX;
        const int dz = key[1] - chunkZ;
        if (dx >= -1 && dx <= 1 && dz >= -1 && dz <= 1) {
            found |= 1u << ((dz + 1) * 3 + (dx + 1));
        }
    }
    return walked == size && found == 0x1ffu;
}

int buildDensityGrid(const std::uint8_t* chunkSource, const void* level, const void* dimension, int chunkX, int chunkZ,
                     float* grid, double& elapsedMs, int& slotOut)
{
    __try {
        const std::uint8_t* gen = nullptr;
        const std::uint8_t* sampler = nullptr;
        if (const int reason = findSampler(chunkSource, level, dimension, gen, sampler); reason != 0) {
            return reason;
        }
        const auto* const factory =
            *reinterpret_cast<const std::uint8_t* const*>(gen + kGeneratorSubObject + g_surface.factoryFromSub);
        if (factory == nullptr) {
            return 20;
        }

        if (factory[g_surface.noBlendFlag] != 1 && !blendTableCovers(factory, chunkX, chunkZ)) {
            return 20;
        }
        const auto* const* const vtable = *reinterpret_cast<const void* const* const*>(gen);
        int slot = -1;
        for (int i = 0; i < kMaxVtableSlots; ++i) {
            if (vtable[i] == g_surface.densityGrid) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            return 21;
        }
        slotOut = slot;
        const std::int32_t pos[2] = {chunkX, chunkZ};
        LARGE_INTEGER frequency{};
        LARGE_INTEGER begin{};
        LARGE_INTEGER end{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&begin);
        reinterpret_cast<DensityGridFn>(const_cast<void*>(g_surface.densityGrid))(const_cast<std::uint8_t*>(gen), grid, pos);
        QueryPerformanceCounter(&end);
        elapsedMs = static_cast<double>(end.QuadPart - begin.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
        return 0;
    } __except (accessFilter(GetExceptionCode())) {
        return 22;
    }
}

void collectSurfaceLevel(const ServerView& view, int feetX, int feetY, int feetZ, Sample& out)
{
    if (!g_surface.ready) {
        noteSurfaceMiss(1, nullptr);
        return;
    }
    alignas(8) float out3[3] = {};
    SurfaceProbe probe{};
    const int reason = sampleSurfaceColumn(static_cast<const std::uint8_t*>(view.chunkSource), view.level,
                                           view.dimension, feetX, feetZ, out3, probe);
    if (reason != 0) {
        noteSurfaceMiss(reason, view.chunkSource);
        return;
    }

    if (out3[2] != 0.0f || !(out3[0] > -64.0f && out3[0] < 64.0f) || !(out3[1] > -4096.0f && out3[1] < 4096.0f)) {
        noteSurfaceMiss(8, nullptr);
        return;
    }
    int cellMin = 0;
    int count = 0;
    dbgsurface::surfaceCells(probe.minY, probe.maxY, cellMin, count);
    if (cellMin < -64 || cellMin > 0 || count < 8 || count > 128) {
        noteSurfaceMiss(9, nullptr);
        return;
    }
    int y = 0;
    out.hasSurfaceLevel = true;
    out.surfaceLevelFound = dbgsurface::findTopSurface(g_surface.k, out3[0], out3[1], cellMin, count, y);
    out.surfaceLevel = y;

    static float grid[dbgsurface::kGridCells];
    static const void* gridSource = nullptr;
    static int gridX = 0;
    static int gridZ = 0;
    static bool gridValid = false;
    static bool gridDisabled = false;
    static const void* disabledFor = nullptr;
    if (disabledFor != view.chunkSource) {
        gridDisabled = false;
        disabledFor = nullptr;
    }
    const int chunkX = feetX >> 4;
    const int chunkZ = feetZ >> 4;
    double elapsed = 0.0;
    int slot = -1;
    int gridReason = -1;
    if (!gridDisabled && (!gridValid || gridSource != view.chunkSource || gridX != chunkX || gridZ != chunkZ)) {
        gridValid = false;
        gridReason = buildDensityGrid(static_cast<const std::uint8_t*>(view.chunkSource), view.level, view.dimension,
                                      chunkX, chunkZ, grid, elapsed, slot);
        if (gridReason == 0) {
            gridValid = true;
            gridSource = view.chunkSource;
            gridX = chunkX;
            gridZ = chunkZ;
            if (elapsed > kGridBudgetMs) {

                gridDisabled = true;
                disabledFor = view.chunkSource;
            }
        } else {
            noteSurfaceMiss(gridReason, view.chunkSource);

            if (gridReason != 20) {
                gridDisabled = true;
                disabledFor = view.chunkSource;
            }
        }
    }
    float density = 0.0f;
    if (gridValid && gridSource == view.chunkSource && gridX == chunkX && gridZ == chunkZ
        && dbgsurface::interpolateGrid(grid, cellMin, feetX, feetY, feetZ, density) && density > -1.0e6f
        && density < 1.0e6f) {
        out.hasDensity = true;
        out.density = density / 128.0f;
    }

    static std::atomic<bool> told{false};
    if (!told.exchange(true)) {
        log().info(L"DebugScreen: surface probe gen={} sampler={} factory={} noBlend={} y={}..{} offset={} factor={} "
                   L"cells={}/{} PS={}{} grid={} slot={} {:.2f} ms",
                   probe.gen, probe.sampler, probe.factory, probe.noBlend, probe.minY, probe.maxY, out3[0], out3[1],
                   cellMin, count, out.surfaceLevelFound ? L"" : L"not found ", y, gridReason, slot, elapsed);
    }
    static std::atomic<bool> toldGrid{false};
    if (gridReason == 0 && !toldGrid.exchange(true)) {
        log().info(L"DebugScreen: density grid of chunk ({}, {}) in {:.2f} ms (slot {}), N={} at y {}", chunkX, chunkZ,
                   elapsed, slot, out.hasDensity ? out.density : 0.0f, feetY);
    }
    static std::atomic<bool> toldSlow{false};
    if (gridReason == 0 && elapsed > kGridBudgetMs && !toldSlow.exchange(true)) {
        log().warn(L"DebugScreen: the density grid took {:.2f} ms; N is computed only once in this world", elapsed);
    }
}

struct DiscardTargets {
    const void* dbVtable = nullptr;
    std::int32_t setHead = 0;
    std::int32_t setSize = 0;
};
DiscardTargets g_discard{};

bool readDiscardCount(const std::uint8_t* chunkSource, int& pending)
{
    __try {
        const std::uint8_t* db = chunkSource;
        for (int hop = 0; hop < kMaxParentHops && db != nullptr; ++hop) {
            if (*reinterpret_cast<const void* const*>(db) == g_discard.dbVtable) {
                break;
            }
            db = *reinterpret_cast<const std::uint8_t* const*>(db + kChunkSourceParent);
        }
        if (db == nullptr || *reinterpret_cast<const void* const*>(db) != g_discard.dbVtable) {
            return false;
        }
        const std::uint64_t size = *reinterpret_cast<const std::uint64_t*>(db + g_discard.setSize);
        const auto* const head = *reinterpret_cast<const std::uint8_t* const*>(db + g_discard.setHead);
        if (head == nullptr || size > 4096) {
            return false;
        }
        std::uint64_t walked = 0;
        for (const auto* node = *reinterpret_cast<const std::uint8_t* const*>(head); node != head;
             node = *reinterpret_cast<const std::uint8_t* const*>(node)) {
            if (node == nullptr || ++walked > size) {
                return false;
            }
        }
        if (walked != size) {
            return false;
        }
        pending = static_cast<int>(size);
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

void collectServer(void* clientRegion, const Wanted& wanted, int feetX, int feetY, int feetZ, int simulationDistance,
                   bool overworld, Sample& out)
{
    ServerView view{};
    if (!serverView(clientRegion, view)) {
        return;
    }
    if (wanted.forcedChunks) {
        collectForcedChunks(clientRegion, view.dimension, out);
    }

    if (wanted.climate && overworld) {
        collectClimate(view, feetX, feetY, feetZ, out);
        collectSurfaceLevel(view, feetX, feetY, feetZ, out);
    }
    if (!wanted.serverChunks) {
        return;
    }
    std::uint64_t count = 0;
    const std::uint8_t* chunk = nullptr;
    int loading = 0;
    if (!walkServerChunks(static_cast<const std::uint8_t*>(view.chunkSource), feetX >> 4, feetZ >> 4, false, count,
                          chunk, loading)) {
        noteServerMiss(7, view.chunkSource, nullptr);
        return;
    }
    if (wanted.serverChunks) {
        out.hasServerChunks = true;
        out.serverChunks = static_cast<int>(count);
        out.serverLoadingChunks = loading;
        int pending = 0;
        if (g_discard.dbVtable != nullptr && readDiscardCount(static_cast<const std::uint8_t*>(view.chunkSource), pending)) {
            out.hasServerUnload = true;
            out.serverUnloadChunks = pending;
        }
        if (simulationDistance > 0) {
            collectServerEntities(feetX >> 4, feetZ >> 4, simulationDistance, out);
        }
    }
}

}

bool worldHeightAt(int x, int z, int& minY, int& maxYExclusive)
{
    void* const region = blockwrite::renderRegion();
    if (region == nullptr || !blockwrite::regionIsAlive(region)) return false;
    static void* lastRegion = nullptr;
    static void* lastSource = nullptr;
    static std::uint64_t settleUntil = 0;
    void* source = nullptr;
    readPointer(static_cast<std::uint8_t*>(region) + kBlockSourceChunkSource, source);
    const std::uint64_t now = GetTickCount64();
    if (region != lastRegion || source != lastSource) {
        lastRegion = region;
        lastSource = source;
        settleUntil = now + 1000;
    }
    if (now < settleUntil) return false;
    void* const chunk = chunkAt(region, x, 0, z);
    if (chunk == nullptr || !memory::plausiblePointer(chunk)) return false;
    std::int32_t low = 0;
    std::int32_t high = 0;
    auto* const bytes = static_cast<std::uint8_t*>(chunk);
    if (!readInt32(bytes + kLevelChunkMinY, low) || !readInt32(bytes + kLevelChunkMaxY, high) || high < low
        || high - low > 4096) {
        return false;
    }
    minY = low;
    maxYExclusive = high + 1;
    return true;
}

using AttachPosFn = float*(__fastcall*)(void* actor, float* out, int location, float alpha);
using ViewVectorFn = float*(__fastcall*)(void* actor, float* out, float alpha);
AttachPosFn g_attachPos = nullptr;
ViewVectorFn g_viewVectorFn = nullptr;
std::int32_t g_rotationField = 0;
constexpr std::uint32_t kHeadRotationTypeId = 0xBABE7211u;
constexpr std::size_t kHeadRotationStride = 8;

constexpr int kEyesLocation = 6;
constexpr std::uint32_t kMobFlagTypeId = 0x14EA24A2u;

bool actorEyeRaw(void* actor, float headYaw, float eye[3], float look[3])
{
    __try {
        float at[4]{};
        float dir[4]{};
        g_attachPos(actor, at, kEyesLocation, 1.0F);
        const float* rotation = g_rotationField != 0 && std::isfinite(headYaw)
                                    ? *reinterpret_cast<float* const*>(static_cast<std::byte*>(actor) + g_rotationField)
                                    : nullptr;
        if (rotation != nullptr && std::isfinite(rotation[0])) {
            debuglines::viewVector(rotation[0], headYaw, dir);
        } else {
            g_viewVectorFn(actor, dir, 1.0F);
        }
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(at[i]) || !std::isfinite(dir[i])) return false;
            eye[i] = at[i];
            look[i] = dir[i];
        }
        const float length = look[0] * look[0] + look[1] * look[1] + look[2] * look[2];
        return length > 0.25F && length < 4.0F;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int collectHitBoxes(debuglines::HitBox* out, int cap, float range, bool includeSelf)
{
    if (out == nullptr || cap <= 0 || !std::isfinite(range) || range <= 0) return 0;
    GameData& game = GameData::instance();
    void* const player = game.player();
    void* registryPointer = nullptr;
    std::int32_t selfId = 0;
    float x = 0, y = 0, z = 0;
    if (player == nullptr || !game.playerBoxFeet(x, y, z)
        || !readPointer(static_cast<std::uint8_t*>(player) + kActorRegistry, registryPointer)
        || !readInt32(static_cast<std::uint8_t*>(player) + kActorEntityId, selfId)
        || !memory::plausiblePointer(registryPointer)) return 0;
    std::uintptr_t owners = 0, boxes = 0;
    const auto registry = reinterpret_cast<std::uintptr_t>(registryPointer);
    if (!findStore(registry, kActorOwnerTypeId, owners)
        || !findStore(registry, kAabbTypeId, boxes)) return 0;
    std::uintptr_t mobs = 0;
    const bool haveMobs = findStore(registry, kMobFlagTypeId, mobs);
    std::uintptr_t heads = 0;
    const bool haveHeads = findStore(registry, kHeadRotationTypeId, heads);
    static std::uint32_t packed[kMaxEntities];
    int count = 0;
    if (!readPacked(owners, packed, kMaxEntities, count)) return 0;
    int written = 0;
    const float rangeSquared = range * range;
    for (int i = 0; i < count && written < cap; ++i) {
        const auto id = packed[i];
        if (!includeSelf && id == static_cast<std::uint32_t>(selfId)) continue;
        std::uint32_t position = 0;
        if (!packedIndexOf(owners, id, position) || position != static_cast<std::uint32_t>(i)) continue;
        std::uintptr_t address = 0;
        debuglines::HitBox box{};
        if (!componentAt(boxes, id, kAabbStride, address) || !readHitBox(address, id, box)) continue;
        const float cx = (box.min[0] + box.max[0]) * 0.5F - x;
        const float cy = (box.min[1] + box.max[1]) * 0.5F - y;
        const float cz = (box.min[2] + box.max[2]) * 0.5F - z;
        if (cx * cx + cy * cy + cz * cz > rangeSquared) continue;

        std::uintptr_t ownerAt = 0;
        void* actor = nullptr;
        float eye[3]{};
        float headYaw = std::numeric_limits<float>::quiet_NaN();
        std::uintptr_t headAt = 0;
        if (haveHeads && componentAt(heads, id, kHeadRotationStride, headAt)) {
            float value = 0.0F;
            if (memory::copyGuarded(reinterpret_cast<const void*>(headAt), &value, sizeof(value))) headYaw = value;
        }
        if (g_attachPos != nullptr && g_viewVectorFn != nullptr
            && componentAt(owners, id, kActorOwnerStride, ownerAt)
            && readPointer(reinterpret_cast<const void*>(ownerAt), actor) && looksLikeGameObject(actor)
            && actorEyeRaw(actor, headYaw, eye, box.look)) {
            box.hasEye = true;

            box.eye[0] = (box.max[0] - box.min[0]) * 0.5F;
            box.eye[1] = eye[1] - box.min[1];
            box.eye[2] = (box.max[2] - box.min[2]) * 0.5F;
            std::uint32_t mobAt = 0;
            box.living = haveMobs && packedIndexOf(mobs, id, mobAt);
        }
        out[written++] = box;
    }
    return written;
}

void setActorFunctions(void* attachPos, void* viewVector, std::int32_t rotationField)
{
    g_attachPos = reinterpret_cast<AttachPosFn>(attachPos);
    g_viewVectorFn = reinterpret_cast<ViewVectorFn>(viewVector);
    g_rotationField = rotationField;
}

bool collect(int feetX, int feetY, int feetZ, const Wanted& wanted, bool force)
{

    static std::uint64_t lastTick = 0;
    const std::uint64_t now = GetTickCount64();
    if (!force && now - lastTick < 50) {
        return false;
    }
    lastTick = now;

    Sample out{};
    void* const region = blockwrite::renderRegion();
    if (region == nullptr || !blockwrite::regionIsAlive(region)) {

        publish(out);
        return true;
    }

    static void* lastRegion = nullptr;
    static void* lastSource = nullptr;
    static std::uint64_t settleUntil = 0;
    void* source = nullptr;
    readPointer(static_cast<std::uint8_t*>(region) + kBlockSourceChunkSource, source);
    if (region != lastRegion || source != lastSource) {
        lastRegion = region;
        lastSource = source;
        settleUntil = now + 1000;
        g_airBlock = nullptr;
    }
    if (now < settleUntil) {
        publish(out);
        return true;
    }

    void* level = nullptr;
    if (readPointer(static_cast<std::uint8_t*>(region) + kBlockSourceLevel, level)
        && looksLikeGameObject(level)) {
        void* data = nullptr;
        if (readPointer(static_cast<std::uint8_t*>(level) + kLevelData, data)
            && memory::plausiblePointer(data)) {
            auto* const bytes = static_cast<std::uint8_t*>(data);
            std::int32_t time = 0;

            if (readInt32(bytes + kLevelDataTime, time) && time >= 0) {
                out.hasTime = true;
                out.dayTime = time;
            }
            std::int32_t distance = 0;
            if (readInt32(bytes + kLevelDataSimulationDistance, distance) && distance > 0
                && distance <= 64) {
                out.hasSimulationDistance = true;
                out.simulationDistance = distance;
            }
            std::int32_t difficulty = 0;
            if (readInt32(bytes + kLevelDataDifficulty, difficulty) && difficulty >= 0
                && difficulty <= 3) {
                out.hasDifficulty = true;
                out.difficultyId = difficulty;
            }
        }
    }

    void* const sub = subChunkAt(region, feetX, feetY, feetZ);
    if (sub != nullptr) {
        const int index = ((feetX & 15) << 8) | ((feetZ & 15) << 4) | (feetY & 15);
        int sky = 0;
        int block = 0;
        if (readLightNibble(sub, kSubChunkSkyLight, index, sky)
            && readLightNibble(sub, kSubChunkBlockLight, index, block) && sky >= 0 && sky <= 15
            && block >= 0 && block <= 15) {
            out.hasLight = true;
            out.skyLight = sky;
            out.blockLight = block;
        }
    }

    void* const chunk = chunkAt(region, feetX, feetY, feetZ);
    if (chunk != nullptr && memory::plausiblePointer(chunk)) {
        auto* const bytes = static_cast<std::uint8_t*>(chunk);
        std::int32_t minY = 0;
        std::int32_t maxY = 0;
        if (readInt32(bytes + kLevelChunkMinY, minY) && readInt32(bytes + kLevelChunkMaxY, maxY)) {

            if (minY == -64 && maxY == 319) {
                out.hasDimension = true;
                out.dimensionId = 0;
            } else if (minY == 0 && maxY == 127) {
                out.hasDimension = true;
                out.dimensionId = 1;
            } else if (minY == 0 && maxY == 255) {
                out.hasDimension = true;
                out.dimensionId = 2;
            }
        }
    }

    if (wanted.speed) {
        collectSpeed(out);
    }
    if (wanted.chunks) {
        collectChunkGrid(region, out);
    }
    if (wanted.target || wanted.blockTags) {
        collectTarget(region, wanted.blockTags, out);
    }
    if (wanted.entities) {
        collectEntities(out);
    }
    if (wanted.entityTarget || wanted.entityTags) {
        collectEntityTarget(wanted.entityTags, out);
    }
    if (wanted.fluid) {
        collectFluid(region, out);
    }
    if (wanted.biome) {
        collectBiome(region, feetX, feetY, feetZ, out);
    }

    static Sample slow{};
    static std::uint64_t slowTick = 0;
    const bool wantServer =
        wanted.localServer
        && (wanted.serverChunks || wanted.forcedChunks || wanted.climate);
    if ((wanted.sections || wantServer || wanted.localDifficulty) && now - slowTick >= 250) {
        slowTick = now;
        slow = Sample{};
        if (wanted.sections) {
            collectSections(region, slow);
        }
        if (wantServer) {
            collectServer(region, wanted, feetX, feetY, feetZ, out.hasSimulationDistance ? out.simulationDistance : 0,
                          out.hasDimension && out.dimensionId == 0, slow);
        }
        if (wanted.localDifficulty && out.hasDimension) {
            collectLocalDifficulty(region, level, out.dimensionId,
                                   out.hasDifficulty ? out.difficultyId : -1, wanted.localServer, slow);
        }
    }

    if (wanted.serverChunks && slow.hasServerUnload) {
        out.hasServerUnload = true;
        out.serverUnloadChunks = slow.serverUnloadChunks;
    }
    if (wanted.climate && slow.hasSurfaceLevel) {
        out.hasSurfaceLevel = true;
        out.surfaceLevelFound = slow.surfaceLevelFound;
        out.surfaceLevel = slow.surfaceLevel;
    }
    if (wanted.climate && slow.hasDensity) {
        out.hasDensity = true;
        out.density = slow.density;
    }
    if (wanted.serverChunks && slow.hasServerChunks) {
        out.hasServerChunks = true;
        out.serverChunks = slow.serverChunks;
        out.serverLoadingChunks = slow.serverLoadingChunks;
        out.hasServerEntities = slow.hasServerEntities;
        out.serverEntityCount = slow.serverEntityCount;
        out.serverVisibleEntities = slow.serverVisibleEntities;
        out.serverEntitySections = slow.serverEntitySections;
    }
    if (wanted.localDifficulty && slow.hasLocalDifficulty) {
        out.hasLocalDifficulty = true;
        out.localDifficulty = slow.localDifficulty;
        out.hasLocalDifficultyRaw = slow.hasLocalDifficultyRaw;
        out.localDifficultyRaw = slow.localDifficultyRaw;
    }
    if (wanted.forcedChunks && slow.hasForcedChunks) {
        out.hasForcedChunks = true;
        out.forcedChunks = slow.forcedChunks;
    }
    if (wanted.climate && slow.hasClimate) {
        out.hasClimate = true;
        for (int i = 0; i < 6; ++i) {
            out.climate[i] = slow.climate[i];
        }
    }
    if (wanted.sections && slow.hasSections) {
        out.hasSections = true;
        out.sectionsNonEmpty = slow.sectionsNonEmpty;
        out.sectionsTotal = slow.sectionsTotal;
    }

    publish(out);
    return true;
}

namespace {

constexpr std::ptrdiff_t kParticleMaps = 0x9f8;
constexpr std::ptrdiff_t kParticleMapBytes = 0x40;
constexpr int kParticleKinds = 3;
constexpr std::ptrdiff_t kMapHead = 0x08;
constexpr std::ptrdiff_t kNodeListBegin = 0x18;
constexpr std::ptrdiff_t kNodeListEnd = 0x20;
constexpr int kMaxParticleNodes = 1024;
constexpr std::int64_t kMaxParticles = 1 << 20;

bool walkParticleMaps(const std::uint8_t* engine, std::int64_t& total)
{
    __try {
        for (int kind = 0; kind < kParticleKinds; ++kind) {
            const std::uint8_t* const map = engine + kParticleMaps + kind * kParticleMapBytes;
            const std::uint8_t* const head = *reinterpret_cast<const std::uint8_t* const*>(map + kMapHead);
            if (head == nullptr) {
                return false;
            }
            const std::uint8_t* node = *reinterpret_cast<const std::uint8_t* const*>(head);
            for (int i = 0; node != head; ++i) {
                if (i >= kMaxParticleNodes || node == nullptr) {
                    return false;
                }
                const auto begin = *reinterpret_cast<const std::uintptr_t*>(node + kNodeListBegin);
                const auto end = *reinterpret_cast<const std::uintptr_t*>(node + kNodeListEnd);
                if (end < begin || (end - begin) % 8 != 0) {
                    return false;
                }
                total += static_cast<std::int64_t>((end - begin) / 8);
                if (total > kMaxParticles) {
                    return false;
                }
                node = *reinterpret_cast<const std::uint8_t* const*>(node);
            }
        }
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

}

bool countLegacyParticles(const void* engine, int& out)
{
    if (!memory::plausiblePointer(engine)) {
        return false;
    }
    std::int64_t total = 0;
    if (!walkParticleMaps(static_cast<const std::uint8_t*>(engine), total)) {
        return false;
    }
    out = static_cast<int>(total);
    return true;
}

namespace {

constexpr std::ptrdiff_t kFrameArgToThis = 0x660;
constexpr std::ptrdiff_t kThisParticleOwner = 0x1188;
constexpr std::ptrdiff_t kOwnerLegacyEngine = 0x3a8;
constexpr std::ptrdiff_t kOwnerSystemControl = 0x3b0;
constexpr std::ptrdiff_t kOwnerSystemEngine = 0x3c0;
constexpr std::ptrdiff_t kSystemEffects = 0x18;
constexpr std::ptrdiff_t kSystemEffectCount = 0x28;
constexpr std::ptrdiff_t kEffectParticlesBegin = 0x50;
constexpr std::ptrdiff_t kEffectParticlesEnd = 0x58;
constexpr std::uintptr_t kParticleBytes = 0xd8;
constexpr std::uintptr_t kMaxEffects = 4096;

bool walkParticleSystem(const std::uint8_t* engine, const std::uint8_t* frameArg, std::int64_t& total)
{
    __try {
        const std::uint8_t* const self = frameArg - kFrameArgToThis;
        const std::uint8_t* const owner = *reinterpret_cast<const std::uint8_t* const*>(self + kThisParticleOwner);
        if (owner == nullptr || *reinterpret_cast<const std::uint8_t* const*>(owner + kOwnerLegacyEngine) != engine) {
            return false;
        }
        const std::uint8_t* const control = *reinterpret_cast<const std::uint8_t* const*>(owner + kOwnerSystemControl);
        if (control == nullptr || *control == 0) {
            return false;
        }
        const std::uint8_t* const system = *reinterpret_cast<const std::uint8_t* const*>(owner + kOwnerSystemEngine);
        if (system == nullptr) {
            return false;
        }
        const auto* const effects = *reinterpret_cast<const std::uint8_t* const* const*>(system + kSystemEffects);
        const std::uintptr_t count = *reinterpret_cast<const std::uintptr_t*>(system + kSystemEffectCount);
        if ((effects == nullptr && count != 0) || count > kMaxEffects) {
            return false;
        }
        for (std::uintptr_t i = 0; i < count; ++i) {
            const std::uint8_t* const effect = effects[i];
            if (effect == nullptr) {
                continue;
            }
            const auto begin = *reinterpret_cast<const std::uintptr_t*>(effect + kEffectParticlesBegin);
            const auto end = *reinterpret_cast<const std::uintptr_t*>(effect + kEffectParticlesEnd);
            if (end < begin || (end - begin) % kParticleBytes != 0) {
                return false;
            }
            total += static_cast<std::int64_t>((end - begin) / kParticleBytes);
            if (total > kMaxParticles) {
                return false;
            }
        }
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

}

bool countDataDrivenParticles(const void* engine, const void* frameArg, int& out)
{
    if (!memory::plausiblePointer(engine) || !memory::plausiblePointer(frameArg)) {
        return false;
    }
    std::int64_t total = 0;
    if (!walkParticleSystem(static_cast<const std::uint8_t*>(engine), static_cast<const std::uint8_t*>(frameArg),
                            total)) {
        return false;
    }
    out = static_cast<int>(total);
    return true;
}

void setSurfaceTargets(const SurfaceTargetsIn& targets, const SurfaceConstantsIn& constants)
{
    dbgsurface::SurfaceConstants k{};
    k.negInv128 = constants.values[0];
    k.one = constants.values[1];
    k.minus90 = constants.values[2];
    k.half = constants.values[3];
    k.minus10 = constants.values[4];
    k.three = constants.values[5];
    k.minus15 = constants.values[6];
    k.plus15 = constants.values[7];
    k.fifty = constants.values[8];
    k.table[0] = constants.table[0];
    k.table[1] = constants.table[1];
    k.topCell = constants.topCell;

    if (!dbgsurface::surfaceConstantsLookRight(k)) {
        log().warn(L"DebugScreen: the preliminary surface loop constants changed; PS is not shown");
        return;
    }
    if (!memory::inGameModule(targets.columnFunction) || targets.samplerFromSubObject <= 0x100
        || targets.samplerFromSubObject >= 0x4000 || targets.factoryFromSubObject <= 0x100
        || targets.factoryFromSubObject >= 0x4000 || targets.factoryNoBlendFlag <= 0 || targets.factoryNoBlendFlag >= 0x60) {
        return;
    }
    g_surface.column = reinterpret_cast<ColumnSampleFn>(targets.columnFunction);
    g_surface.samplerFromSub = targets.samplerFromSubObject;
    g_surface.factoryFromSub = targets.factoryFromSubObject;
    g_surface.noBlendFlag = targets.factoryNoBlendFlag;
    g_surface.densityGrid = memory::inGameModule(targets.densityGrid) ? targets.densityGrid : nullptr;
    g_surface.k = k;
    g_surface.ready = true;
}

void setDiscardTargets(const void* dbVtable, std::int32_t setHead, std::int32_t setSize)
{
    if (!memory::inGameModule(dbVtable) || setHead <= 0x70 || setHead >= 0x1000 || setSize != setHead + 8) {
        return;
    }
    g_discard.dbVtable = dbVtable;
    g_discard.setHead = setHead;
    g_discard.setSize = setSize;
}

void setRegionalDifficultySlot(std::int32_t slot)
{

    if (slot > 0 && slot < 0x2000 && slot % 8 == 0) {
        g_regionalDifficultySlot.store(slot, std::memory_order_release);
    }
}

void setRegionalDifficultyTail(const void* tail)
{
    g_regionalDifficultyTail.store(memory::inGameModule(tail) ? tail : nullptr, std::memory_order_release);
}

void setClimateTargets(void* sampleFunction, const void* generatorVtable, std::int32_t biomeSourceOffset)
{

    if (!memory::inGameModule(sampleFunction) || !memory::inGameModule(generatorVtable) || biomeSourceOffset <= 0x100
        || biomeSourceOffset >= 0x4000) {
        return;
    }
    g_climate.sample = reinterpret_cast<ClimateSampleFn>(sampleFunction);
    g_climate.generatorVtable = generatorVtable;
    g_climate.biomeSourceOffset = biomeSourceOffset;
}

bool read(Sample& out)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint64_t before = g_generation.load(std::memory_order_acquire);
        if ((before & 1) != 0) {
            continue;
        }
        out = g_sample;
        if (g_generation.load(std::memory_order_acquire) == before) {
            return before != 0;
        }
    }
    return false;
}

}
