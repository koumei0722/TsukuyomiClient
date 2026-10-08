#include "game/ExtendedEnchantLevel.h"

#include "config/Config.h"
#include "config/WriteSwitches.h"
#include "core/Logger.h"
#include "hooks/HookManager.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tsukuyomi::extendedenchantlevel {

namespace {

constexpr char kSection[] = "ExtendedEnchantLevel";
constexpr int kMaxLevel = 32767;
constexpr int kLogLimit = 20;

constexpr std::size_t kRangeCallRel = 0x1f;
constexpr std::size_t kRangeCallNext = 0x23;
constexpr std::size_t kCanEnchantCallRel = 0x14;
constexpr std::size_t kCanEnchantCallNext = 0x18;
constexpr std::size_t kCheckCallRel = 0x22;
constexpr std::size_t kCheckCallNext = 0x26;

constexpr std::size_t kGroupVectors = 0x08;
constexpr std::size_t kGroupStride = 0x18;
constexpr std::uint64_t kGroupCount = 3;
constexpr std::size_t kEntrySize = 8;
constexpr std::size_t kEntryLevel = 4;
constexpr std::uint8_t kTypeCount = 0x2a;
constexpr std::uint8_t kResultModify = 2;
constexpr std::uint8_t kResultAppend = 3;
constexpr std::size_t kResultSize = 0x18;
constexpr std::size_t kResultIndex = 0x08;
constexpr std::size_t kResultLevel = 0x10;
constexpr std::size_t kGroupTableSite = 0x24;
constexpr std::uint8_t kGroupTableShape[] = {0x44, 0x8D, 0x04, 0xDD, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x05};
constexpr std::size_t kGroupTableDisp = 0x24 + 11;

using ExecuteFn = void(__fastcall*)(void*, void*, void*);
using RangeCheckFn = bool(__fastcall*)(int, int, int, void*);
using CanEnchantFn = void*(__fastcall*)(void*, void*, std::uint64_t, bool);

ExecuteFn g_execute = nullptr;
RangeCheckFn g_rangeCheck = nullptr;
CanEnchantFn g_canEnchant = nullptr;
const void* g_rangeReturn = nullptr;
const void* g_canEnchantReturn = nullptr;
const void* g_checkReturn = nullptr;
const std::uint64_t* g_groupTable = nullptr;

thread_local int t_inEnchantCommand = 0;

std::atomic<int> g_rangeLogs{0};
std::atomic<int> g_relaxLogs{0};

struct ExistingEntry {
    bool found = false;
    std::size_t index = 0;
    std::int32_t level = 0;
};

bool findExisting(std::byte* enchants, std::uint8_t type, ExistingEntry& out)
{
    std::uint64_t group = 0;
    if (!memory::copyGuarded(g_groupTable + type, &group, sizeof(group)) || group >= kGroupCount) {
        return false;
    }
    std::byte* range[2] = {};
    if (!memory::copyGuarded(enchants + kGroupVectors + group * kGroupStride, range, sizeof(range))) {
        return false;
    }
    out = ExistingEntry{};
    if (range[0] == nullptr || range[1] <= range[0]) {
        return range[1] == range[0];
    }
    if ((range[1] - range[0]) % kEntrySize != 0) {
        return false;
    }
    const std::size_t count = static_cast<std::size_t>(range[1] - range[0]) / kEntrySize;
    for (std::size_t i = 0; i < count; ++i) {
        std::byte* const entry = range[0] + i * kEntrySize;
        std::uint8_t entryType = 0;
        std::int32_t level = 0;
        if (!memory::copyGuarded(entry, &entryType, sizeof(entryType))
            || !memory::copyGuarded(entry + kEntryLevel, &level, sizeof(level))) {
            return false;
        }
        if (entryType == type) {
            out = ExistingEntry{true, i, level};
            return true;
        }
    }
    return true;
}

void __fastcall detourExecute(void* self, void* origin, void* output)
{
    ++t_inEnchantCommand;
    g_execute(self, origin, output);
    --t_inEnchantCommand;
}

bool __fastcall detourRangeCheck(int value, int min, int max, void* output)
{
    if (_ReturnAddress() == g_rangeReturn && max < kMaxLevel) {
        if (value > max && value <= kMaxLevel && g_rangeLogs.fetch_add(1, std::memory_order_relaxed) < kLogLimit) {
            log().info(L"ExtendedEnchantLevel: /enchant level {} allowed (vanilla maximum {})", value, max);
        }
        max = kMaxLevel;
    }
    return g_rangeCheck(value, min, max, output);
}

void* __fastcall detourCanEnchant(void* enchants, void* result, std::uint64_t instance, bool allowNonVanilla)
{
    const void* const back = _ReturnAddress();
    void* const answer = g_canEnchant(enchants, result, instance, allowNonVanilla);
    if (t_inEnchantCommand <= 0 || (back != g_canEnchantReturn && back != g_checkReturn) || enchants == nullptr
        || result == nullptr) {
        return answer;
    }
    const auto type = static_cast<std::uint8_t>(instance & 0xff);
    const auto requested = static_cast<std::int32_t>(instance >> 32);
    ExistingEntry existing;
    if (type >= kTypeCount || !findExisting(static_cast<std::byte*>(enchants), type, existing)) {
        return answer;
    }
    std::uint8_t vanillaKind = 0;
    memory::copyGuarded(result, &vanillaKind, sizeof(vanillaKind));
    std::byte out[kResultSize] = {};
    std::int32_t level = requested;
    if (existing.found) {
        level = existing.level == requested ? std::min(existing.level + 1, kMaxLevel)
                                            : std::max(existing.level, requested);
        out[0] = static_cast<std::byte>(kResultModify);
        const std::uint64_t index = existing.index;
        std::memcpy(out + kResultIndex, &index, sizeof(index));
        std::memcpy(out + kResultLevel, &level, sizeof(level));
    } else {
        out[0] = static_cast<std::byte>(kResultAppend);
    }
    if (!memory::writeGuarded(result, out, sizeof(out))) {
        return answer;
    }
    if (vanillaKind != static_cast<std::uint8_t>(out[0]) && g_relaxLogs.fetch_add(1, std::memory_order_relaxed) < kLogLimit) {
        log().info(L"ExtendedEnchantLevel: /enchant type {} level {} allowed ({}; vanilla answer {})", type, level,
                   existing.found ? L"replaces the existing one" : L"added", vanillaKind);
    }
    return answer;
}

bool resolveCall(Target target, std::size_t relOffset, std::size_t nextOffset, void*& callee, const void*& back)
{
    const std::byte* const site = Scanner::instance().address(target);
    if (site == nullptr) {
        return false;
    }
    callee = memory::ripTarget(site, relOffset);
    back = site + nextOffset;
    return callee != nullptr && memory::inGameModule(callee);
}

}

