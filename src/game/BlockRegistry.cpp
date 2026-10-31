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

constexpr std::size_t kLeaDisp = 39 + 3;

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

const void* transformedBlock(const void* block, int quarters, int mirror, bool* ok)
{
    if (ok != nullptr) {
        *ok = false;
    }
    const int turn = ((quarters % 4) + 4) % 4;
    if (block == nullptr || (turn == 0 && mirror == 0)) {
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
    const void* const turned = callTransformBlock(fn, block, static_cast<unsigned>(turn),
                                                 static_cast<unsigned>(mirror), faulted);
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
    nbt::Tag tag = nbt::Tag::End;
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

nbt::Tag stateValueTagType(const void* desc, unsigned int value)
{
    if (desc == nullptr || !memory::isReadable(desc, sizeof(void*))) {
        return nbt::Tag::End;
    }
    const void* vtable = nullptr;
    StateValueNameFn fn = nullptr;
    if (!safeCopy(&vtable, desc, sizeof(vtable)) || vtable == nullptr
        || !safeCopy(&fn, static_cast<const std::uint8_t*>(vtable) + 8, sizeof(fn)) || fn == nullptr) {
        return nbt::Tag::End;
    }
    alignas(8) std::uint8_t holder[0x20] = {};
    fn(desc, holder, value);
    const void* tag = nullptr;
    std::memcpy(&tag, holder, sizeof(tag));
    const void* tagVtable = nullptr;
    const void* getId = nullptr;
    std::uint8_t code[3] = {};
    if (tag == nullptr || !safeCopy(&tagVtable, tag, sizeof(tagVtable)) || tagVtable == nullptr
        || !safeCopy(&getId, static_cast<const std::uint8_t*>(tagVtable) + 0x28, sizeof(getId)) || getId == nullptr
        || !safeCopy(code, getId, sizeof(code)) || code[0] != 0xB0 || code[2] != 0xC3) {
        return nbt::Tag::End;
    }
    switch (code[1]) {
    case 1: return nbt::Tag::Byte;
    case 3: return nbt::Tag::Int;
    case 8: return nbt::Tag::String;
    default: return nbt::Tag::End;
    }
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
std::vector<StateSlot> readStateSlots(const void* legacy, bool* complete = nullptr)
{
    std::vector<StateSlot> out;
    bool valid=true;
    if (complete) *complete=false;
    const void* const head = readPointer(legacy, 0x1d8);
    if (head == nullptr || !memory::isReadable(head, 0x20)) {
        return out;
    }
    std::vector<const void*> stack{readPointer(head, 0x08)};
    for (int guard = 0; !stack.empty() && guard < 512 && out.size() < 64; ++guard) {
        const void* const node = stack.back();
        stack.pop_back();
        if (node == head) continue;
        if (node == nullptr || !memory::isReadable(node, 0x40)) {
            valid=false;
            continue;
        }
        std::uint8_t raw[0x40] = {};
        if (!safeCopy(raw, node, sizeof(raw))) {
            valid=false;
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
                slot.tag = stateValueTagType(slot.desc, 0);
                if (!slot.names.empty() && slot.tag != nbt::Tag::String) slot.tag = nbt::Tag::End;
                out.push_back(std::move(slot));
            } else valid=false;
        }
        stack.push_back(readPointer(node, 0x00));
        stack.push_back(readPointer(node, 0x10));
    }
    if (complete) *complete=valid && stack.empty();
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
void forgetLegacyCache();
std::unordered_map<const void*, std::string> g_legacyNames;
bool g_legacyNamesReady = false;
struct CachedStateSlots {
    std::vector<StateSlot> slots;
    bool complete=false;
};
std::unordered_map<const void*, CachedStateSlots> g_legacySlots;
std::vector<const void*> g_forgottenLegacies;

const CachedStateSlots& cachedStateSlots(const void* legacy)
{
    const auto [found,inserted]=g_legacySlots.try_emplace(legacy);
    if (inserted) found->second.slots=readStateSlots(legacy,&found->second.complete);
    return found->second;
}

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
        if (!g_forgottenLegacies.empty()) {
            std::size_t same = 0;
            for (const void* key : g_forgottenLegacies) same += g_legacyNames.count(key);
            static unsigned said = 0;
            if (said++ < 4)
                log().info(L"Blocks: rebuilt the block name table for the new world ({} kinds, {} of {} old BlockLegacy addresses are still there)",
                           g_legacyNames.size(), same, g_forgottenLegacies.size());
            g_forgottenLegacies.clear();
        }
    }
    const auto found = g_legacyNames.find(legacy);
    if (found == g_legacyNames.end()) {
        return false;
    }
    std::snprintf(out, cap, "%s", found->second.c_str());
    return true;
}

void forgetWorldLookups()
{
    {
        std::lock_guard<std::mutex> lock(g_blockLookupMutex);
        if (g_legacyNamesReady) {
            g_forgottenLegacies.clear();
            for (const auto& [key, name] : g_legacyNames) g_forgottenLegacies.push_back(key);
        }
        g_legacyNames.clear();
        g_legacyNamesReady = false;
        g_legacySlots.clear();
    }
    forgetLegacyCache();
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
    const auto& slots=cachedStateSlots(legacy);
    int rows = 0;
    for (const StateSlot& slot : slots.slots) {
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

bool typedStatesOfBlock(const void* block,std::vector<structure::StateValue>& out)
{
    out.clear();
    if (!block || !memory::isReadable(block,0x128)) return false;
    const void* legacy=readPointer(block,kBlockLegacyAt);
    if (!legacy || !memory::isReadable(legacy,0x248)) return false;
    std::uint16_t index=0;
    if (!safeCopy(&index,static_cast<const std::uint8_t*>(block)+0x120,sizeof(index))) return false;
    std::lock_guard lock(g_blockLookupMutex);
    const auto& slots=cachedStateSlots(legacy);
    if (!slots.complete) return false;
    for (const auto& slot : slots.slots) {
        const auto value=(index&slot.mask)>>slot.shift;
        if (slot.tag==nbt::Tag::End || value>=slot.values) { out.clear(); return false; }
        structure::StateValue state;
        state.name=slot.name; state.tag=slot.tag;
        if (slot.tag==nbt::Tag::String) {
            if (value>=slot.names.size()) { out.clear(); return false; }
            state.text=slot.names[value];
        } else state.number=value;
        out.push_back(std::move(state));
    }
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b) { return a.name<b.name; });
    return true;
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
    constexpr std::ptrdiff_t kMaterialFlagsAt = 0x29;
    const void* const legacy = readPointer(block, kBlockLegacyAt);
    std::uint8_t flags = 0;
    if (!memory::plausiblePointer(legacy)
        || !safeCopy(&flags, static_cast<const std::byte*>(legacy) + kMaterialFlagsAt, sizeof flags)) return false;
    liquid = (flags & 0x02) != 0;
    blocksMotion = (flags & 0x04) != 0;
    return true;
}

namespace {
using UseEventTypeFn = std::uint8_t(__fastcall*)(const void*);

__declspec(noinline) bool useEventTypeGuarded(UseEventTypeFn fn, const void* handler, std::uint8_t& out) noexcept
{
    __try {
        out = fn(handler);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}

int usedByClick(const void* block)
{
    static const auto site = [] {
        struct Site { std::int32_t tableAt = 0; std::uint8_t event = 0; bool ok = false; } out;
        const auto* at = Scanner::instance().address(Target::BlockUseListenerSite);
        if (at == nullptr || !safeCopy(&out.tableAt, at + 1, sizeof(out.tableAt)) || !safeCopy(&out.event, at + 10, 1)) return out;
        out.ok = out.tableAt > 0 && out.tableAt < 0x1000 && out.tableAt % 8 == 0;
        if (!out.ok) log().warn(L"Block use listener: unexpected table offset {:#x}", out.tableAt);
        return out;
    }();
    if (!site.ok || block == nullptr) return -1;
    const void* const legacy = readPointer(block, kBlockLegacyAt);
    if (legacy == nullptr) return -1;
    const auto* const table = static_cast<const std::uint8_t*>(legacy) + site.tableAt;
    std::uintptr_t range[2]{};
    if (!safeCopy(range, table + 8, sizeof(range)) || range[1] < range[0] || (range[1] - range[0]) % 8 != 0
        || (range[1] - range[0]) / 8 > 64) return -1;
    for (auto at = range[0]; at < range[1]; at += 8) {
        const void* const handler = readPointer(reinterpret_cast<const void*>(at), 0);
        const void* const fn = readPointer(readPointer(handler, 0), 8);
        if (fn == nullptr || !memory::inGameModule(fn)) return -1;
        std::uint8_t event = 0;
        if (!useEventTypeGuarded(reinterpret_cast<UseEventTypeFn>(const_cast<void*>(fn)), handler, event)) return -1;
        if (event == site.event) return 1;
    }
    return 0;
}

namespace {
struct OptionalHit {
    float point[3];
    bool has;
};
using BlockUseFn = bool(__fastcall*)(const void* block, void* player, const std::int32_t* pos, std::uint8_t face,
                                     std::uint8_t source, const OptionalHit* hit);

__declspec(noinline) bool blockUseGuarded(BlockUseFn fn, const void* block, void* player, const std::int32_t* pos,
                                          std::uint8_t face, const OptionalHit* hit, bool& out) noexcept
{
    __try {
        out = fn(block, player, pos, face, 0, hit);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}

int clickUses(void* gameMode, const void* block, const std::int32_t pos[3], int face, const float click[3])
{
    const auto fn = reinterpret_cast<BlockUseFn>(Scanner::instance().address(Target::BlockUse));
    void* const player = const_cast<void*>(readPointer(gameMode, 8));
    if (fn == nullptr || player == nullptr || block == nullptr || pos == nullptr || click == nullptr || face < 0 || face > 5) {
        return -1;
    }
    OptionalHit hit{};
    for (int i = 0; i < 3; ++i) hit.point[i] = static_cast<float>(pos[i]) + click[i];
    hit.has = true;
    const std::int32_t at[3]{pos[0], pos[1], pos[2]};
    bool used = false;
    if (!blockUseGuarded(fn, block, player, at, static_cast<std::uint8_t>(face), &hit, used)) {
        static std::atomic<int> said{0};
        if (said.fetch_add(1, std::memory_order_relaxed) < 4) log().warn(L"Blocks: Block::use faulted at {}, {}, {}", pos[0], pos[1], pos[2]);
        return -1;
    }
    return used ? 1 : 0;
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
std::atomic<const void*> g_air{nullptr};
std::atomic<unsigned long> g_simThread{0};
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

namespace {
constexpr std::size_t kLegacyCacheSlots = 64;
struct LegacyCacheSlot {
    std::atomic<const void*> key{nullptr};
    std::atomic<const void*> value{nullptr};
    std::atomic_flag writing = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> version{0};
};
LegacyCacheSlot g_legacyCache[kLegacyCacheSlots];

void forgetLegacyCache()
{
    for (auto& slot : g_legacyCache) {
        while (slot.writing.test_and_set(std::memory_order_acquire)) {}
        slot.version.fetch_add(1, std::memory_order_seq_cst);
        slot.key.store(nullptr, std::memory_order_seq_cst);
        slot.value.store(nullptr, std::memory_order_seq_cst);
        slot.version.fetch_add(1, std::memory_order_seq_cst);
        slot.writing.clear(std::memory_order_release);
    }
}
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

void setAirBlock(const void* air)
{
    g_air.store(air, std::memory_order_release);
}

const void* airBlock()
{
    return g_air.load(std::memory_order_acquire);
}

void setSimThread(unsigned long id)
{
    g_simThread.store(id, std::memory_order_relaxed);
}

unsigned long simThread()
{
    return g_simThread.load(std::memory_order_relaxed);
}

}
