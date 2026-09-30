#include "game/TradeUi.h"

#include "core/Logger.h"
#include "game/GameCallable.h"
#include "hooks/Detours.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <vector>

namespace tsukuyomi::tradeui {

namespace {

using gamecallable::CallableOps;
using gamecallable::GameCallable;

using ParseFn = void*(__fastcall*)(void* sel, const void* bag);
using SelPartFn = int(__fastcall*)(void* sel);
using GetOfferFn = const void*(__fastcall*)(void* model, int tier, int index);
using InvokeFn = int(__fastcall*)(GameCallable* self, const void* const* pbag);
using NestedBindFn = void(__fastcall*)(void* registry, const std::uint32_t* collection,
                                       const std::uint32_t* name, GameCallable* callable);
using JsonIndexFn = void*(__fastcall*)(void* value, const char* key);
using JsonFromDoubleFn = void(__fastcall*)(void* value, double number);
using JsonAssignFn = void*(__fastcall*)(void* dst, void* src);
using JsonDtorFn = void(__fastcall*)(void* value);
struct UiName {
    const char* data;
    unsigned long long size;
};
using BagSetBoolFn = void(__fastcall*)(void* bag, const UiName* name, const bool* value);

ParseFn g_parse = nullptr;
SelPartFn g_selTier = nullptr;
SelPartFn g_selIndex = nullptr;
GetOfferFn g_getOffer = nullptr;
const void* g_selectInvoke = nullptr;
NestedBindFn g_nestedBind = nullptr;
std::ptrdiff_t g_registryOffset = -1;
std::ptrdiff_t g_modelOffset = -1;
std::ptrdiff_t g_offerOwnerPtr = -1;
std::ptrdiff_t g_offerOwnerCount = -1;
std::ptrdiff_t g_traderIdOffset = -1;
JsonIndexFn g_jsonIndex = nullptr;
JsonFromDoubleFn g_jsonFromDouble = nullptr;
JsonAssignFn g_jsonAssign = nullptr;
JsonDtorFn g_jsonDtor = nullptr;
BagSetBoolFn g_bagSetBool = nullptr;
bool g_ready = false;
bool g_rowReady = false;

Listener* g_listener = nullptr;

int g_hoverTier = -1;
int g_hoverIndex = -1;
unsigned long long g_hoverMs = 0;

std::atomic<void*> g_clientCtrl{nullptr};
std::atomic<void*> g_clientModel{nullptr};
std::atomic<void*> g_clientOwner{nullptr};
std::atomic<int> g_unlockTier{-1};
SRWLOCK g_viewLock = SRWLOCK_INIT;
std::vector<std::pair<int, int>> g_favRows;
std::vector<std::vector<int>> g_restRows;
std::vector<int> g_rawCounts;
std::atomic<bool> g_viewActive{false};
thread_local int t_noRemap = 0;
std::ptrdiff_t g_selTierValue = -1;
std::ptrdiff_t g_selTierFlag = -1;
std::ptrdiff_t g_selIndexValue = -1;
std::ptrdiff_t g_selIndexFlag = -1;
TranslateFn g_translate = nullptr;
bool g_favTierHooked = false;
using TradePossibleFn = bool(__fastcall*)(void* model, const void* offer);
TradePossibleFn g_tradePossible = nullptr;
std::ptrdiff_t g_offerUsesOffset = -1;
std::ptrdiff_t g_offerMaxUsesOffset = -1;
std::uintptr_t g_nameLabelBegin = 0;
std::uintptr_t g_nameLabelEnd = 0;
using CurrentTierFn = int(__fastcall*)(void* owner);
CurrentTierFn g_currentTierOriginal = nullptr;
std::ptrdiff_t g_selectedIndexOffset = -1;
constexpr std::ptrdiff_t kOfferBytes = 0x1b0;

std::atomic<int> g_faults{0};
void noteFault(const wchar_t* what)
{
    if (g_faults.fetch_add(1) < 8) {
        log().error(L"TradeUi: {} faulted (the game's layout may have changed)", what);
    }
}

struct alignas(16) Selection {
    unsigned char bytes[0x80];
};

bool parseGuarded(const void* bag, int& tier, int& index)
{
    Selection sel{};
    __try {
        g_parse(&sel, bag);
        tier = g_selTier(&sel);
        index = g_selIndex(&sel);
        g_jsonDtor(&sel);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const void* getOfferGuarded(GetOfferFn fn, void* model, int tier, int index, bool& faulted)
{
    faulted = false;
    __try {
        return fn(model, tier, index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return nullptr;
    }
}

bool readPointerGuarded(const void* at, void*& out)
{
    __try {
        out = *static_cast<void* const*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct alignas(16) FakeBag {
    unsigned char bytes[0x40];
};

bool setNumberGuarded(void* parent, const char* key, double number)
{
    alignas(16) unsigned char tmp[16]{};
    __try {
        void* const slot = g_jsonIndex(parent, key);
        g_jsonFromDouble(tmp, number);
        g_jsonAssign(slot, tmp);
        g_jsonDtor(tmp);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool buildBagGuarded(FakeBag& bag, int tier, int index)
{
    __try {
        void* const root = bag.bytes + 8;
        void* const collections = g_jsonIndex(root, "#collections");
        return setNumberGuarded(collections, "trade_tiers", tier)
               && setNumberGuarded(collections, "trades", index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void destroyBagGuarded(FakeBag& bag)
{
    __try {
        g_jsonDtor(bag.bytes + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

const void* g_toggleInvoke = nullptr;

bool invokeToggleGuarded(GameCallable* callable, void* event)
{
    __try {
        using Fn = std::uint64_t(__fastcall*)(GameCallable*, void*);
        reinterpret_cast<Fn>(const_cast<void*>(g_toggleInvoke))(callable, event);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool invokeSelectGuarded(GameCallable* callable, const void* const* pbag)
{
    __try {
        reinterpret_cast<InvokeFn>(const_cast<void*>(g_selectInvoke))(callable, pbag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool nestedBindGuarded(void* registry, const std::uint32_t* coll, const std::uint32_t* name,
                       GameCallable* callable)
{
    __try {
        g_nestedBind(registry, coll, name, callable);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void bagSetBoolGuarded(void* bag, const char* name, std::size_t size, bool value)
{
    const UiName key{name, size};
    __try {
        g_bagSetBool(bag, &key, &value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

constexpr std::uint32_t fnv(const char* s)
{
    std::uint32_t h = 0x811C9DC5u;
    for (; *s != 0; ++s) {
        h ^= static_cast<std::uint8_t>(*s);
        h *= 0x01000193u;
    }
    return h;
}

struct RowSlot {
    RowBoolGetter fn = nullptr;
};
constexpr int kMaxRowSlots = 16;
RowSlot g_rowSlots[kMaxRowSlots];
int g_rowSlotCount = 0;

void __fastcall invokeRow(GameCallable* self, const void* name, void* bag)
{
    const auto* slot = static_cast<const RowSlot*>(self->capture);
    if (slot == nullptr || slot->fn == nullptr || name == nullptr || bag == nullptr) {
        return;
    }
    int tier = -1;
    int index = -1;
    if (!parseGuarded(bag, tier, index)) {
        return;
    }
    const bool value = slot->fn(tier, index);
    const auto* s = static_cast<const unsigned char*>(name);
    std::uint64_t size = 0;
    std::uint64_t cap = 0;
    std::memcpy(&size, s + 0x10, 8);
    std::memcpy(&cap, s + 0x18, 8);
    if (size == 0 || size > 64 || cap < size) {
        return;
    }
    const char* text = (cap > 15) ? *reinterpret_cast<const char* const*>(s) : reinterpret_cast<const char*>(s);
    bagSetBoolGuarded(bag, text, static_cast<std::size_t>(size), value);
}

const CallableOps kRowOps{gamecallable::moveCapture, gamecallable::destroyNothing,
                          reinterpret_cast<const void*>(&invokeRow)};

const std::byte* callAfter(const std::byte* begin, std::size_t size, std::uint32_t imm, std::size_t* at)
{
    std::uint8_t want[4];
    std::memcpy(want, &imm, 4);
    for (std::size_t i = 0; i + 4 <= size; ++i) {
        if (std::memcmp(begin + i, want, 4) != 0) {
            continue;
        }
        if (at != nullptr) {
            *at = i;
        }
        for (std::size_t j = i + 4; j + 5 <= size && j < i + 4 + 0x30; ++j) {
            if (static_cast<std::uint8_t>(begin[j]) != 0xE8) {
                continue;
            }
            std::int32_t rel = 0;
            std::memcpy(&rel, begin + j + 1, 4);
            const std::byte* target = begin + j + 5 + rel;
            if (memory::inGameModule(target) && memory::isExecutable(target, 1)) {
                return target;
            }
        }
        return nullptr;
    }
    return nullptr;
}

const void* bindingInvoke(const char* name)
{
    const std::byte* const ctor = Scanner::instance().address(Target::Trade2Ctor);
    constexpr std::size_t kCtorBytes = 0x2000;
    if (ctor == nullptr || name == nullptr || !memory::isReadable(ctor, kCtorBytes)) {
        return nullptr;
    }
    const std::uint32_t imm = fnv(name);
    std::size_t at = 0;
    bool found = false;
    for (std::size_t j = 0; j + 4 <= kCtorBytes; ++j) {
        if (std::memcmp(ctor + j, &imm, 4) == 0) {
            at = j;
            found = true;
            break;
        }
    }
    if (!found) {
        return nullptr;
    }
    const std::byte* ops = nullptr;
    for (std::size_t j = (at > 0x30 ? at - 0x30 : 0); j + 7 <= at; ++j) {
        if (static_cast<std::uint8_t>(ctor[j]) == 0x48 && static_cast<std::uint8_t>(ctor[j + 1]) == 0x8D
            && static_cast<std::uint8_t>(ctor[j + 2]) == 0x05) {
            std::int32_t rel = 0;
            std::memcpy(&rel, ctor + j + 3, 4);
            ops = ctor + j + 7 + rel;
        }
    }
    void* invoke = nullptr;
    if (ops == nullptr || !memory::inGameModule(ops) || !readPointerGuarded(ops + 16, invoke) || invoke == nullptr
        || !memory::inGameModule(invoke) || !memory::isExecutable(invoke, 1)) {
        return nullptr;
    }
    return invoke;
}

bool resolveJson(const std::byte* bagSet)
{
    if (bagSet == nullptr || !memory::isReadable(bagSet, 0xB0)) {
        return false;
    }
    std::vector<const std::byte*> calls;
    for (std::size_t i = 0; i + 5 <= 0xB0 && calls.size() < 8; ++i) {
        if (static_cast<std::uint8_t>(bagSet[i]) != 0xE8) {
            continue;
        }
        std::int32_t rel = 0;
        std::memcpy(&rel, bagSet + i + 1, 4);
        const std::byte* target = bagSet + i + 5 + rel;
        if (memory::inGameModule(target) && memory::isExecutable(target, 1)) {
            calls.push_back(target);
            i += 4;
        }
    }
    if (calls.size() < 6) {
        return false;
    }
    g_jsonIndex = reinterpret_cast<JsonIndexFn>(const_cast<std::byte*>(calls[0]));
    g_jsonFromDouble = reinterpret_cast<JsonFromDoubleFn>(const_cast<std::byte*>(calls[3]));
    g_jsonAssign = reinterpret_cast<JsonAssignFn>(const_cast<std::byte*>(calls[4]));
    g_jsonDtor = reinterpret_cast<JsonDtorFn>(const_cast<std::byte*>(calls[5]));
    return true;
}

}

void onScansReady()
{
    if (g_ready) {
        return;
    }
    Scanner& scanner = Scanner::instance();
    g_parse = scanner.addressAs<ParseFn>(Target::TradeSelParse);
    g_selTier = scanner.addressAs<SelPartFn>(Target::TradeSelTier);
    g_selIndex = scanner.addressAs<SelPartFn>(Target::TradeSelIndex);
    g_getOffer = scanner.addressAs<GetOfferFn>(Target::TradeGetOffer);
    auto selFields = [](const void* fn, std::ptrdiff_t& value, std::ptrdiff_t& flag) {
        const auto* c = static_cast<const std::uint8_t*>(fn);
        if (c != nullptr && memory::isReadable(c, 0x2c) && c[22] == 0x0F && c[23] == 0xB6 && c[24] == 0x49
            && c[39] == 0x8B && c[40] == 0x46) {
            flag = c[25];
            value = c[41];
        }
    };
    selFields(reinterpret_cast<const void*>(g_selTier), g_selTierValue, g_selTierFlag);
    selFields(reinterpret_cast<const void*>(g_selIndex), g_selIndexValue, g_selIndexFlag);
    g_tradePossible = scanner.addressAs<TradePossibleFn>(Target::TradePossible);
    if (const auto* x = static_cast<const std::uint8_t*>(bindingInvoke("#trade_cross_out_visible"));
        x != nullptr && memory::isReadable(x, 0xa0)) {
        for (std::size_t k = 0; k + 6 <= 0xa0; ++k) {
            std::int32_t disp = 0;
            std::memcpy(&disp, x + k + 2, 4);
            if (disp <= 0 || disp >= static_cast<std::int32_t>(kOfferBytes)) {
                continue;
            }
            if (g_offerMaxUsesOffset < 0 && x[k] == 0x8B && x[k + 1] == 0x88) {
                g_offerMaxUsesOffset = disp;
            } else if (g_offerMaxUsesOffset > 0 && g_offerUsesOffset < 0 && x[k] == 0x39 && x[k + 1] == 0x88) {
                g_offerUsesOffset = disp;
            }
        }
    }
    g_toggleInvoke = scanner.address(Target::TradeToggleInvoke);
    if (g_selectInvoke == nullptr) {
        g_selectInvoke = scanner.address(Target::TradeSelectInvoke);
    }
    if (const std::byte* sel = scanner.address(Target::TradeSelectInvoke)) {
        if (memory::isReadable(sel + 0x57, 7) && static_cast<std::uint8_t>(sel[0x57]) == 0x48
            && static_cast<std::uint8_t>(sel[0x58]) == 0x8B && static_cast<std::uint8_t>(sel[0x59]) == 0x8E) {
            std::int32_t disp = 0;
            std::memcpy(&disp, sel + 0x5A, 4);
            if (disp > 0 && disp < 0x4000) {
                g_modelOffset = disp;
            }
        }
    }
    if (const auto* fn = reinterpret_cast<const std::uint8_t*>(g_getOffer); fn != nullptr && memory::isReadable(fn, 0x80)) {
        for (std::size_t k = 0; k + 7 <= 0x80; ++k) {
            std::int32_t disp = 0;
            std::memcpy(&disp, fn + k + 3, 4);
            if (g_offerOwnerCount < 0 && fn[k] == 0x48 && fn[k + 1] == 0x8B && fn[k + 2] == 0x91) {
                g_offerOwnerCount = disp;
            } else if (g_offerOwnerPtr < 0 && fn[k] == 0x4C && fn[k + 1] == 0x8B && fn[k + 2] == 0xB9) {
                g_offerOwnerPtr = disp;
            }
        }
    }
    if (const std::byte* load = scanner.address(Target::TradeTraderIdLoad); load != nullptr && memory::isReadable(load, 7)) {
        std::int32_t disp = 0;
        std::memcpy(&disp, load + 3, 4);
        if (disp > 0 && disp < 0x2000) {
            g_traderIdOffset = disp;
        }
    }
    if (const std::byte* sel = scanner.address(Target::TradeSelectModel); sel != nullptr && memory::isReadable(sel, 0x200)) {
        for (std::size_t k = 0; k + 8 <= 0x200; ++k) {
            if (static_cast<std::uint8_t>(sel[k]) == 0x41 && static_cast<std::uint8_t>(sel[k + 1]) == 0x89
                && static_cast<std::uint8_t>(sel[k + 2]) == 0x94 && static_cast<std::uint8_t>(sel[k + 3]) == 0x24) {
                std::int32_t disp = 0;
                std::memcpy(&disp, sel + k + 4, 4);
                if (disp > 0 && disp < 0x1000) {
                    g_selectedIndexOffset = disp;
                }
                break;
            }
        }
    }
    const bool jsonOk = resolveJson(static_cast<const std::byte*>(hooks::uiBagSetFunction()));
    g_ready = g_parse != nullptr && g_selTier != nullptr && g_selIndex != nullptr
              && g_getOffer != nullptr && g_selectInvoke != nullptr && g_modelOffset > 0 && jsonOk;

    if (const std::byte* ctor = scanner.address(Target::Trade2Ctor)) {
        constexpr std::size_t kCtorBytes = 0x2000;
        if (memory::isReadable(ctor, kCtorBytes)) {
            std::size_t at = 0;
            const std::byte* a = callAfter(ctor, kCtorBytes, fnv("#trade_toggle_state"), &at);
            const std::byte* b = callAfter(ctor, kCtorBytes, fnv("#trade_possible"), nullptr);
            if (a != nullptr && a == b) {
                for (std::size_t k = (at > 0x20 ? at - 0x20 : 0); k + 7 <= at; ++k) {
                    if (static_cast<std::uint8_t>(ctor[k]) == 0x4C && static_cast<std::uint8_t>(ctor[k + 1]) == 0x8D
                        && static_cast<std::uint8_t>(ctor[k + 2]) == 0xA8) {
                        std::int32_t disp = 0;
                        std::memcpy(&disp, ctor + k + 3, 4);
                        if (disp > 0 && disp < 0x4000) {
                            g_registryOffset = disp;
                        }
                    }
                }
                g_nestedBind = reinterpret_cast<NestedBindFn>(const_cast<std::byte*>(a));
            }
            std::size_t nameAt = 0;
            if (callAfter(ctor, kCtorBytes, fnv("#name_label"), &nameAt) != nullptr) {
                const std::byte* ops = nullptr;
                for (std::size_t k = (nameAt > 0x30 ? nameAt - 0x30 : 0); k + 7 <= nameAt; ++k) {
                    if (static_cast<std::uint8_t>(ctor[k]) == 0x48 && static_cast<std::uint8_t>(ctor[k + 1]) == 0x8D
                        && static_cast<std::uint8_t>(ctor[k + 2]) == 0x05) {
                        std::int32_t rel = 0;
                        std::memcpy(&rel, ctor + k + 3, 4);
                        ops = ctor + k + 7 + rel;
                    }
                }
                void* invoke = nullptr;
                if (ops != nullptr && memory::inGameModule(ops) && readPointerGuarded(ops + 16, invoke)
                    && invoke != nullptr && memory::inGameModule(invoke)) {
                    DWORD64 imageBase = 0;
                    if (const PRUNTIME_FUNCTION rf =
                            RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(invoke), &imageBase, nullptr);
                        rf != nullptr && imageBase + rf->BeginAddress == reinterpret_cast<DWORD64>(invoke)) {
                        g_nameLabelBegin = static_cast<std::uintptr_t>(imageBase + rf->BeginAddress);
                        g_nameLabelEnd = static_cast<std::uintptr_t>(imageBase + rf->EndAddress);
                    }
                }
            }
            std::size_t immAt = 0;
            if (callAfter(ctor, kCtorBytes, fnv("#trade_possible"), &immAt) != nullptr) {
                const std::byte* ops = nullptr;
                for (std::size_t k = (immAt > 0x40 ? immAt - 0x40 : 0); k + 7 <= immAt; ++k) {
                    if (static_cast<std::uint8_t>(ctor[k]) == 0x48 && static_cast<std::uint8_t>(ctor[k + 1]) == 0x8D
                        && static_cast<std::uint8_t>(ctor[k + 2]) == 0x05) {
                        std::int32_t rel = 0;
                        std::memcpy(&rel, ctor + k + 3, 4);
                        ops = ctor + k + 7 + rel;
                    }
                }
                void* invoke = nullptr;
                if (ops != nullptr && memory::inGameModule(ops)
                    && readPointerGuarded(ops + 16, invoke) && invoke != nullptr
                    && memory::inGameModule(invoke) && memory::isReadable(invoke, 0x120)) {
                    const auto* code = static_cast<const std::uint8_t*>(invoke);
                    constexpr std::uint8_t kPat[] = {0x4C, 0x8D, 0x45, 0xFF, 0x48, 0x89, 0xF1, 0xE8};
                    for (std::size_t k = 0; k + sizeof(kPat) + 4 <= 0x120; ++k) {
                        if (std::memcmp(code + k, kPat, sizeof(kPat)) == 0) {
                            std::int32_t rel = 0;
                            std::memcpy(&rel, code + k + sizeof(kPat), 4);
                            const auto* target = code + k + sizeof(kPat) + 4 + rel;
                            if (memory::inGameModule(target) && memory::isExecutable(target, 1)) {
                                g_bagSetBool = reinterpret_cast<BagSetBoolFn>(const_cast<std::uint8_t*>(target));
                            }
                            break;
                        }
                    }
                }
            }
        }
    }
    g_rowReady = g_ready && g_nestedBind != nullptr && g_registryOffset > 0 && g_bagSetBool != nullptr;
    log().info(L"TradeUi: {} (model +{:#x}, rows {} registry +{:#x}, json {}, trader id +{:#x}/+{:#x}/+{:#x}, "
               L"toggle {}, name label {:#x}..{:#x}, selection +{:#x}/+{:#x} +{:#x}/+{:#x})",
               g_ready ? L"ready" : L"NOT usable", g_modelOffset, g_rowReady ? L"ok" : L"no",
               g_registryOffset, jsonOk, g_offerOwnerPtr, g_offerOwnerCount, g_traderIdOffset,
               g_toggleInvoke != nullptr,
               g_nameLabelBegin != 0 ? g_nameLabelBegin - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) : 0,
               g_nameLabelEnd != 0 ? g_nameLabelEnd - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) : 0,
               g_selTierValue, g_selTierFlag, g_selIndexValue, g_selIndexFlag);
    log().info(L"TradeUi: trade possible {}, offer uses +{:#x} / max uses +{:#x}", g_tradePossible != nullptr,
               g_offerUsesOffset, g_offerMaxUsesOffset);
}

bool available()
{
    return g_ready;
}

void setListener(Listener* listener)
{
    g_listener = listener;
}

void onHoverInvoke(const void* bag)
{
    if (!g_ready || bag == nullptr) {
        return;
    }
    int tier = -1;
    int index = -1;
    if (!parseGuarded(bag, tier, index)) {
        noteFault(L"parsing the hovered row");
        return;
    }
    g_hoverTier = tier;
    g_hoverIndex = index;
    g_hoverMs = GetTickCount64();
}

void onSecondaryInvoke(void* ctrl, const void* bag)
{
    (void)ctrl;
    if (!g_ready || bag == nullptr || g_listener == nullptr) {
        return;
    }
    int tier = -1;
    int index = -1;
    if (!parseGuarded(bag, tier, index)) {
        noteFault(L"parsing the right-clicked row");
        return;
    }
    g_listener->onTradeSecondary(tier, index);
}

bool hovered(int& tier, int& index, unsigned long long withinMs)
{
    if (g_hoverTier < 0 || g_hoverIndex < 0 || GetTickCount64() - g_hoverMs > withinMs) {
        return false;
    }
    tier = g_hoverTier;
    index = g_hoverIndex;
    return true;
}

void forgetHover()
{
    g_hoverTier = -1;
    g_hoverIndex = -1;
    g_hoverMs = 0;
}

const void* offer(void* ctrl, int tier, int index)
{
    if (!g_ready || ctrl == nullptr || tier < 0 || index < 0) {
        return nullptr;
    }
    void* model = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(ctrl) + g_modelOffset, model) || model == nullptr
        || !memory::isReadable(model, 0x100)) {
        return nullptr;
    }
    bool faulted = false;
    const void* result = getOfferGuarded(g_getOffer, model, tier, index, faulted);
    if (faulted) {
        noteFault(L"getOffer");
        return nullptr;
    }
    return result;
}

namespace {
bool tradePossibleGuarded(void* model, const void* offer, bool& result)
{
    __try {
        result = g_tradePossible(model, offer);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readOfferIntsGuarded(const void* offer, int& uses, int& maxUses)
{
    __try {
        std::memcpy(&uses, static_cast<const std::byte*>(offer) + g_offerUsesOffset, 4);
        std::memcpy(&maxUses, static_cast<const std::byte*>(offer) + g_offerMaxUsesOffset, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}

bool tradePossible(void* ctrl, const void* offer)
{
    if (!g_ready || g_tradePossible == nullptr || ctrl == nullptr || offer == nullptr) {
        return true;
    }
    void* model = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(ctrl) + g_modelOffset, model) || model == nullptr) {
        return true;
    }
    bool result = true;
    if (!tradePossibleGuarded(model, offer, result)) {
        noteFault(L"trade possible");
        return true;
    }
    return result;
}

bool offerSoldOut(const void* offer)
{
    if (offer == nullptr || g_offerUsesOffset <= 0 || g_offerMaxUsesOffset <= 0) {
        return false;
    }
    int uses = 0;
    int maxUses = -1;
    if (!readOfferIntsGuarded(offer, uses, maxUses)) {
        return false;
    }
    return maxUses >= 0 && uses >= maxUses;
}

bool traderId(void* ctrl, std::int64_t& out)
{
    out = 0;
    if (!g_ready || ctrl == nullptr || g_offerOwnerPtr <= 0 || g_offerOwnerCount <= 0 || g_traderIdOffset <= 0) {
        return false;
    }
    void* model = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(ctrl) + g_modelOffset, model) || model == nullptr) {
        return false;
    }
    void* counts = nullptr;
    void* owner = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(model) + g_offerOwnerCount, counts) || counts == nullptr
        || !memory::isReadable(static_cast<std::byte*>(counts) + 8, 4)
        || *reinterpret_cast<const volatile std::int32_t*>(static_cast<std::byte*>(counts) + 8) <= 0
        || !readPointerGuarded(static_cast<std::byte*>(model) + g_offerOwnerPtr, owner) || owner == nullptr
        || !memory::isReadable(static_cast<std::byte*>(owner) + g_traderIdOffset, 8)) {
        return false;
    }
    std::memcpy(&out, static_cast<std::byte*>(owner) + g_traderIdOffset, 8);
    return out != 0 && out != -1;
}

const void* offerBuyA(const void* o)
{
    return o;
}
const void* offerBuyB(const void* o)
{
    return o != nullptr ? static_cast<const std::byte*>(o) + 0x80 : nullptr;
}
const void* offerSell(const void* o)
{
    return o != nullptr ? static_cast<const std::byte*>(o) + 0x100 : nullptr;
}

bool selectTrade(void* ctrl, int tier, int index)
{
    if (!g_ready || ctrl == nullptr || offer(ctrl, tier, index) == nullptr) {
        return false;
    }
    FakeBag bag{};
    if (!buildBagGuarded(bag, tier, index)) {
        destroyBagGuarded(bag);
        noteFault(L"building the selection bag");
        return false;
    }
    GameCallable callable;
    callable.capture = ctrl;
    struct NoRemap {
        NoRemap() { ++t_noRemap; }
        ~NoRemap() { --t_noRemap; }
    } noRemap;
    if (g_toggleInvoke != nullptr) {
        alignas(8) std::byte event[0x20]{};
        event[0x08] = std::byte{1};
        const void* bagPtr = &bag;
        std::memcpy(event + 0x10, &bagPtr, sizeof(bagPtr));
        if (!invokeToggleGuarded(&callable, event)) {
            destroyBagGuarded(bag);
            noteFault(L"trade_toggle");
            return false;
        }
    }
    const void* pbag = &bag;
    const bool ok = invokeSelectGuarded(&callable, &pbag);
    destroyBagGuarded(bag);
    if (!ok) {
        noteFault(L"trade_select");
    }
    return ok;
}

namespace {

void* ownerOfModel(void* model)
{
    if (model == nullptr || g_offerOwnerPtr <= 0 || g_offerOwnerCount <= 0) {
        return nullptr;
    }
    void* counts = nullptr;
    void* owner = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(model) + g_offerOwnerCount, counts) || counts == nullptr
        || !memory::isReadable(static_cast<std::byte*>(counts) + 8, 4)
        || *reinterpret_cast<const volatile std::int32_t*>(static_cast<std::byte*>(counts) + 8) <= 0
        || !readPointerGuarded(static_cast<std::byte*>(model) + g_offerOwnerPtr, owner)) {
        return nullptr;
    }
    return owner;
}

}

void setClientScreen(void* ctrl)
{
    void* model = nullptr;
    if (g_ready && ctrl != nullptr) {
        if (!readPointerGuarded(static_cast<std::byte*>(ctrl) + g_modelOffset, model)) {
            model = nullptr;
        }
    }
    void* const owner = ownerOfModel(model);
    g_clientCtrl.store(model != nullptr ? ctrl : nullptr, std::memory_order_release);
    g_clientModel.store(model, std::memory_order_release);
    g_clientOwner.store(owner, std::memory_order_release);
    if (model == nullptr && g_viewActive.load(std::memory_order_relaxed)) {
        setFavoriteTier({}, {});
    }
}

const void* offerRaw(void* ctrl, int tier, int rawIndex)
{
    return offer(ctrl, tier, rawIndex);
}

void setUnlockTier(int maxTier)
{
    g_unlockTier.store(maxTier, std::memory_order_relaxed);
}

void setCurrentTierOriginal(const void* original)
{
    g_currentTierOriginal = reinterpret_cast<CurrentTierFn>(const_cast<void*>(original));
}

namespace {
bool callCurrentTierGuarded(void* owner, int& out)
{
    __try {
        out = g_currentTierOriginal(owner);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readIntGuarded(const void* at, int& out)
{
    __try {
        out = *static_cast<const volatile int*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* ownerOfCtrl(void* ctrl)
{
    void* model = nullptr;
    if (!g_ready || ctrl == nullptr || !readPointerGuarded(static_cast<std::byte*>(ctrl) + g_modelOffset, model)) {
        return nullptr;
    }
    return ownerOfModel(model);
}
}

int currentTier(void* ctrl)
{
    void* const owner = ownerOfCtrl(ctrl);
    int value = -1;
    if (owner == nullptr || g_currentTierOriginal == nullptr || !callCurrentTierGuarded(owner, value)) {
        return -1;
    }
    return value;
}

bool selectedOffer(void* ctrl, int& tier, int& rawIndex)
{
    tier = -1;
    rawIndex = -1;
    void* const owner = ownerOfCtrl(ctrl);
    int sel = -1;
    if (owner == nullptr || g_selectedIndexOffset <= 0
        || !readIntGuarded(static_cast<const std::byte*>(owner) + g_selectedIndexOffset, sel) || sel < 0) {
        return false;
    }
    const std::byte* begin = nullptr;
    for (int t = 0; t < 8; ++t) {
        for (int i = 0; i < 64; ++i) {
            const auto* o = static_cast<const std::byte*>(offerRaw(ctrl, t, i));
            if (o == nullptr) {
                break;
            }
            if (begin == nullptr || o < begin) {
                begin = o;
            }
        }
    }
    if (begin == nullptr) {
        return false;
    }
    const std::byte* want = begin + static_cast<std::ptrdiff_t>(sel) * kOfferBytes;
    for (int t = 0; t < 8; ++t) {
        for (int i = 0; i < 64; ++i) {
            const void* o = offerRaw(ctrl, t, i);
            if (o == nullptr) {
                break;
            }
            if (o == want) {
                tier = t;
                rawIndex = i;
                return true;
            }
        }
    }
    return false;
}

namespace {

constexpr int kNoRow = 0x3fff;

bool mapView(int viewTier, int viewIndex, int& rawTier, int& rawIndex)
{
    rawTier = viewTier;
    rawIndex = viewIndex;
    if (!g_viewActive.load(std::memory_order_acquire) || viewTier < 0 || viewIndex < 0) {
        return false;
    }
    bool mapped = false;
    AcquireSRWLockShared(&g_viewLock);
    if (!g_favRows.empty()) {
        mapped = true;
        const auto i = static_cast<std::size_t>(viewIndex);
        if (viewTier == 0) {
            if (i < g_favRows.size()) {
                rawTier = g_favRows[i].first;
                rawIndex = g_favRows[i].second;
            } else {
                rawTier = 0;
                rawIndex = kNoRow;
            }
        } else {
            const auto t = static_cast<std::size_t>(viewTier - 1);
            rawTier = viewTier - 1;
            if (t < g_restRows.size()) {
                rawIndex = (i < g_restRows[t].size()) ? g_restRows[t][i] : kNoRow;
            }
        }
    }
    ReleaseSRWLockShared(&g_viewLock);
    return mapped;
}

bool readSelGuarded(void* sel, int& tier, int& index)
{
    __try {
        tier = g_selTier(sel);
        index = g_selIndex(sel);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeSelGuarded(void* sel, int tier, int index)
{
    __try {
        auto* const b = static_cast<std::uint8_t*>(sel);
        if (b[g_selTierFlag] != 1 || b[g_selIndexFlag] != 1) {
            return false;
        }
        std::memcpy(b + g_selTierValue, &tier, sizeof(tier));
        std::memcpy(b + g_selIndexValue, &index, sizeof(index));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readCtrlOfBindingGuarded(const void* self, void*& ctrl)
{
    __try {
        std::memcpy(&ctrl, static_cast<const std::byte*>(self) + 8, sizeof(ctrl));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}

void setFavoriteTier(const std::vector<std::pair<int, int>>& favorites, const std::vector<int>& tierCounts)
{
    std::vector<std::vector<int>> rest;
    if (!favorites.empty()) {
        rest.resize(tierCounts.size());
        for (std::size_t t = 0; t < tierCounts.size(); ++t) {
            for (int i = 0; i < tierCounts[t]; ++i) {
                const bool moved = std::find(favorites.begin(), favorites.end(),
                                             std::pair<int, int>(static_cast<int>(t), i))
                                   != favorites.end();
                if (!moved) {
                    rest[t].push_back(i);
                }
            }
        }
    }
    AcquireSRWLockExclusive(&g_viewLock);
    g_favRows = favorites;
    g_restRows = std::move(rest);
    g_rawCounts = tierCounts;
    ReleaseSRWLockExclusive(&g_viewLock);
    g_viewActive.store(!favorites.empty(), std::memory_order_release);
}

bool favoriteTierShown(const void* self)
{
    if (!g_viewActive.load(std::memory_order_acquire) || self == nullptr) {
        return false;
    }
    void* ctrl = nullptr;
    if (!readCtrlOfBindingGuarded(self, ctrl)) {
        return false;
    }
    return ctrl != nullptr && ctrl == g_clientCtrl.load(std::memory_order_acquire);
}

bool favoriteTierShownAny()
{
    return g_viewActive.load(std::memory_order_acquire) && g_clientCtrl.load(std::memory_order_acquire) != nullptr;
}

int favoriteTierRows()
{
    AcquireSRWLockShared(&g_viewLock);
    const int n = static_cast<int>(g_favRows.size());
    ReleaseSRWLockShared(&g_viewLock);
    return n;
}

bool restRows(int rawTier, int& count, bool& emptied)
{
    count = 0;
    emptied = false;
    bool known = false;
    AcquireSRWLockShared(&g_viewLock);
    const auto t = static_cast<std::size_t>(rawTier);
    if (rawTier >= 0 && t < g_restRows.size() && t < g_rawCounts.size()) {
        known = true;
        count = static_cast<int>(g_restRows[t].size());
        emptied = (count == 0 && g_rawCounts[t] > 0);
    }
    ReleaseSRWLockShared(&g_viewLock);
    return known;
}

void writeFavoriteTierName(void* ret)
{
    if (g_translate != nullptr && g_translate(ret, "xbox.profile.favorite")) {
        return;
    }
    auto* const s = static_cast<unsigned char*>(ret);
    std::memset(s, 0, 32);
    constexpr char kText[] = "Favorites";
    std::memcpy(s, kText, sizeof(kText) - 1);
    const std::uint64_t size = sizeof(kText) - 1;
    const std::uint64_t cap = 15;
    std::memcpy(s + 0x10, &size, 8);
    std::memcpy(s + 0x18, &cap, 8);
}

void afterSelParse(void* sel)
{
    if (!g_viewActive.load(std::memory_order_acquire) || t_noRemap > 0 || sel == nullptr || g_selTier == nullptr
        || g_selIndex == nullptr || g_selTierValue < 0 || g_selTierFlag < 0 || g_selIndexValue < 0
        || g_selIndexFlag < 0) {
        return;
    }
    int tier = -1;
    int index = -1;
    if (!readSelGuarded(sel, tier, index)) {
        noteFault(L"reading the row selection");
        return;
    }
    int rawTier = tier;
    int rawIndex = index;
    if (!mapView(tier, index, rawTier, rawIndex) || (rawTier == tier && rawIndex == index)) {
        return;
    }
    if (!writeSelGuarded(sel, rawTier, rawIndex)) {
        noteFault(L"rewriting the row selection");
    }
}

void setTranslator(TranslateFn fn)
{
    g_translate = fn;
}

void setFavoriteTierHooked(bool hooked)
{
    g_favTierHooked = hooked;
}

bool favoriteTierAvailable()
{
    return g_favTierHooked && g_selTierValue >= 0 && g_selTierFlag >= 0 && g_selIndexValue >= 0
           && g_selIndexFlag >= 0;
}

const void* resolveTierBinding(TierBinding which)
{
    static constexpr const char* kNames[] = {"#trade_selector_total", "#trade_tier_total", "#tier_visible",
                                             "#is_tier_unlocked", "#tier_name"};
    const auto k = static_cast<std::size_t>(which);
    if (k >= std::size(kNames)) {
        return nullptr;
    }
    return bindingInvoke(kNames[k]);
}

int overrideTier(void* owner, int value, const void* returnAddress)
{
    const int unlock = g_unlockTier.load(std::memory_order_relaxed);
    if (unlock < 0 || owner == nullptr || owner != g_clientOwner.load(std::memory_order_acquire)) {
        return value;
    }
    const auto ret = reinterpret_cast<std::uintptr_t>(returnAddress);
    if (g_nameLabelBegin != 0 && ret >= g_nameLabelBegin && ret < g_nameLabelEnd) {
        return value;
    }
    return value > unlock ? value : unlock;
}

bool bindRowBool(void* ctrl, const char* name, RowBoolGetter fn)
{
    if (!g_rowReady || ctrl == nullptr || name == nullptr || fn == nullptr) {
        return false;
    }
    int slot = -1;
    for (int i = 0; i < g_rowSlotCount; ++i) {
        if (g_rowSlots[i].fn == fn) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (g_rowSlotCount >= kMaxRowSlots) {
            return false;
        }
        slot = g_rowSlotCount++;
        g_rowSlots[slot].fn = fn;
    }
    const std::uint32_t collection = fnv("trade_tiers");
    const std::uint32_t hash = fnv(name);
    GameCallable callable;
    callable.ops = &kRowOps;
    callable.capture = &g_rowSlots[slot];
    if (!nestedBindGuarded(static_cast<std::byte*>(ctrl) + g_registryOffset, &collection, &hash, &callable)) {
        noteFault(L"row binding registration");
        return false;
    }
    return true;
}

}