void install()
{
    nlohmann::json& section = Config::instance().section(kSection);
    Config::keepOnly(section, {"enabled"});
    if (!Config::ensureBool(section, "enabled", true)) {
        log().info(L"ExtendedEnchantLevel: disabled in Tsukuyomi.json");
        return;
    }
    void* const execute = Scanner::instance().address(Target::EnchantCommandExecute);
    void* rangeCheck = nullptr;
    void* canEnchant = nullptr;
    void* canEnchantAgain = nullptr;
    if (execute == nullptr
        || !resolveCall(Target::EnchantLevelRangeCheckSite, kRangeCallRel, kRangeCallNext, rangeCheck, g_rangeReturn)
        || !resolveCall(Target::ApplyEnchantCanEnchantSite, kCanEnchantCallRel, kCanEnchantCallNext, canEnchant,
                        g_canEnchantReturn)
        || !resolveCall(Target::EnchantCanEnchantCheckSite, kCheckCallRel, kCheckCallNext, canEnchantAgain,
                        g_checkReturn)
        || canEnchantAgain != canEnchant) {
        log().error(L"ExtendedEnchantLevel: the /enchant level checks were not found");
        return;
    }
    std::uint8_t shape[sizeof(kGroupTableShape)] = {};
    if (!memory::copyGuarded(static_cast<std::byte*>(canEnchant) + kGroupTableSite, shape, sizeof(shape))
        || std::memcmp(shape, kGroupTableShape, sizeof(shape)) != 0) {
        log().error(L"ExtendedEnchantLevel: the enchantment group table was not found");
        return;
    }
    g_groupTable = static_cast<const std::uint64_t*>(
        memory::ripTarget(static_cast<std::byte*>(canEnchant), kGroupTableDisp));
    if (g_groupTable == nullptr || !memory::inGameModule(g_groupTable)) {
        log().error(L"ExtendedEnchantLevel: the enchantment group table was not found");
        return;
    }
    constexpr const wchar_t* kNames[] = {L"EnchantCommandExecute", L"EnchantLevelRangeCheck", L"EnchantCanEnchant"};
    for (const wchar_t* name : kNames) {
        if (!writes::allowed(name)) {
            log().info(L"ExtendedEnchantLevel: {} is turned off in hooks.json; /enchant keeps the vanilla limit", name);
            return;
        }
    }
    HookManager& hooks = HookManager::instance();
    if (!hooks.create(execute, reinterpret_cast<void*>(&detourExecute), reinterpret_cast<void**>(&g_execute),
                      kNames[0])
        || !hooks.create(rangeCheck, reinterpret_cast<void*>(&detourRangeCheck),
                         reinterpret_cast<void**>(&g_rangeCheck), kNames[1])
        || !hooks.create(canEnchant, reinterpret_cast<void*>(&detourCanEnchant),
                         reinterpret_cast<void**>(&g_canEnchant), kNames[2])) {
        log().error(L"ExtendedEnchantLevel: could not hook the /enchant level checks");
        return;
    }
    log().info(L"ExtendedEnchantLevel: /enchant accepts levels up to {}", kMaxLevel);
}

}
