#include "game/BlockRegistry.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <iterator>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

#include <Windows.h>
#include <intrin.h>

#include "core/Logger.h"
#include "core/Notice.h"
#include "core/Strings.h"
#include "game/ItemIcon.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

namespace tsukuyomi::blocks {

namespace {

constexpr std::ptrdiff_t kNodeLeft = 0x00;
constexpr std::ptrdiff_t kNodeParent = 0x08;
constexpr std::ptrdiff_t kNodeRight = 0x10;
constexpr std::ptrdiff_t kNodeName = 0x28;
constexpr std::ptrdiff_t kNodeValue = 0x50;
constexpr std::ptrdiff_t kLegacyDefaultBlock = 0x240;
constexpr std::ptrdiff_t kBlockLegacyAt = 0x68;

constexpr std::size_t kLeaDisp = 47 + 3;

constexpr std::size_t kMinEntries = 64;
constexpr std::size_t kMaxEntries = 100000;

std::size_t g_lastEntries = 0;

__declspec(noinline) bool safeCopy(void* to, const void* from, std::size_t size) noexcept
{
    __try {
        std::memcpy(to, from, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const void* readPointer(const void* at, std::ptrdiff_t offset)
{
    if (at == nullptr) {
        return nullptr;
    }
    const void* value = nullptr;
    if (!safeCopy(&value, reinterpret_cast<const std::uint8_t*>(at) + offset, sizeof(value))) {
        return nullptr;
    }
    return value;
}

bool readStdString(const void* at, std::string& out)
{
    std::uint8_t header[0x20]{};
    if (at == nullptr || !safeCopy(header, at, sizeof(header))) {
        return false;
    }
    std::uint64_t size = 0;
    std::uint64_t capacity = 0;
    std::memcpy(&size, header + 0x10, sizeof(size));
    std::memcpy(&capacity, header + 0x18, sizeof(capacity));
    if (size == 0 || size > 128 || capacity < size) {
        return false;
    }

    char text[129]{};
    if (capacity <= 15) {
        std::memcpy(text, header, static_cast<std::size_t>(size));
    } else {
        const void* pointer = nullptr;
        std::memcpy(&pointer, header, sizeof(pointer));
        if (!safeCopy(text, pointer, static_cast<std::size_t>(size))) {
            return false;
        }
    }
    for (std::uint64_t i = 0; i < size; ++i) {
        const auto one = static_cast<unsigned char>(text[i]);
        if (one < 0x20 || one >= 0x7F) {
            return false;
        }
    }
    out.assign(text, static_cast<std::size_t>(size));
    return true;
}

const void* tableGlobal()
{
    const Scanner& scanner = Scanner::instance();
    if (!scanner.found(Target::BlockRegistryRef)) {
        return nullptr;
    }
    return memory::ripTarget(scanner.address(Target::BlockRegistryRef), kLeaDisp);
}

using TransformBlockFn = const void*(__fastcall*)(const void*, unsigned, unsigned);

__declspec(noinline) const void* callTransformBlock(TransformBlockFn fn, const void* block,
                                                    unsigned rotation, unsigned mirror,
                                                    bool& faulted) noexcept
{
    faulted = false;
    __try {
        return fn(block, rotation, mirror);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return nullptr;
    }
}

}

const void* rotatedBlock(const void* block, int quarters, bool* ok)
{
    if (ok != nullptr) {
        *ok = false;
    }
    const int turn = ((quarters % 4) + 4) % 4;
    if (block == nullptr || turn == 0) {
        if (ok != nullptr) {
            *ok = (block != nullptr);
        }
        return block;
    }
    const Scanner& scanner = Scanner::instance();
    if (!scanner.found(Target::BlockTransform)) {
        return block;
    }
    if (!memory::isReadable(block, 0x128)) {
        return block;
    }
    const void* const legacy = readPointer(block, 0x68);
    if (legacy == nullptr || !memory::isReadable(legacy, 0x330)) {
        return block;
    }
    const auto fn = reinterpret_cast<TransformBlockFn>(scanner.address(Target::BlockTransform));
    bool faulted = false;
    const void* const turned = callTransformBlock(fn, block, static_cast<unsigned>(turn), 0u, faulted);
    if (faulted) {
        static std::atomic<int> said{0};
        if (said.fetch_add(1, std::memory_order_relaxed) < 4) {
            log().warn(L"BlockRegistry: transformBlock faulted ({:#x} / rotation {})",
                       reinterpret_cast<std::uintptr_t>(block),
                       turn * 90);
        }
        return block;
    }
    if (turned == nullptr || !memory::isReadable(turned, 0x70)) {
        return block;
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return turned;
}

std::size_t lastEntryCount()
{
    return g_lastEntries;
}

bool resolve(const std::vector<std::string>& wanted, Table& out)
{
    g_lastEntries = 0;

    const void* const global = tableGlobal();
    if (global == nullptr) {
        log().warn(L"BlockRegistry: the block table signature did not match");
        return false;
    }

    const void* head = nullptr;
    std::uint64_t declared = 0;
    if (!safeCopy(&head, global, sizeof(head))
        || !safeCopy(&declared, reinterpret_cast<const std::uint8_t*>(global) + 8,
                     sizeof(declared))) {
        log().warn(L"BlockRegistry: could not read the block table");
        return false;
    }
    if (head == nullptr || declared < kMinEntries || declared > kMaxEntries) {
        log().warn(L"BlockRegistry: the block table looks wrong ({} entries)", declared);
        return false;
    }

    std::vector<const void*> stack;
    stack.reserve(64);
    stack.push_back(readPointer(head, kNodeParent));

    const bool takeAll = wanted.empty();
    std::size_t remaining = wanted.size();
    const std::size_t guard = static_cast<std::size_t>(declared) * 4 + 64;
    std::size_t steps = 0;
    std::size_t rejected = 0;
    std::string firstRejected;
    const void* firstRejectedLegacy = nullptr;
    const void* firstRejectedBlock = nullptr;

    while (!stack.empty() && steps < guard) {
        ++steps;
        const void* const node = stack.back();
        stack.pop_back();
        if (node == nullptr || node == head) {
            continue;
        }

        std::string name;
        if (readStdString(reinterpret_cast<const std::uint8_t*>(node) + kNodeName, name)) {
            ++g_lastEntries;
            const bool asked =
                takeAll || std::find(wanted.begin(), wanted.end(), name) != wanted.end();
            if (asked) {
                const void* const legacy = readPointer(node, kNodeValue);
                const void* const block = readPointer(legacy, kLegacyDefaultBlock);
                const bool verified = legacy != nullptr && block != nullptr
                                      && readPointer(block, kBlockLegacyAt) == legacy;
                if (!verified && block != nullptr) {
                    if (rejected++ == 0) {
                        firstRejected = name;
                        firstRejectedLegacy = legacy;
                        firstRejectedBlock = block;
                    }
                }
                if (verified && out.emplace(name, block).second && !takeAll
                    && remaining > 0) {
                    --remaining;
                    if (remaining == 0) {
                        break;
                    }
                }
            }
        }

        stack.push_back(readPointer(node, kNodeLeft));
        stack.push_back(readPointer(node, kNodeRight));
    }

    if (steps >= guard) {
        log().warn(L"BlockRegistry: stopped walking the block table (looped?)");
    }
    if (rejected != 0) {
        static std::atomic<int> told{0};
        if (told.fetch_add(1, std::memory_order_relaxed) < 8) {
            log().warn(L"BlockRegistry: {} name(s) whose default state does not point back at "
                       L"their own BlockLegacy - dropping them (first: {} / BlockLegacy {:#x} "
                       L"/ Block {:#x})",
                       rejected,
                       toUtf16(firstRejected),
                       reinterpret_cast<std::uintptr_t>(firstRejectedLegacy),
                       reinterpret_cast<std::uintptr_t>(firstRejectedBlock));
        }
    }
    return true;
}

namespace {

struct StateSlot {
    std::string name;
    std::uint32_t values = 0;
    std::uint32_t shift = 0;
    std::uint32_t mask = 0;
    const void* desc = nullptr;
    std::vector<std::string> names;
};

using StateValueNameFn = void*(__fastcall*)(const void*, void*, unsigned int);

bool stateValueName(const void* desc, unsigned int value, std::string& out)
{
    out.clear();
    if (desc == nullptr || !memory::isReadable(desc, sizeof(void*))) {
        return false;
    }
    const void* vtable = nullptr;
    if (!safeCopy(&vtable, desc, sizeof(vtable)) || vtable == nullptr
        || !memory::isReadable(vtable, 0x10)) {
        return false;
    }
    StateValueNameFn fn = nullptr;
    if (!safeCopy(&fn, static_cast<const std::uint8_t*>(vtable) + 8, sizeof(fn))
        || fn == nullptr) {
        return false;
    }
    alignas(8) std::uint8_t holder[0x20] = {};
    fn(desc, holder, value);

    const void* tag = nullptr;
    std::memcpy(&tag, holder, sizeof(tag));
    if (tag == nullptr || !memory::isReadable(tag, 0x28)) {
        return false;
    }
    return readStdString(static_cast<const std::uint8_t*>(tag) + 0x08, out);
}

std::vector<std::string> readStateValueNames(const void* desc, std::uint32_t count)
{
    std::vector<std::string> out;
    if (desc == nullptr || count == 0 || count > 64) {
        return out;
    }
    out.reserve(count);
    std::string one;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!stateValueName(desc, i, one)) {
            return {};
        }
        out.push_back(one);
    }
    return out;
}
std::vector<StateSlot> readStateSlots(const void* legacy)
{
    std::vector<StateSlot> out;
    const void* const head = readPointer(legacy, 0x1d8);
    if (head == nullptr || !memory::isReadable(head, 0x20)) {
        return out;
    }
    std::vector<const void*> stack{readPointer(head, 0x08)};
    for (int guard = 0; !stack.empty() && guard < 512 && out.size() < 64; ++guard) {
        const void* const node = stack.back();
        stack.pop_back();
        if (node == nullptr || node == head || !memory::isReadable(node, 0x40)) {
            continue;
        }
        std::uint8_t raw[0x40] = {};
        if (!safeCopy(raw, node, sizeof(raw))) {
            continue;
        }
        if (raw[0x19] == 0) {
            StateSlot slot;
            std::uint32_t width = 0;
            std::uint32_t high = 0;
            std::memcpy(&slot.values, raw + 0x28, 4);
            std::memcpy(&width, raw + 0x2c, 4);
            std::memcpy(&high, raw + 0x30, 4);
            std::memcpy(&slot.mask, raw + 0x34, 4);
            slot.shift = (high >= width && width > 0) ? (high - width + 1) : 0;
            const void* desc = nullptr;
            std::memcpy(&desc, raw + 0x38, sizeof(desc));
            if (desc != nullptr && memory::isReadable(desc, 0x40)) {
                readStdString(static_cast<const std::uint8_t*>(desc) + 0x20, slot.name);
                slot.desc = desc;
            }
            if (!slot.name.empty() && slot.values > 0 && slot.mask != 0) {
                slot.names = readStateValueNames(slot.desc, slot.values);
                out.push_back(std::move(slot));
            }
        }
        stack.push_back(readPointer(node, 0x00));
        stack.push_back(readPointer(node, 0x10));
    }
    return out;
}

}

std::string stateNamesOf(const void* block)
{
    if (block == nullptr || !memory::isReadable(block, 0x128)) {
        return {};
    }
    const void* const legacy = readPointer(block, 0x68);
    if (legacy == nullptr || !memory::isReadable(legacy, 0x248)) {
        return {};
    }
    std::string out;
    for (const StateSlot& one : readStateSlots(legacy)) {
        if (out.size() > 400) {
            out += "...";
            break;
        }
        out += one.name + "(" + std::to_string(one.values) + ") ";
    }
    return out;
}

namespace {

std::mutex g_blockLookupMutex;
std::unordered_map<const void*, std::string> g_legacyNames;
bool g_legacyNamesReady = false;
std::unordered_map<const void*, std::vector<StateSlot>> g_legacySlots;

}

bool nameOfBlock(const void* block, char* out, std::size_t cap)
{
    if (out == nullptr || cap == 0) {
        return false;
    }
    out[0] = '\0';
    const void* const legacy = legacyOfFast(block);
    if (legacy == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_blockLookupMutex);
    if (!g_legacyNamesReady) {
        Table table;
        if (!resolve({}, table)) {
            return false;
        }
        for (const auto& [name, def] : table) {
            const void* const key = legacyOfFast(def);
            if (key != nullptr) {
                g_legacyNames.emplace(key, name);
            }
        }
        g_legacyNamesReady = true;
    }
    const auto found = g_legacyNames.find(legacy);
    if (found == g_legacyNames.end()) {
        return false;
    }
    std::snprintf(out, cap, "%s", found->second.c_str());
    return true;
}

int statesOfBlock(const void* block, char out[][kStateTextBytes], int maxStates)
{
    if (block == nullptr || out == nullptr || maxStates <= 0 || !memory::isReadable(block, 0x128)) {
        return 0;
    }
    const void* const legacy = readPointer(block, kBlockLegacyAt);
    if (legacy == nullptr || !memory::isReadable(legacy, 0x248)) {
        return 0;
    }
    std::uint16_t index = 0;
    if (!safeCopy(&index, static_cast<const std::uint8_t*>(block) + 0x120, sizeof(index))) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_blockLookupMutex);
    auto slots = g_legacySlots.find(legacy);
    if (slots == g_legacySlots.end()) {
        slots = g_legacySlots.emplace(legacy, readStateSlots(legacy)).first;
    }
    int rows = 0;
    for (const StateSlot& slot : slots->second) {
        if (rows >= maxStates) {
            break;
        }
        const std::uint32_t value = slot.mask != 0 ? ((index & slot.mask) >> slot.shift) : 0;
        if (!slot.names.empty() && value < slot.names.size()) {
            std::snprintf(out[rows], kStateTextBytes, "%s: %s", slot.name.c_str(),
                          slot.names[value].c_str());
        } else {
            std::snprintf(out[rows], kStateTextBytes, "%s: %u", slot.name.c_str(), value);
        }
        ++rows;
    }
    return rows;
}

int tagsOfBlock(const void* block, char out[][kStateTextBytes], int maxTags)
{
    constexpr std::ptrdiff_t kTagsBegin = 0x1a0;
    constexpr std::ptrdiff_t kTagsEnd = 0x1a8;
    constexpr std::uintptr_t kTagBytes = 0x30;
    constexpr std::ptrdiff_t kTagText = 0x08;
    if (block == nullptr || out == nullptr || maxTags <= 0) {
        return 0;
    }
    const void* const legacy = readPointer(block, kBlockLegacyAt);
    const auto begin = reinterpret_cast<std::uintptr_t>(readPointer(legacy, kTagsBegin));
    const auto end = reinterpret_cast<std::uintptr_t>(readPointer(legacy, kTagsEnd));
    if (legacy == nullptr || begin == 0 || end < begin || (end - begin) % kTagBytes != 0
        || (end - begin) / kTagBytes > 64) {
        return 0;
    }
    int rows = 0;
    for (std::uintptr_t at = begin; at < end && rows < maxTags; at += kTagBytes) {
        std::string tag;
        if (!readStdString(reinterpret_cast<const void*>(at + kTagText), tag)) {
            continue;
        }
        std::snprintf(out[rows], kStateTextBytes, tag.find(':') == std::string::npos ? "#minecraft:%s" : "#%s",
                      tag.c_str());
        ++rows;
    }
    return rows;
}

bool materialFlags(const void* block, bool& blocksMotion, bool& liquid)
{
    constexpr std::ptrdiff_t kMaterialAt = 0x28;
    constexpr std::size_t kLiquidAt = 0x02;
    constexpr std::size_t kBlocksMotionAt = 0x03;
    const void* const legacy = readPointer(block, kBlockLegacyAt);
    const void* const material = readPointer(legacy, kMaterialAt);
    std::uint8_t bytes[4]{};
    if (material == nullptr || !safeCopy(bytes, material, sizeof(bytes))
        || bytes[kLiquidAt] > 1 || bytes[kBlocksMotionAt] > 1) {
        return false;
    }
    liquid = bytes[kLiquidAt] != 0;
    blocksMotion = bytes[kBlocksMotionAt] != 0;
    return true;
}

const void* stateVariant(const void* block, const std::vector<WantedState>& want,
                         std::size_t* missing)
{
    if (missing != nullptr) {
        *missing = 0;
    }
    if (block == nullptr || want.empty() || !memory::isReadable(block, 0x128)) {
        return block;
    }
    const void* const legacy = readPointer(block, 0x68);
    if (legacy == nullptr || !memory::isReadable(legacy, 0x248)) {
        return block;
    }
    const std::vector<StateSlot> slots = readStateSlots(legacy);
    if (slots.empty()) {
        if (missing != nullptr) {
            *missing = want.size();
        }
        return block;
    }

    std::uint16_t index = 0;
    if (!safeCopy(&index, static_cast<const std::uint8_t*>(block) + 0x120, sizeof(index))) {
        return block;
    }

    std::size_t unresolved = 0;
    for (const WantedState& one : want) {
        const auto found = std::find_if(slots.begin(), slots.end(),
                                        [&](const StateSlot& s) { return s.name == one.name; });
        if (found == slots.end()) {
            ++unresolved;
            continue;
        }
        if (one.isText) {
            const auto hit = std::find(found->names.begin(), found->names.end(), one.text);
            if (hit == found->names.end()) {
                ++unresolved;
                continue;
            }
            const auto pick =
                static_cast<std::uint32_t>(hit - found->names.begin()) << found->shift;
            index = static_cast<std::uint16_t>((index & ~found->mask) | (pick & found->mask));
            continue;
        }

        const std::int64_t valueIndex = one.number;
        if (valueIndex < 0 || valueIndex >= static_cast<std::int64_t>(found->values)) {
            ++unresolved;
            continue;
        }
        const std::uint32_t value = static_cast<std::uint32_t>(valueIndex) << found->shift;
        index = static_cast<std::uint16_t>((index & ~found->mask) | (value & found->mask));
    }
    if (missing != nullptr) {
        *missing = unresolved;
    }

    const void* const begin = readPointer(legacy, 0x228);
    const void* const end = readPointer(legacy, 0x230);
    if (begin == nullptr || end <= begin) {
        return block;
    }
    const std::size_t count = static_cast<std::size_t>(
        static_cast<const std::uint8_t*>(end) - static_cast<const std::uint8_t*>(begin))
        / sizeof(void*);
    if (index >= count) {
        return block;
    }
    const void* picked = nullptr;
    if (!safeCopy(&picked, static_cast<const std::uint8_t*>(begin) + index * sizeof(void*),
                  sizeof(picked))
        || picked == nullptr) {
        return block;
    }
    return picked;
}

namespace {

std::atomic<bool> g_ghostOn{false};
std::atomic<bool> g_ghostArmed{false};
std::atomic<bool> g_worldDropped{false};
std::int32_t g_ghostMin[3] = {};
std::int32_t g_ghostMax[3] = {};
std::atomic<float> g_ghostAlpha{0.45F};

std::atomic<bool> g_layerOn{false};
std::atomic<int> g_layerAxis{1};
std::atomic<std::int32_t> g_layerLo{0};
std::atomic<std::int32_t> g_layerHi{0};

std::vector<std::uint16_t> g_drawCells;

std::vector<std::uint16_t> g_drawCells2;

std::vector<std::uint16_t> g_wantCells;
std::vector<std::uint16_t> g_wantCells2;
std::atomic<const std::uint16_t*> g_wantCellsPtr{nullptr};
std::atomic<const std::uint16_t*> g_wantCells2Ptr{nullptr};

constexpr std::uint16_t kWantAirTag = 0xFFFFu;

std::vector<std::uint8_t> g_diffCells;
std::atomic<std::uint8_t*> g_diffCellsPtr{nullptr};
std::atomic<const std::uint16_t*> g_drawCells2Ptr{nullptr};
std::vector<const void*> g_drawPalette;
std::atomic<const std::uint16_t*> g_drawCellsPtr{nullptr};

std::atomic<int> g_drawReaders{0};
std::vector<std::vector<std::uint16_t>> g_retiredCells;
std::vector<std::vector<std::uint8_t>> g_retiredDiff;
std::vector<std::vector<const void*>> g_retiredPalettes;
std::atomic<std::size_t> g_drawCellsCount{0};
std::atomic<const void* const*> g_drawPalettePtr{nullptr};
std::atomic<std::size_t> g_drawPaletteCount{0};
std::atomic<const void*> g_air{nullptr};
std::int32_t g_lastMin[3] = {0, 0, 0};
std::int32_t g_lastMax[3] = {0, 0, 0};
std::atomic<unsigned long long> g_lastBoundsAt{0};
constexpr unsigned long long kLastBoundsMs = 8000;
std::atomic<unsigned long> g_simThread{0};

std::atomic<unsigned long> g_meshThreads[kMeshThreadLimit] = {};
std::atomic<std::size_t> g_meshThreadCount{0};

bool waitForDrawReaders()
{
    for (int i = 0; i < 2000; ++i) {
        if (g_drawReaders.load(std::memory_order_seq_cst) == 0) {
            return true;
        }
        Sleep(0);
        if ((i % 50) == 49) {
            Sleep(1);
        }
    }
    return false;
}

void retireDrawCells()
{
    const bool drained = waitForDrawReaders();
    if (drained) {
        g_retiredCells.clear();
        g_retiredDiff.clear();
        g_retiredPalettes.clear();
        g_drawCells.clear();
        g_drawCells2.clear();
        g_diffCells.clear();
        g_wantCells.clear();
        g_wantCells2.clear();
        g_drawCells.shrink_to_fit();
        g_drawCells2.shrink_to_fit();
        g_diffCells.shrink_to_fit();
        g_wantCells.shrink_to_fit();
        g_wantCells2.shrink_to_fit();
        return;
    }
    notice::failOnce("BlockRegistry.retireCells",
                     L"BlockRegistry: the renderer is still reading the marks; the old table is kept",
                     "Schematica: an old drawing table is retained until its readers finish");
    g_retiredCells.push_back(std::move(g_drawCells));
    g_retiredCells.push_back(std::move(g_drawCells2));
    g_retiredCells.push_back(std::move(g_wantCells));
    g_retiredCells.push_back(std::move(g_wantCells2));
    g_retiredDiff.push_back(std::move(g_diffCells));
    g_drawCells = {};
    g_drawCells2 = {};
    g_wantCells = {};
    g_wantCells2 = {};
    g_diffCells = {};
    if (g_retiredCells.size() > 16 && g_drawReaders.load(std::memory_order_seq_cst) == 0) {
        g_retiredCells.clear();
        g_retiredDiff.clear();
        g_retiredPalettes.clear();
    }
}

bool ghostCellIndex(std::int32_t x, std::int32_t y, std::int32_t z, std::size_t& out)
{
    if (x < g_ghostMin[0] || x >= g_ghostMax[0] || y < g_ghostMin[1] || y >= g_ghostMax[1]
        || z < g_ghostMin[2] || z >= g_ghostMax[2]) {
        return false;
    }
    const std::uint64_t sy = static_cast<std::uint64_t>(g_ghostMax[1] - g_ghostMin[1]);
    const std::uint64_t sz = static_cast<std::uint64_t>(g_ghostMax[2] - g_ghostMin[2]);
    const std::uint64_t dx = static_cast<std::uint64_t>(x - g_ghostMin[0]);
    const std::uint64_t dy = static_cast<std::uint64_t>(y - g_ghostMin[1]);
    const std::uint64_t dz = static_cast<std::uint64_t>(z - g_ghostMin[2]);
    const std::uint64_t at = (dx * sy + dy) * sz + dz;
    if (at >= kGhostCellLimit) {
        return false;
    }
    out = static_cast<std::size_t>(at);
    return true;
}

}

void setGhostRegion(std::int32_t x, std::int32_t y, std::int32_t z,
                    std::int32_t sx, std::int32_t sy, std::int32_t sz, float alpha)
{
    g_ghostOn.store(false, std::memory_order_release);
    g_ghostAlpha.store(alpha, std::memory_order_relaxed);
    g_ghostMin[0] = x;  g_ghostMin[1] = y;  g_ghostMin[2] = z;
    g_ghostMax[0] = x + sx;  g_ghostMax[1] = y + sy;  g_ghostMax[2] = z + sz;

    g_drawCellsPtr.store(nullptr, std::memory_order_seq_cst);
    g_drawCells2Ptr.store(nullptr, std::memory_order_seq_cst);
    g_diffCellsPtr.store(nullptr, std::memory_order_seq_cst);
    g_wantCellsPtr.store(nullptr, std::memory_order_seq_cst);
    g_wantCells2Ptr.store(nullptr, std::memory_order_seq_cst);
    g_drawCellsCount.store(0, std::memory_order_relaxed);
    retireDrawCells();
    const std::uint64_t cells = static_cast<std::uint64_t>(sx) * static_cast<std::uint64_t>(sy)
                                * static_cast<std::uint64_t>(sz);
    const bool usable = cells > 0 && cells <= kGhostCellLimit;
    if (!usable && cells > 0) {
        log().warn(L"Schematica: {} cells is over the limit of {}, so no ghost or "
                   L"color box can be drawn for this schematic (and its collision is kept)",
                   cells, kGhostCellLimit);
    }

    if (usable) {
        g_drawCells.assign(static_cast<std::size_t>(cells), 0);
        g_drawCells2.assign(static_cast<std::size_t>(cells), 0);
        g_diffCells.assign(static_cast<std::size_t>(cells), 0);
        g_wantCells.assign(static_cast<std::size_t>(cells), 0);
        g_wantCells2.assign(static_cast<std::size_t>(cells), 0);
        g_drawCellsCount.store(g_drawCells.size(), std::memory_order_relaxed);
        g_drawCellsPtr.store(g_drawCells.data(), std::memory_order_seq_cst);
        g_drawCells2Ptr.store(g_drawCells2.data(), std::memory_order_seq_cst);
        g_diffCellsPtr.store(g_diffCells.data(), std::memory_order_seq_cst);
        g_wantCellsPtr.store(g_wantCells.data(), std::memory_order_seq_cst);
        g_wantCells2Ptr.store(g_wantCells2.data(), std::memory_order_seq_cst);
    }

    const bool valid = sx > 0 && sy > 0 && sz > 0;
    g_ghostArmed.store(valid, std::memory_order_release);
    if (valid) {
        g_ghostOn.store(true, std::memory_order_release);
    }
}

void clearGhostRegion()
{
    g_ghostOn.store(false, std::memory_order_release);
    if (!g_ghostArmed.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    for (int i = 0; i < 3; ++i) {
        g_lastMin[i] = g_ghostMin[i];
        g_lastMax[i] = g_ghostMax[i];
    }
    g_lastBoundsAt.store(GetTickCount64(), std::memory_order_release);

    g_drawCellsPtr.store(nullptr, std::memory_order_seq_cst);
    g_drawCells2Ptr.store(nullptr, std::memory_order_seq_cst);
    g_diffCellsPtr.store(nullptr, std::memory_order_seq_cst);
    g_wantCellsPtr.store(nullptr, std::memory_order_seq_cst);
    g_wantCells2Ptr.store(nullptr, std::memory_order_seq_cst);
    g_drawCellsCount.store(0, std::memory_order_relaxed);
    retireDrawCells();
}

bool ghostOn() { return g_ghostOn.load(std::memory_order_acquire); }

float ghostAlpha()
{
    return g_ghostAlpha.load(std::memory_order_relaxed);
}

bool lastGhostBounds(std::int32_t* mn, std::int32_t* mx)
{
    if (mn == nullptr || mx == nullptr) {
        return false;
    }
    const unsigned long long at = g_lastBoundsAt.load(std::memory_order_acquire);
    if (at == 0 || GetTickCount64() - at > kLastBoundsMs) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        mn[i] = g_lastMin[i];
        mx[i] = g_lastMax[i];
    }
    return true;
}

bool ghostBounds(std::int32_t* mn, std::int32_t* mx)
{
    if (!g_ghostOn.load(std::memory_order_acquire) || mn == nullptr || mx == nullptr) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        mn[i] = g_ghostMin[i];
        mx[i] = g_ghostMax[i];
    }
    return true;
}

namespace {
constexpr std::size_t kLayerSlots = 1024;
constexpr std::uint8_t kNoLayer = 0xFF;
struct LayerSlot {
    std::atomic<const void*> key{nullptr};
    std::atomic<std::uint8_t> layer{kNoLayer};
};
LayerSlot g_layers[kLayerSlots];

LayerSlot* layerSlot(const void* block, bool insert)
{
    const std::size_t start = (reinterpret_cast<std::uintptr_t>(block) >> 4) % kLayerSlots;
    for (std::size_t i = 0; i < kLayerSlots; ++i) {
        LayerSlot& slot = g_layers[(start + i) % kLayerSlots];
        const void* const have = slot.key.load(std::memory_order_acquire);
        if (have == block) {
            return &slot;
        }
        if (have == nullptr) {
            if (!insert) {
                return nullptr;
            }
            const void* expect = nullptr;
            if (slot.key.compare_exchange_strong(expect, block, std::memory_order_acq_rel)) {
                return &slot;
            }
            if (slot.key.load(std::memory_order_acquire) == block) {
                return &slot;
            }
        }
    }
    return nullptr;
}
}

void resetRealLayers()
{
    for (auto& slot : g_layers) {
        slot.layer.store(kNoLayer, std::memory_order_relaxed);
        slot.key.store(nullptr, std::memory_order_release);
    }
}

void noteRealLayer(const void* block, int layer)
{
    if (block == nullptr || layer < 0 || layer >= 32) {
        return;
    }
    LayerSlot* const slot = layerSlot(block, true);
    if (slot == nullptr) {
        return;
    }
    slot->layer.store(static_cast<std::uint8_t>(layer), std::memory_order_relaxed);
}

int realLayer(const void* block)
{
    if (block == nullptr) {
        return -1;
    }
    const LayerSlot* const slot = layerSlot(block, false);
    if (slot == nullptr) {
        return -1;
    }
    const std::uint8_t layer = slot->layer.load(std::memory_order_relaxed);
    return layer == kNoLayer ? -1 : static_cast<int>(layer);
}

bool hasBlockEntity(const void* block)
{
    if (block == nullptr || !memory::isReadable(block, 0x70)) {
        return false;
    }
    const void* const legacy = readPointer(block, 0x68);
    if (legacy == nullptr || !memory::isReadable(legacy, 0x162)) {
        return false;
    }
    std::uint8_t kind = 0;
    if (!safeCopy(&kind, static_cast<const std::uint8_t*>(legacy) + 0x161, 1)) {
        return false;
    }
    return kind != 0;
}

namespace {
constexpr std::ptrdiff_t kLegacyIdGuess = 0x17e;
constexpr std::size_t kLegacyImageBytes = 0x300;
constexpr std::size_t kLegacySearchFrom = 0x100;
constexpr std::size_t kLegacySearchTo = 0x300;

std::atomic<std::ptrdiff_t> g_legacyIdOffset{-1};
std::atomic<int> g_itemIdState{0};

struct KnownLegacyId {
    const char* name;
    std::uint16_t id;
};
constexpr KnownLegacyId kKnownLegacyIds[] = {
    {"minecraft:chest", 54},
    {"minecraft:crafting_table", 58},
    {"minecraft:furnace", 61},
    {"minecraft:stonecutter_block", 452},
    {"minecraft:cartography_table", 455},
    {"minecraft:smithing_table", 457},
    {"minecraft:loom", 459},
};
}

bool calibrateItemIds()
{
    const int state = g_itemIdState.load(std::memory_order_acquire);
    if (state != 0) {
        return state == 1;
    }
    std::vector<std::string> names;
    for (const KnownLegacyId& one : kKnownLegacyIds) {
        names.emplace_back(one.name);
    }
    Table table;
    if (!resolve(names, table)) {
        return false;
    }
    std::vector<std::vector<std::uint8_t>> images;
    std::vector<std::uint16_t> want;
    std::size_t large = 0;
    for (const KnownLegacyId& one : kKnownLegacyIds) {
        const auto found = table.find(one.name);
        if (found == table.end()) {
            continue;
        }
        const void* const legacy = readPointer(found->second, 0x68);
        if (legacy == nullptr) {
            continue;
        }
        std::vector<std::uint8_t> image(kLegacyImageBytes);
        if (!safeCopy(image.data(), legacy, image.size())) {
            continue;
        }
        images.push_back(std::move(image));
        want.push_back(one.id);
        if (one.id > 255) {
            ++large;
        }
    }
    if (images.size() < 3 || large == 0) {
        g_itemIdState.store(2, std::memory_order_release);
        log().warn(L"BlockRegistry: not enough samples to calibrate the icon id ({} samples / "
                   L"{} above 255) - the material list will have no icons",
                   images.size(),
                   large);
        return false;
    }
    std::vector<const std::uint8_t*> pointers;
    for (const auto& image : images) {
        pointers.push_back(image.data());
    }
    bool guessOk = true;
    constexpr auto kGuessAt = static_cast<std::size_t>(kLegacyIdGuess);
    for (std::size_t i = 0; i < images.size() && guessOk; ++i) {
        const std::uint16_t got = static_cast<std::uint16_t>(
            images[i][kGuessAt] | (images[i][kGuessAt + 1] << 8));
        guessOk = (got == want[i]);
    }
    const std::ptrdiff_t offset =
        guessOk ? kLegacyIdGuess
                : itemicon::findUniqueU16Offset(pointers.data(), want.data(), pointers.size(),
                                                kLegacyImageBytes, kLegacySearchFrom,
                                                kLegacySearchTo, 2);
    if (offset < 0) {
        g_itemIdState.store(2, std::memory_order_release);
        log().warn(L"BlockRegistry: could not pin down the icon id field ({} samples / result "
                   L"{}: -1 = nothing matched, -2 = more than one) - the material list will "
                   L"have no icons",
                   images.size(),
                   offset);
        return false;
    }
    g_legacyIdOffset.store(offset, std::memory_order_relaxed);
    g_itemIdState.store(1, std::memory_order_release);
    return true;
}

namespace {

std::atomic<std::ptrdiff_t> g_registryTlsAt{-1};

constexpr std::ptrdiff_t kItemMaxStackAt = 0xa8;
constexpr std::ptrdiff_t kItemNameAt = 0x128;
constexpr std::ptrdiff_t kCounterObject = 0x00;
constexpr std::ptrdiff_t kCounterWeak = 0x0c;

constexpr std::size_t kItemListBeginAt = 45;
constexpr std::size_t kItemListEndAt = 49;
constexpr std::size_t kMaxItems = 20000;

constexpr char kTlsPattern[] =
    "8B 05 ?? ?? ?? ?? 65 48 8B 14 25 58 00 00 00 48 8B 04 C2 48 8B 80 ?? ?? ?? ?? F0 FF 41 0C";
constexpr std::size_t kTlsDispAt = 22;

bool tlsIndexOf(std::uint32_t& out)
{
    out = 0;
    auto* const base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    if (base == nullptr || !memory::isReadable(base, sizeof(IMAGE_DOS_HEADER))) {
        return false;
    }
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!memory::isReadable(nt, sizeof(IMAGE_NT_HEADERS64))
        || nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (dir.VirtualAddress == 0 || dir.Size < sizeof(IMAGE_TLS_DIRECTORY64)) {
        return false;
    }
    const auto* const tls =
        reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(base + dir.VirtualAddress);
    if (!memory::isReadable(tls, sizeof(IMAGE_TLS_DIRECTORY64))) {
        return false;
    }
    const auto* const at = reinterpret_cast<const std::uint32_t*>(tls->AddressOfIndex);
    if (at == nullptr || !memory::isReadable(at, sizeof(std::uint32_t))) {
        return false;
    }
    out = *at;
    return true;
}

void* itemRegistryPtr()
{
    std::ptrdiff_t at = g_registryTlsAt.load(std::memory_order_acquire);
    if (at == -1) {
        at = -2;
        const ScanHit hit = scanMainModule(kTlsPattern);
        if (hit.address != nullptr) {
            std::int32_t disp = 0;
            std::memcpy(&disp, reinterpret_cast<const unsigned char*>(hit.address) + kTlsDispAt,
                        sizeof(disp));
            if (disp > 0 && disp < (1 << 20)) {
                at = disp;
            }
        }
        g_registryTlsAt.store(at, std::memory_order_release);
    }
    if (at < 0) {
        return nullptr;
    }
    std::uint32_t index = 0;
    if (!tlsIndexOf(index)) {
        return nullptr;
    }
    auto** const array = reinterpret_cast<void**>(__readgsqword(0x58));
    if (array == nullptr || !memory::isReadable(array + index, sizeof(void*))) {
        return nullptr;
    }
    void* const blockPtr = array[index];
    if (blockPtr == nullptr
        || !memory::isReadable(static_cast<unsigned char*>(blockPtr) + at, sizeof(void*))) {
        return nullptr;
    }
    void* registry = nullptr;
    std::memcpy(&registry, static_cast<unsigned char*>(blockPtr) + at, sizeof(registry));
    return registry;
}

__declspec(noinline) bool readItemMapName(const void* at, char (&out)[128]) noexcept
{
    __try {
        const auto* data = static_cast<const std::uint8_t*>(at);
        const auto size = *reinterpret_cast<const std::size_t*>(data + 0x10);
        const auto capacity = *reinterpret_cast<const std::size_t*>(data + 0x18);
        if (size == 0 || size > 127 || capacity < size) return false;
        const char* chars = capacity <= 15 ? reinterpret_cast<const char*>(data)
                                           : *reinterpret_cast<const char* const*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            const auto ch = static_cast<unsigned char>(chars[i]);
            if (ch < 0x20 || ch >= 0x7f) return false;
            out[i] = static_cast<char>(ch);
        }
        out[size] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using ItemLookupByNameFn = void*(__fastcall*)(void*, void*, int*, const void*);
std::atomic<ItemLookupByNameFn> g_lookupByName{nullptr};

struct StringView {
    const char* data;
    std::size_t size;
};

__declspec(noinline) bool callLookupByName(ItemLookupByNameFn fn, void* registry, void* out,
                                           int* flag, const void* view)
{
    __try {
        fn(registry, out, flag, view);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const void* lookupItemByName(const std::string& name)
{
    ItemLookupByNameFn fn = g_lookupByName.load(std::memory_order_acquire);
    if (fn == nullptr) {
        fn = Scanner::instance().addressAs<ItemLookupByNameFn>(Target::ItemRegistryLookupByName);
        if (fn == nullptr) {
            return nullptr;
        }
        g_lookupByName.store(fn, std::memory_order_release);
    }
    void* const registry = itemRegistryPtr();
    if (registry == nullptr || !memory::isReadable(registry, 0x60) || name.empty()) {
        return nullptr;
    }
    StringView view{name.c_str(), name.size()};
    int flag = 0;
    void* counter = nullptr;
    if (!callLookupByName(fn, registry, &counter, &flag, &view) || counter == nullptr
        || !memory::isReadable(counter, 0x10)) {
        return nullptr;
    }
    void* item = nullptr;
    std::memcpy(&item, static_cast<unsigned char*>(counter) + kCounterObject, sizeof(item));
    if (memory::isWritable(static_cast<unsigned char*>(counter) + kCounterWeak, 4)) {
        _InterlockedDecrement(reinterpret_cast<volatile long*>(
            static_cast<unsigned char*>(counter) + kCounterWeak));
    }
    return item;
}

int stackSizeOfItemName(const std::string& name)
{

    const void* const item = lookupItemByName(name);
    int size = 0;
    if (item != nullptr && memory::isReadable(item, kItemMaxStackAt + 1)) {
        size = *(static_cast<const unsigned char*>(item) + kItemMaxStackAt);

    }
    return (size >= 1 && size <= 64) ? size : 0;
}

std::atomic<int> g_stackState{0};

struct KnownStack {
    const char* name;
    int size;
};
constexpr KnownStack kKnownStacks[] = {
    {"minecraft:stone", 64},
    {"minecraft:chest", 64},
    {"minecraft:bed", 1},
    {"minecraft:cake", 64},
    {"minecraft:undyed_shulker_box", 1},
    {"minecraft:iron_block", 64},
};

bool calibrateStackSizes()
{
    const int state = g_stackState.load(std::memory_order_acquire);
    if (state != 0) {
        return state == 1;
    }
    if (!calibrateItemIds()) {
        return false;
    }
    std::vector<std::string> names;
    for (const KnownStack& one : kKnownStacks) {
        names.emplace_back(one.name);
    }
    Table table;
    if (!resolve(names, table)) {
        return false;
    }
    std::size_t checked = 0;
    std::size_t wrong = 0;
    std::size_t small = 0;
    std::wstring detail;
    for (const KnownStack& one : kKnownStacks) {
        const auto found = table.find(one.name);
        if (found == table.end()) {
            continue;
        }
        const int got = stackSizeOfItemName(one.name);
        if (got == 0) {
            continue;
        }
        ++checked;
        if (one.size != 64) {
            ++small;
        }
        if (got != one.size) {
            ++wrong;
            detail += L" ";
            detail += toUtf16(one.name);
            detail += L"=";
            detail += std::to_wstring(got);
            detail += L"(want ";
            detail += std::to_wstring(one.size);
            detail += L")";
        }
    }
    const bool ok = (checked >= 3 && small >= 1 && wrong == 0);
    g_stackState.store(ok ? 1 : 2, std::memory_order_release);
    if (!ok) {
        notice::failOnce("BlockRegistry.stackSize",
                         std::format(L"BlockRegistry: the stack size cannot be read from the game ({} samples "
                                     L"read / {} not 64 / {} mismatched{}); the material list shows no stacks",
                                     checked, small, wrong, detail),
                         "The material list cannot show stacks: the stack size could not be read from the game");
    }
    return ok;
}

}

std::vector<std::string> registeredItemNames()
{
    std::vector<std::string> names;
    void* const registry = itemRegistryPtr();
    const auto* site = Scanner::instance().address(Target::ItemRegistryItemListSite);
    if (registry == nullptr || site == nullptr || !memory::isReadable(site + kItemListBeginAt, 5))
        return names;
    const auto beginAt = static_cast<std::ptrdiff_t>(site[kItemListBeginAt]);
    const auto endAt = static_cast<std::ptrdiff_t>(site[kItemListEndAt]);
    if (endAt != beginAt + 8 || beginAt % 8 != 0) return names;
    std::uintptr_t begin = 0, end = 0;
    if (!safeCopy(&begin, static_cast<const std::uint8_t*>(registry) + beginAt, sizeof(begin))
        || !safeCopy(&end, static_cast<const std::uint8_t*>(registry) + endAt, sizeof(end))
        || begin == 0 || end < begin || (end - begin) % 8 != 0 || (end - begin) / 8 > kMaxItems) return names;
    const std::size_t count = (end - begin) / 8;
    names.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const void* counter = readPointer(reinterpret_cast<const void*>(begin), static_cast<std::ptrdiff_t>(i * 8));
        const void* item = readPointer(counter, kCounterObject);
        if (item == nullptr) continue;
        char name[128]{};
        if (readItemMapName(static_cast<const std::uint8_t*>(item) + kItemNameAt, name)) names.emplace_back(name);
    }
    return names;
}

bool maxStackSizeOf(const char* name, int& out)
{
    out = 0;
    if (!calibrateStackSizes()) {
        return false;
    }
    int size = 0;
    if (name != nullptr && name[0] != 0) {
        size = stackSizeOfItemName(std::string(name));
    }
    if (size <= 0) {
        return false;
    }
    out = size;
    return true;
}

const void* itemByName(const std::string& name)
{
    return lookupItemByName(name);
}

bool blockItemIdAux(const void* block, std::int32_t& out)
{
    out = 0;
    const std::ptrdiff_t offset = g_legacyIdOffset.load(std::memory_order_relaxed);
    if (offset < 0 || block == nullptr) {
        return false;
    }
    const void* const legacy = readPointer(block, 0x68);
    if (legacy == nullptr) {
        return false;
    }
    std::uint16_t raw = 0;
    if (!safeCopy(&raw, static_cast<const std::uint8_t*>(legacy) + offset, sizeof(raw))) {
        return false;
    }
    if (raw == 0) {
        return false;
    }
    out = itemicon::packIdAux(itemicon::blockItemIdFromLegacyId(raw), 0);
    return true;
}

void setGhostPalette(std::vector<const void*> blocks)
{
    resetRealLayers();

    g_drawPalettePtr.store(nullptr, std::memory_order_seq_cst);
    g_drawPaletteCount.store(0, std::memory_order_relaxed);
    if (waitForDrawReaders()) {
        g_retiredPalettes.clear();
    } else if (!g_drawPalette.empty()) {
        g_retiredPalettes.push_back(std::move(g_drawPalette));
    }
    g_drawPalette = std::move(blocks);
    g_drawPaletteCount.store(g_drawPalette.size(), std::memory_order_relaxed);
    g_drawPalettePtr.store(g_drawPalette.data(), std::memory_order_seq_cst);
}

void setGhostCellBlock(std::int32_t x, std::int32_t y, std::int32_t z, std::size_t entry,
                       int layer)
{
    std::size_t at = 0;
    if (!ghostCellIndex(x, y, z, at)) {
        return;
    }
    if (entry + 1 > 0xFFFFu) {
        return;
    }
    std::vector<std::uint16_t>& target = (layer == 1) ? g_drawCells2 : g_drawCells;
    if (at >= target.size()) {
        return;
    }
    target[at] = static_cast<std::uint16_t>(entry + 1);
}

bool dropGhostCell(std::int32_t x, std::int32_t y, std::int32_t z)
{
    g_drawReaders.fetch_add(1, std::memory_order_seq_cst);
    bool dropped = false;
    auto* const cells = const_cast<std::uint16_t*>(g_drawCellsPtr.load(std::memory_order_seq_cst));
    auto* const cells2 = const_cast<std::uint16_t*>(g_drawCells2Ptr.load(std::memory_order_seq_cst));
    const std::size_t count = g_drawCellsCount.load(std::memory_order_relaxed);
    std::size_t at = 0;
    if (ghostCellIndex(x, y, z, at) && at < count) {
        if (cells != nullptr && cells[at] != 0) {
            cells[at] = 0;
            dropped = true;
        }
        if (cells2 != nullptr && cells2[at] != 0) {
            cells2[at] = 0;
            dropped = true;
        }
    }
    g_drawReaders.fetch_sub(1, std::memory_order_seq_cst);
    return dropped;
}

void setGhostWantCell(std::int32_t x, std::int32_t y, std::int32_t z, std::size_t entry,
                      int layer)
{
    std::size_t at = 0;
    if (!ghostCellIndex(x, y, z, at) || entry + 1 >= kWantAirTag) {
        return;
    }
    std::vector<std::uint16_t>& target = (layer == 1) ? g_wantCells2 : g_wantCells;
    if (at >= target.size()) {
        return;
    }
    target[at] = static_cast<std::uint16_t>(entry + 1);
}

void setGhostWantAir(std::int32_t x, std::int32_t y, std::int32_t z)
{
    std::size_t at = 0;
    if (!ghostCellIndex(x, y, z, at) || at >= g_wantCells.size()) {
        return;
    }
    g_wantCells[at] = kWantAirTag;
}

bool restoreGhostCellFromWant(std::int32_t x, std::int32_t y, std::int32_t z)
{
    if (!g_ghostOn.load(std::memory_order_acquire)) {
        return false;
    }
    g_drawReaders.fetch_add(1, std::memory_order_seq_cst);
    bool back = false;
    do {
        const std::uint16_t* const want = g_wantCellsPtr.load(std::memory_order_seq_cst);
        const std::uint16_t* const want2 = g_wantCells2Ptr.load(std::memory_order_seq_cst);
        auto* const live =
            const_cast<std::uint16_t*>(g_drawCellsPtr.load(std::memory_order_seq_cst));
        auto* const live2 =
            const_cast<std::uint16_t*>(g_drawCells2Ptr.load(std::memory_order_seq_cst));
        const std::size_t count = g_drawCellsCount.load(std::memory_order_relaxed);
        std::size_t at = 0;
        if (count == 0 || !ghostCellIndex(x, y, z, at) || at >= count) {
            break;
        }
        if (want != nullptr && live != nullptr && want[at] != 0 && want[at] != kWantAirTag
            && live[at] != want[at]) {
            live[at] = want[at];
            back = true;
        }
        if (want2 != nullptr && live2 != nullptr && want2[at] != 0 && live2[at] != want2[at]) {
            live2[at] = want2[at];
            back = true;
        }
    } while (false);
    g_drawReaders.fetch_sub(1, std::memory_order_seq_cst);
    return back;
}

std::atomic<std::size_t> g_diffCellChanges{0};

bool setDiffCell(std::int32_t x, std::int32_t y, std::int32_t z, DiffColor color)
{
    if (!g_ghostOn.load(std::memory_order_acquire)) {
        return false;
    }
    g_drawReaders.fetch_add(1, std::memory_order_seq_cst);
    std::uint8_t* const cells = g_diffCellsPtr.load(std::memory_order_seq_cst);
    const std::size_t count = g_drawCellsCount.load(std::memory_order_relaxed);
    std::size_t at = 0;
    bool changed = false;
    if (cells != nullptr && count != 0 && ghostCellIndex(x, y, z, at) && at < count) {
        const auto want = static_cast<std::uint8_t>(color);
        if (cells[at] != want) {
            cells[at] = want;
            changed = true;
            g_diffCellChanges.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_drawReaders.fetch_sub(1, std::memory_order_seq_cst);
    return changed;
}

std::size_t takeDiffCellChanges()
{
    return g_diffCellChanges.exchange(0, std::memory_order_relaxed);
}

void setLayerFilter(bool on, int axis, std::int32_t lo, std::int32_t hi)
{
    if (lo > hi) {
        std::swap(lo, hi);
    }
    g_layerAxis.store((axis >= 0 && axis <= 2) ? axis : 1, std::memory_order_relaxed);
    g_layerLo.store(lo, std::memory_order_relaxed);
    g_layerHi.store(hi, std::memory_order_relaxed);
    g_layerOn.store(on, std::memory_order_release);
}

bool layerShows(std::int32_t x, std::int32_t y, std::int32_t z)
{
    if (!g_layerOn.load(std::memory_order_acquire)) {
        return true;
    }
    const int axis = g_layerAxis.load(std::memory_order_relaxed);
    const std::int32_t v = (axis == 0) ? x : ((axis == 2) ? z : y);
    return v >= g_layerLo.load(std::memory_order_relaxed)
           && v <= g_layerHi.load(std::memory_order_relaxed);
}

DiffColor diffCellAt(std::int32_t x, std::int32_t y, std::int32_t z)
{
    if (!g_ghostOn.load(std::memory_order_acquire)) {
        return DiffColor::None;
    }
    if (!layerShows(x, y, z)) {
        return DiffColor::None;
    }
    std::size_t at = 0;
    if (!ghostCellIndex(x, y, z, at)) {
        return DiffColor::None;
    }
    g_drawReaders.fetch_add(1, std::memory_order_seq_cst);
    DiffColor found = DiffColor::None;
    const std::uint8_t* const cells = g_diffCellsPtr.load(std::memory_order_seq_cst);
    const std::size_t count = g_drawCellsCount.load(std::memory_order_relaxed);
    if (cells != nullptr && count != 0 && at < count) {
        const std::uint8_t raw = cells[at];
        if (raw <= kDiffColorMax) {
            found = static_cast<DiffColor>(raw);
        }
    }
    g_drawReaders.fetch_sub(1, std::memory_order_seq_cst);
    return found;
}

DiffKind diffKindOf(const void* real, const void* want, bool wantAir)
{
    if (real == nullptr) {
        return DiffKind::UnknownWorld;
    }
    const void* const air = airBlock();
    if (air == nullptr) {
        return DiffKind::UnknownWorld;
    }
    if (wantAir) {
        return real != air ? DiffKind::Extra : DiffKind::Match;
    }
    if (real == air) {
        return DiffKind::Missing;
    }
    if (want == nullptr) {
        return DiffKind::UnknownWant;
    }
    if (real == want) {
        return DiffKind::Match;
    }
    const void* const legacyReal = legacyOfFast(real);
    const void* const legacyWant = legacyOfFast(want);
    if (legacyReal != nullptr && legacyReal == legacyWant) {
        return DiffKind::State;
    }
    return DiffKind::Wrong;
}

DiffKind diffKindOf(const void* real, const void* want, bool wantAir,
                    const void* realExtra, bool realExtraKnown, bool wantWater)
{
    const DiffKind base = diffKindOf(real, want, wantAir);
    if (base != DiffKind::Match || wantAir || !realExtraKnown) {
        return base;
    }
    const void* const air = airBlock();
    const bool realWater = (realExtra != nullptr && realExtra != air);
    return realWater == wantWater ? DiffKind::Match : DiffKind::State;
}

DiffColor colorOfDiffKind(DiffKind kind)
{
    switch (kind) {
    case DiffKind::Missing:
        return DiffColor::Missing;
    case DiffKind::Wrong:
        return DiffColor::Wrong;
    case DiffKind::State:
        return DiffColor::State;
    case DiffKind::Extra:
        return DiffColor::Extra;
    case DiffKind::Match:
    case DiffKind::UnknownWorld:
    case DiffKind::UnknownWant:
        break;
    }
    return DiffColor::None;
}

bool noteWorldBlockAt(std::int32_t x, std::int32_t y, std::int32_t z, const void* real,
                      const void* realExtra, bool realExtraKnown)
{
    if (!g_ghostOn.load(std::memory_order_acquire) || real == nullptr) {
        return false;
    }
    g_drawReaders.fetch_add(1, std::memory_order_seq_cst);
    bool changed = false;
    do {
        const std::uint16_t* const want = g_wantCellsPtr.load(std::memory_order_seq_cst);
        const std::uint16_t* const want2 = g_wantCells2Ptr.load(std::memory_order_seq_cst);
        const std::size_t count = g_drawCellsCount.load(std::memory_order_relaxed);
        std::size_t at = 0;
        if (want == nullptr || count == 0 || !ghostCellIndex(x, y, z, at) || at >= count) {
            break;
        }
        const std::uint16_t tag = want[at];
        if (tag == 0) {
            break;
        }
        const void* wantBlock = nullptr;
        if (tag != kWantAirTag) {
            const void* const* const palette =
                g_drawPalettePtr.load(std::memory_order_seq_cst);
            const std::size_t size = g_drawPaletteCount.load(std::memory_order_relaxed);
            if (palette != nullptr && tag - 1u < size) {
                wantBlock = palette[tag - 1u];
            }
            if (wantBlock == nullptr) {
                break;
            }
        }
        const bool wantWater = (want2 != nullptr && want2[at] != 0);
        changed = setDiffCell(x, y, z,
                              colorOfDiffKind(diffKindOf(real, wantBlock,
                                                         tag == kWantAirTag, realExtra,
                                                         realExtraKnown, wantWater)));
    } while (false);
    g_drawReaders.fetch_sub(1, std::memory_order_seq_cst);
    return changed;
}

std::size_t collectDiffBoxes(std::vector<DiffBox>& out)
{
    out.clear();
    if (!g_ghostOn.load(std::memory_order_acquire)) {
        return 0;
    }
    g_drawReaders.fetch_add(1, std::memory_order_seq_cst);
    const std::uint8_t* const cells = g_diffCellsPtr.load(std::memory_order_seq_cst);
    const std::size_t count = g_drawCellsCount.load(std::memory_order_relaxed);
    const std::int32_t minX = g_ghostMin[0];
    const std::int32_t minY = g_ghostMin[1];
    const std::int32_t minZ = g_ghostMin[2];
    const std::size_t sy = static_cast<std::size_t>(g_ghostMax[1] - minY);
    const std::size_t sz = static_cast<std::size_t>(g_ghostMax[2] - minZ);
    const std::size_t plane = sy * sz;
    const std::size_t sx = (cells != nullptr && plane != 0) ? count / plane : 0;
    if (sx != 0) {
        std::vector<std::uint8_t> showX(sx, 1);
        std::vector<std::uint8_t> showY(sy, 1);
        std::vector<std::uint8_t> showZ(sz, 1);
        if (g_layerOn.load(std::memory_order_acquire)) {
            const int layerAxis = g_layerAxis.load(std::memory_order_relaxed);
            const std::int32_t layerLo = g_layerLo.load(std::memory_order_relaxed);
            const std::int32_t layerHi = g_layerHi.load(std::memory_order_relaxed);
            std::vector<std::uint8_t>& axis =
                (layerAxis == 0) ? showX : ((layerAxis == 2) ? showZ : showY);
            const std::int32_t base = (layerAxis == 0) ? minX : ((layerAxis == 2) ? minZ : minY);
            for (std::size_t i = 0; i < axis.size(); ++i) {
                const std::int32_t v = base + static_cast<std::int32_t>(i);
                axis[i] = (v >= layerLo && v <= layerHi) ? 1 : 0;
            }
        }
        const auto lit = [](std::uint8_t v) {
            return static_cast<unsigned>(v) - 1u < static_cast<unsigned>(kDiffColorMax);
        };
        std::size_t litCount = 0;
        for (std::size_t dx = 0, at = 0; dx < sx; ++dx) {
            for (std::size_t dy = 0; dy < sy; ++dy) {
                const bool rowShown = showX[dx] != 0 && showY[dy] != 0;
                for (std::size_t dz = 0; dz < sz; ++dz, ++at) {
                    litCount += (lit(cells[at]) && rowShown && showZ[dz] != 0) ? 1 : 0;
                }
            }
        }
        out.reserve(litCount);
        for (std::size_t dx = 0, at = 0; dx < sx; ++dx) {
            for (std::size_t dy = 0; dy < sy; ++dy) {
                const bool rowShown = showX[dx] != 0 && showY[dy] != 0;
                for (std::size_t dz = 0; dz < sz; ++dz, ++at) {
                    const std::uint8_t raw = cells[at];
                    if (!lit(raw) || !rowShown || showZ[dz] == 0) {
                        continue;
                    }
                    std::uint8_t covered = 0;
                    const bool columnShown = showX[dx] != 0 && showZ[dz] != 0;
                    if (dy > 0 && lit(cells[at - sz]) && showY[dy - 1] != 0 && columnShown) {
                        covered |= 0x01;
                    }
                    if (dy + 1 < sy && lit(cells[at + sz]) && showY[dy + 1] != 0 && columnShown) {
                        covered |= 0x02;
                    }
                    if (dz > 0 && lit(cells[at - 1]) && showZ[dz - 1] != 0 && rowShown) {
                        covered |= 0x04;
                    }
                    if (dz + 1 < sz && lit(cells[at + 1]) && showZ[dz + 1] != 0 && rowShown) {
                        covered |= 0x08;
                    }
                    if (dx > 0 && lit(cells[at - plane]) && showX[dx - 1] != 0 && showY[dy] != 0
                        && showZ[dz] != 0) {
                        covered |= 0x10;
                    }
                    if (dx + 1 < sx && lit(cells[at + plane]) && showX[dx + 1] != 0
                        && showY[dy] != 0 && showZ[dz] != 0) {
                        covered |= 0x20;
                    }
                    DiffBox box;
                    box.x = minX + static_cast<std::int32_t>(dx);
                    box.y = minY + static_cast<std::int32_t>(dy);
                    box.z = minZ + static_cast<std::int32_t>(dz);
                    box.color = static_cast<DiffColor>(raw);
                    box.covered = covered;
                    box.hidden = covered == 0x3F;
                    out.push_back(box);
                }
            }
        }
    }
    g_drawReaders.fetch_sub(1, std::memory_order_seq_cst);
    return out.size();
}

namespace {
constexpr std::size_t kLegacyCacheSlots = 64;
struct LegacyCacheSlot {
    std::atomic<const void*> key{nullptr};
    std::atomic<const void*> value{nullptr};
    std::atomic_flag writing = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> version{0};
};
LegacyCacheSlot g_legacyCache[kLegacyCacheSlots];
}

std::size_t appendStateVariants(const void* block, std::vector<const void*>& out)
{
    if (block == nullptr || !memory::isReadable(block, 0x70)) {
        return 0;
    }
    const void* const legacy = readPointer(block, 0x68);
    if (legacy == nullptr || !memory::isReadable(legacy, 0x238)) {
        return 0;
    }
    const void* const begin = readPointer(legacy, 0x228);
    const void* const end = readPointer(legacy, 0x230);
    if (begin == nullptr || end <= begin) {
        return 0;
    }
    const std::size_t bytes = static_cast<std::size_t>(
        static_cast<const std::uint8_t*>(end) - static_cast<const std::uint8_t*>(begin));
    constexpr std::size_t kMaxStates = 4096;
    if ((bytes % sizeof(void*)) != 0 || bytes / sizeof(void*) > kMaxStates) {
        return 0;
    }
    const std::size_t count = bytes / sizeof(void*);
    if (!memory::isReadable(begin, bytes)) {
        return 0;
    }
    std::size_t added = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const void* one = nullptr;
        if (!safeCopy(&one, static_cast<const std::uint8_t*>(begin) + i * sizeof(void*),
                      sizeof(one))
            || one == nullptr) {
            continue;
        }
        out.push_back(one);
        ++added;
    }
    return added;
}

const void* legacyOfFast(const void* block)
{
    if (block == nullptr) {
        return nullptr;
    }
    const std::size_t at =
        (reinterpret_cast<std::uintptr_t>(block) >> 4) % kLegacyCacheSlots;
    auto& slot = g_legacyCache[at];
    const auto version = slot.version.load(std::memory_order_seq_cst);
    if ((version & 1) == 0 && slot.key.load(std::memory_order_seq_cst) == block) {
        const void* const value = slot.value.load(std::memory_order_seq_cst);
        if (slot.key.load(std::memory_order_seq_cst) == block
            && slot.version.load(std::memory_order_seq_cst) == version) return value;
    }
    const void* answer = nullptr;
    if (memory::isReadable(block, 0x70)) {
        answer = readPointer(block, 0x68);
    }
    if (!slot.writing.test_and_set(std::memory_order_acquire)) {
        slot.version.fetch_add(1, std::memory_order_seq_cst);
        slot.value.store(answer, std::memory_order_seq_cst);
        slot.key.store(block, std::memory_order_seq_cst);
        slot.version.fetch_add(1, std::memory_order_seq_cst);
        slot.writing.clear(std::memory_order_release);
    }
    return answer;
}

bool ghostSubChunkOccupied(std::int32_t baseX, std::int32_t baseY, std::int32_t baseZ)
{
    if (!g_ghostOn.load(std::memory_order_acquire)) {
        return false;
    }
    const std::int32_t x0 = std::max(baseX, g_ghostMin[0]);
    const std::int32_t y0 = std::max(baseY, g_ghostMin[1]);
    const std::int32_t z0 = std::max(baseZ, g_ghostMin[2]);
    const std::int32_t x1 = std::min(baseX + 16, g_ghostMax[0]);
    const std::int32_t y1 = std::min(baseY + 16, g_ghostMax[1]);
    const std::int32_t z1 = std::min(baseZ + 16, g_ghostMax[2]);
    if (x0 >= x1 || y0 >= y1 || z0 >= z1) {
        return false;
    }
    g_drawReaders.fetch_add(1, std::memory_order_seq_cst);
    bool found = false;
    const std::uint16_t* const cells = g_drawCellsPtr.load(std::memory_order_seq_cst);
    const std::size_t count = g_drawCellsCount.load(std::memory_order_relaxed);
    if (cells != nullptr && count != 0) {
        for (std::int32_t x = x0; x < x1 && !found; ++x) {
            for (std::int32_t y = y0; y < y1 && !found; ++y) {
                for (std::int32_t z = z0; z < z1; ++z) {
                    std::size_t at = 0;
                    if (!ghostCellIndex(x, y, z, at) || at >= count) {
                        continue;
                    }
                    if (cells[at] != 0) {
                        found = true;
                        break;
                    }
                }
            }
        }
    }
    g_drawReaders.fetch_sub(1, std::memory_order_seq_cst);
    return found;
}

bool ghostBoxTouchesSubChunk(std::int32_t baseX, std::int32_t baseY, std::int32_t baseZ)
{
    if (!g_ghostOn.load(std::memory_order_acquire)) {
        return false;
    }
    return baseX + 16 > g_ghostMin[0] && baseX < g_ghostMax[0]
           && baseY + 16 > g_ghostMin[1] && baseY < g_ghostMax[1]
           && baseZ + 16 > g_ghostMin[2] && baseZ < g_ghostMax[2];
}

bool ghostInside(std::int32_t x, std::int32_t y, std::int32_t z)
{
    if (!g_ghostOn.load(std::memory_order_acquire)) {
        return false;
    }
    std::size_t at = 0;
    return ghostCellIndex(x, y, z, at);
}

const void* ghostBlockAt(std::int32_t x, std::int32_t y, std::int32_t z, int layer)
{
    if (!g_ghostOn.load(std::memory_order_acquire)) {
        return nullptr;
    }
    std::size_t at = 0;
    if (!ghostCellIndex(x, y, z, at)) {
        return nullptr;
    }
    if (!layerShows(x, y, z)) {
        return nullptr;
    }
    g_drawReaders.fetch_add(1, std::memory_order_seq_cst);
    const void* found = nullptr;
    do {
        const std::uint16_t* const cells =
            (layer == 1) ? g_drawCells2Ptr.load(std::memory_order_seq_cst)
                         : g_drawCellsPtr.load(std::memory_order_seq_cst);
        const std::size_t count = g_drawCellsCount.load(std::memory_order_relaxed);
        if (cells == nullptr || count == 0 || at >= count) {
            break;
        }
        const std::uint16_t tag = cells[at];
        if (tag == 0) {
            break;
        }
        const void* const* const palette = g_drawPalettePtr.load(std::memory_order_seq_cst);
        const std::size_t size = g_drawPaletteCount.load(std::memory_order_relaxed);
        if (palette == nullptr || tag - 1u >= size) {
            break;
        }
        found = palette[tag - 1u];
    } while (false);
    g_drawReaders.fetch_sub(1, std::memory_order_seq_cst);
    return found;
}

const void* wantBlockAt(std::int32_t x, std::int32_t y, std::int32_t z)
{
    if (!g_ghostOn.load(std::memory_order_acquire)) {
        return nullptr;
    }
    std::size_t at = 0;
    if (!ghostCellIndex(x, y, z, at) || !layerShows(x, y, z)) {
        return nullptr;
    }
    g_drawReaders.fetch_add(1, std::memory_order_seq_cst);
    const void* found = nullptr;
    do {
        const std::uint16_t* const want = g_wantCellsPtr.load(std::memory_order_seq_cst);
        const std::size_t count = g_drawCellsCount.load(std::memory_order_relaxed);
        if (want == nullptr || count == 0 || at >= count) {
            break;
        }
        const std::uint16_t tag = want[at];
        if (tag == 0 || tag == kWantAirTag) {
            break;
        }
        const void* const* const palette = g_drawPalettePtr.load(std::memory_order_seq_cst);
        const std::size_t size = g_drawPaletteCount.load(std::memory_order_relaxed);
        if (palette == nullptr || tag - 1u >= size) {
            break;
        }
        found = palette[tag - 1u];
    } while (false);
    g_drawReaders.fetch_sub(1, std::memory_order_seq_cst);
    return found;
}

namespace {
std::atomic<bool> g_ghostOverMismatch{false};
}

void setGhostOverMismatch(bool on)
{
    g_ghostOverMismatch.store(on, std::memory_order_relaxed);
}

bool ghostOverMismatchOn()
{
    return g_ghostOverMismatch.load(std::memory_order_relaxed);
}

bool ghostCell(std::int32_t x, std::int32_t y, std::int32_t z)
{
    if (!g_ghostOn.load(std::memory_order_acquire)) {
        return false;
    }
    std::size_t at = 0;
    if (!ghostCellIndex(x, y, z, at)) {
        return false;
    }
    g_drawReaders.fetch_add(1, std::memory_order_seq_cst);
    bool found = false;
    do {
        const std::uint16_t* const cells = g_drawCellsPtr.load(std::memory_order_seq_cst);
        const std::size_t count = g_drawCellsCount.load(std::memory_order_relaxed);
        if (cells == nullptr || count == 0 || at >= count) {
            break;
        }
        found = cells[at] != 0;
    } while (false);
    g_drawReaders.fetch_sub(1, std::memory_order_seq_cst);
    return found;
}

void setAirBlock(const void* air)
{
    g_air.store(air, std::memory_order_release);
}

void dropWorldBlocks()
{
    g_ghostOn.store(false, std::memory_order_release);
    g_drawPalettePtr.store(nullptr, std::memory_order_seq_cst);
    g_drawPaletteCount.store(0, std::memory_order_relaxed);
    g_air.store(nullptr, std::memory_order_release);
    g_worldDropped.store(true, std::memory_order_release);
}

bool takeWorldDropped()
{
    return g_worldDropped.exchange(false, std::memory_order_acq_rel);
}

const void* airBlock()
{
    return g_air.load(std::memory_order_acquire);
}

void waitUntilIdle()
{
    g_ghostOn.store(false, std::memory_order_release);
    g_drawCellsPtr.store(nullptr, std::memory_order_seq_cst);
    g_drawCells2Ptr.store(nullptr, std::memory_order_seq_cst);
    g_drawPalettePtr.store(nullptr, std::memory_order_seq_cst);
    g_drawCellsCount.store(0, std::memory_order_relaxed);
    waitForDrawReaders();
    Sleep(50);
}

void setSimThread(unsigned long id)
{
    g_simThread.store(id, std::memory_order_relaxed);
}

unsigned long simThread()
{
    return g_simThread.load(std::memory_order_relaxed);
}

static std::size_t meshSeatsTaken()
{
    const std::size_t taken = g_meshThreadCount.load(std::memory_order_acquire);
    return taken > kMeshThreadLimit ? kMeshThreadLimit : taken;
}

bool isMeshThread(unsigned long id)
{
    if (id == 0) {
        return false;
    }
    const std::size_t taken = meshSeatsTaken();
    for (std::size_t i = 0; i < taken; ++i) {
        if (g_meshThreads[i].load(std::memory_order_acquire) == id) {
            return true;
        }
    }
    return false;
}

void noteMeshThread(unsigned long id)
{
    if (id == 0 || id == g_simThread.load(std::memory_order_relaxed)) {
        return;
    }
    if (isMeshThread(id)) {
        return;
    }
    if (g_meshThreadCount.load(std::memory_order_acquire) >= kMeshThreadLimit) {
        return;
    }
    const std::size_t at = g_meshThreadCount.fetch_add(1, std::memory_order_acq_rel);
    if (at >= kMeshThreadLimit) {
        return;
    }
    g_meshThreads[at].store(id, std::memory_order_release);
}

std::size_t meshThreadCount()
{
    std::size_t live = 0;
    const std::size_t taken = meshSeatsTaken();
    for (std::size_t i = 0; i < taken; ++i) {
        if (g_meshThreads[i].load(std::memory_order_acquire) != 0) {
            ++live;
        }
    }
    return live;
}

}
