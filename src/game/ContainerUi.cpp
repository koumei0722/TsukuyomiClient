#include "game/ContainerUi.h"
#include "game/EnchantKey.h"

#include "game/BlockRegistry.h"
#include "game/GameCallable.h"

#include "hooks/Detours.h"

#include "core/Logger.h"
#include "core/Strings.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <algorithm>
#include <mutex>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <unordered_map>
#include <string_view>
#include <utility>
#include <vector>

namespace tsukuyomi::containerui {

namespace {

using GetItemFn = const void*(__fastcall*)(void* mc, const std::string* coll, int index);
using IsNullFn = bool(__fastcall*)(const void* stack);
using MaxStackFn = std::uint8_t(__fastcall*)(const void* stack);
using MatchesFn = bool(__fastcall*)(const void* a, const void* b, const void* flags);

SmHandleFn g_smOriginal = nullptr;
GetItemFn g_getItem = nullptr;
IsNullFn g_isNull = nullptr;
MaxStackFn g_maxStack = nullptr;
MatchesFn g_matches = nullptr;
const void* g_matchFlags = nullptr;
std::ptrdiff_t g_smOffset = -1;
std::ptrdiff_t g_mcOffset = -1;
bool g_ready = false;

Listener* g_listener = nullptr;
constexpr std::size_t kMaxObservers = 4;
Listener* g_observers[kMaxObservers] = {};

constexpr std::ptrdiff_t kStackItemOffset = 0x08;
constexpr std::ptrdiff_t kStackAuxOffset = 0x20;
constexpr std::ptrdiff_t kStackCountOffset = 0x22;
constexpr std::ptrdiff_t kStackValidOffset = 0x23;
constexpr std::ptrdiff_t kItemNameOffset = 0x128;

constexpr int kMaxCollection = 64;

int callSmGuarded(SmHandleFn fn, void* sm, std::uint32_t id, int state, const std::string* coll,
                  int index, bool& faulted)
{
    faulted = false;
    __try {
        return fn(sm, id, state, coll, index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return 0;
    }
}

const void* callGetItemGuarded(void* mc, const std::string* coll, int index, bool& faulted)
{
    faulted = false;
    __try {
        return g_getItem(mc, coll, index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return nullptr;
    }
}

const void* callScreenGetItemGuarded(const void* fn, void* ctrl, const std::string* coll, int index,
                                     bool& faulted)
{
    faulted = false;
    __try {
        using Fn = const void*(__fastcall*)(void*, const std::string*, int);
        return reinterpret_cast<Fn>(const_cast<void*>(fn))(ctrl, coll, index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return nullptr;
    }
}

using StorageInfoFn = void(__fastcall*)(void* out, const void* stack, void* mc);
using CompoundGetFn = const void*(__fastcall*)(const void* tag, const std::string_view* key);
StorageInfoFn g_storageInfo = nullptr;
CompoundGetFn g_compoundGet = nullptr;
std::ptrdiff_t g_stackUserDataOffset = -1;
std::ptrdiff_t g_itemCategoryOffset = -1;

bool callStorageInfoGuarded(void* out, const void* stack, void* mc, bool& faulted)
{
    faulted = false;
    __try {
        g_storageInfo(out, stack, mc);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return false;
    }
}

const void* callCompoundGetGuarded(const void* tag, const std::string_view* key, bool& faulted)
{
    faulted = false;
    __try {
        return g_compoundGet(tag, key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return nullptr;
    }
}

bool callIsNullGuarded(const void* stack, bool& faulted)
{
    faulted = false;
    __try {
        return g_isNull(stack);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return true;
    }
}

int callMaxStackGuarded(const void* stack, bool& faulted)
{
    faulted = false;
    __try {
        return g_maxStack(stack);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return 0;
    }
}

bool callMatchesGuarded(const void* a, const void* b, bool& faulted)
{
    faulted = false;
    __try {
        return g_matches(a, b, g_matchFlags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return false;
    }
}

bool readQwordGuarded(const void* at, std::uint64_t& out)
{
    __try {
        out = *static_cast<const volatile std::uint64_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readByteGuarded(const void* at, std::uint8_t& out)
{
    __try {
        out = *static_cast<const volatile std::uint8_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readWordGuarded(const void* at, std::uint16_t& out)
{
    __try {
        out = *static_cast<const volatile std::uint16_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readStdStringGuarded(const void* at, char* out, size_t outSize)
{
    __try {
        const auto* b = static_cast<const std::uint8_t*>(at);
        std::uint64_t size = *reinterpret_cast<const std::uint64_t*>(b + 0x10);
        const std::uint64_t cap = *reinterpret_cast<const std::uint64_t*>(b + 0x18);
        if (size > cap || size >= outSize || cap > 0x100000) {
            return false;
        }
        const char* text = (cap >= 16) ? *reinterpret_cast<const char* const*>(b)
                                       : reinterpret_cast<const char*>(b);
        for (std::uint64_t i = 0; i < size; ++i) {
            out[i] = text[i];
        }
        out[size] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct SlotKey {
    std::string coll;
    int index = -1;
    bool operator<(const SlotKey& o) const
    {
        return (coll != o.coll) ? coll < o.coll : index < o.index;
    }
};

struct ScreenState {
    void* sm = nullptr;
    void* ctrl = nullptr;
    void* mc = nullptr;
    std::uintptr_t kind = 0;
    ULONGLONG aliveMs = 0;
    bool aliveNow = false;
    const void* emptyItem = nullptr;
    std::unordered_map<std::string, int> sizes;
    std::map<SlotKey, POINT> positions;
};
ScreenState g_screen;

std::unordered_map<std::string, std::string> g_names;
const std::string& intern(const std::string& s)
{
    auto it = g_names.find(s);
    if (it == g_names.end()) {
        it = g_names.emplace(s, s).first;
    }
    return it->second;
}

std::string g_hoverColl;
int g_hoverIndex = -1;
POINT g_hoverCursor{};
ULONGLONG g_hoverMs = 0;
int g_pitch = 0;
std::string g_lastPitchColl;
int g_lastPitchIndex = -1;
POINT g_lastPitchCursor{};

std::atomic<int> g_synthDepth{0};

struct PendingPress {
    std::uint32_t id = 0;
    std::string coll;
    int index = -1;
};
PendingPress g_pending;
Stats g_stats;

std::atomic<bool> g_loggedScreen{false};
std::atomic<int> g_faultLogs{0};

void noteFault(const wchar_t* what)
{
    if (g_faultLogs.fetch_add(1) < 8) {
        log().error(L"ContainerUi: {} faulted (the game's layout may have changed)", what);
    }
}

void forgetScreen()
{
    const bool had = (g_screen.ctrl != nullptr);
    g_screen = ScreenState{};
    g_pending = PendingPress{};
    g_hoverColl.clear();
    g_hoverIndex = -1;
    g_hoverMs = 0;
    g_lastPitchColl.clear();
    g_lastPitchIndex = -1;
    if (had && g_listener != nullptr) {
        g_listener->onScreenLost();
    }
    if (had) {
        for (Listener* const one : g_observers) {
            if (one != nullptr) {
                one->onScreenLost();
            }
        }
    }
}

using gamecallable::CallableOps;
using gamecallable::GameCallable;

struct BindSlot {
    std::uintptr_t fn = 0;
    std::uintptr_t arg = 0;
};
constexpr int kMaxSlots = 1024;
BindSlot g_slots[kMaxSlots];
int g_slotCount = 0;
std::map<std::string, int> g_slotIndex;

constexpr auto callableMove = &gamecallable::moveCapture;
constexpr auto callableDestroy = &gamecallable::destroyNothing;

const BindSlot* slotOf(const GameCallable* self)
{
    return static_cast<const BindSlot*>(self->capture);
}

bool __fastcall invokeBool(GameCallable* self)
{
    const BindSlot* s = slotOf(self);
    return s != nullptr && s->fn != 0 && reinterpret_cast<BoolGetter>(s->fn)(s->arg);
}
int __fastcall invokeInt(GameCallable* self)
{
    const BindSlot* s = slotOf(self);
    return (s != nullptr && s->fn != 0) ? reinterpret_cast<IntGetter>(s->fn)(s->arg) : 0;
}
void* __fastcall invokeText(void* ret, GameCallable* self)
{
    char text[16]{};
    const BindSlot* s = slotOf(self);
    if (s != nullptr && s->fn != 0) {
        reinterpret_cast<TextGetter>(s->fn)(s->arg, text, sizeof(text));
    }
    text[15] = 0;
    auto* const out = static_cast<unsigned char*>(ret);
    std::memset(out, 0, 32);
    const std::uint64_t length = std::strlen(text);
    std::memcpy(out, text, length);
    const std::uint64_t capacity = 15;
    std::memcpy(out + 0x10, &length, sizeof(length));
    std::memcpy(out + 0x18, &capacity, sizeof(capacity));
    return ret;
}
bool __fastcall invokeTrue(GameCallable*)
{
    return true;
}
int __fastcall invokeCollInt(GameCallable* self, const std::string* coll, const int* index)
{
    if (self == nullptr || coll == nullptr || index == nullptr || self->spare[0] == 0) {
        return 0;
    }
    return reinterpret_cast<CollIntGetter>(self->spare[0])(self->capture, *coll, *index, self->spare[1]);
}
void __fastcall moveCollection(GameCallable* src, GameCallable* dst)
{
    dst->capture = src->capture;
    dst->spare[0] = src->spare[0];
    dst->spare[1] = src->spare[1];
}
int __fastcall invokeButton(GameCallable* self, void*)
{
    const BindSlot* s = slotOf(self);
    if (s != nullptr && s->fn != 0) {
        reinterpret_cast<ButtonHandler>(s->fn)(s->arg);
    }
    return 0;
}

const CallableOps kBoolOps{callableMove, callableDestroy, reinterpret_cast<const void*>(&invokeBool)};
const CallableOps kIntOps{callableMove, callableDestroy, reinterpret_cast<const void*>(&invokeInt)};
const CallableOps kTextOps{callableMove, callableDestroy, reinterpret_cast<const void*>(&invokeText)};
const CallableOps kTrueOps{callableMove, callableDestroy, reinterpret_cast<const void*>(&invokeTrue)};
const CallableOps kButtonOps{callableMove, callableDestroy,
                             reinterpret_cast<const void*>(&invokeButton)};
const CallableOps kCollIntOps{&moveCollection, callableDestroy,
                              reinterpret_cast<const void*>(&invokeCollInt)};

using BindRegFn = void(__fastcall*)(void* ctrl, const std::uint32_t* hash, GameCallable* getter,
                                    GameCallable* condition);
using ClickRegFn = void(__fastcall*)(void* ctrl, std::uint32_t id, GameCallable* handler);
using EventRegFn = void(__fastcall*)(void* ctrl, std::uint32_t id, bool flag, int state,
                                     GameCallable* handler);
BindRegFn g_bindBool = nullptr;
BindRegFn g_bindInt = nullptr;
BindRegFn g_bindText = nullptr;
BindRegFn g_bindCollInt = nullptr;
BindRegFn g_bindFloat = nullptr;
constexpr int kMaxHudObservers = 4;
std::atomic<HudCreatedFn> g_hudObservers[kMaxHudObservers]{};
std::atomic<int> g_hudObserverCount{0};
ClickRegFn g_regClick = nullptr;
EventRegFn g_regEvent = nullptr;
std::ptrdiff_t g_idAuxSlot = -1;
using ScreenGetItemFn = const void*(__fastcall*)(void* ctrl, const std::string* coll, int index);
ScreenGetItemFn g_screenGetItem = nullptr;
bool g_bindReady = false;
bool g_refresh = false;

bool callBindGuarded(BindRegFn fn, void* ctrl, const std::uint32_t* hash, GameCallable* getter,
                     GameCallable* condition)
{
    __try {
        fn(ctrl, hash, getter, condition);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool callClickRegGuarded(ClickRegFn fn, void* ctrl, std::uint32_t id, GameCallable* handler)
{
    __try {
        fn(ctrl, id, handler);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool callEventRegGuarded(EventRegFn fn, void* ctrl, std::uint32_t id, bool flag, int state,
                         GameCallable* handler)
{
    __try {
        fn(ctrl, id, flag, state, handler);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using IdAuxFn = int(__fastcall*)(const void* item, std::uint32_t aux, const void* userData);
int callIdAuxGuarded(IdAuxFn fn, const void* item, std::uint32_t aux, bool& faulted)
{
    faulted = false;
    __try {
        return fn(item, aux, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return 0;
    }
}

int slotFor(const char* kind, const char* name, std::uintptr_t fn, std::uintptr_t arg)
{
    std::string key = std::string(kind) + ':' + name;
    auto it = g_slotIndex.find(key);
    if (it != g_slotIndex.end()) {
        g_slots[it->second].fn = fn;
        g_slots[it->second].arg = arg;
        return it->second;
    }
    if (g_slotCount >= kMaxSlots) {
        return -1;
    }
    const int i = g_slotCount++;
    g_slots[i].fn = fn;
    g_slots[i].arg = arg;
    g_slotIndex.emplace(std::move(key), i);
    return i;
}

std::vector<const std::byte*> callsAfterImmediate(const std::byte* begin, std::size_t size,
                                                  std::uint32_t imm)
{
    std::vector<const std::byte*> out;
    if (begin == nullptr || !memory::isReadable(begin, size)) {
        return out;
    }
    std::uint8_t want[4];
    std::memcpy(want, &imm, 4);
    for (std::size_t i = 0; i + 4 <= size; ++i) {
        if (std::memcmp(begin + i, want, 4) != 0) {
            continue;
        }
        for (std::size_t j = i + 4; j + 5 <= size && j < i + 4 + 0x40; ++j) {
            if (static_cast<std::uint8_t>(begin[j]) != 0xE8) {
                continue;
            }
            std::int32_t rel = 0;
            std::memcpy(&rel, begin + j + 1, 4);
            const std::byte* target = begin + j + 5 + rel;
            if (memory::inGameModule(target) && memory::isExecutable(target, 1)) {
                out.push_back(target);
                break;
            }
        }
    }
    return out;
}

const void* opsBeforeImmediate(const std::byte* begin, std::size_t size, std::uint32_t imm)
{
    std::uint8_t want[4];
    std::memcpy(want, &imm, 4);
    for (std::size_t i = 0; i + 4 <= size; ++i) {
        if (std::memcmp(begin + i, want, 4) != 0) {
            continue;
        }
        const std::byte* found = nullptr;
        for (std::size_t j = (i > 0x40 ? i - 0x40 : 0); j + 7 <= i; ++j) {
            if (static_cast<std::uint8_t>(begin[j]) == 0x48
                && static_cast<std::uint8_t>(begin[j + 1]) == 0x8D
                && static_cast<std::uint8_t>(begin[j + 2]) == 0x05) {
                std::int32_t rel = 0;
                std::memcpy(&rel, begin + j + 3, 4);
                found = begin + j + 7 + rel;
            }
        }
        return found;
    }
    return nullptr;
}

void resolveBinders(const std::byte* ctor)
{
    constexpr std::size_t kCtorBytes = 0x6000;
    if (ctor == nullptr || !memory::isReadable(ctor, kCtorBytes)) {
        log().warn(L"ContainerUi: the controller constructor could not be read; no bindings");
        return;
    }
    auto pick = [ctor](std::uint32_t a, std::uint32_t b) -> void* {
        const std::vector<const std::byte*> xs = callsAfterImmediate(ctor, kCtorBytes, a);
        const std::vector<const std::byte*> ys = callsAfterImmediate(ctor, kCtorBytes, b);
        const std::byte* found = nullptr;
        for (const std::byte* x : xs) {
            if (std::find(ys.begin(), ys.end(), x) == ys.end() || x == found) {
                continue;
            }
            if (found != nullptr) {
                return nullptr;
            }
            found = x;
        }
        return const_cast<std::byte*>(found);
    };
    g_bindBool = reinterpret_cast<BindRegFn>(pick(buttonId("#classic_stack_splitting_overlay_visible"),
                                                  buttonId("#pocket_stack_splitting_overlay_visible")));
    g_bindInt = reinterpret_cast<BindRegFn>(
        pick(buttonId("#inventory_selected_item"), buttonId("#inventory_selected_item_color")));
    g_bindText = reinterpret_cast<BindRegFn>(pick(buttonId("#progressive_select_text"),
                                                  buttonId("#inventory_selected_item_stack_count")));
    g_regClick = reinterpret_cast<ClickRegFn>(
        pick(buttonId("button.scroll_up"), buttonId("button.scroll_down")));
    g_regEvent = reinterpret_cast<EventRegFn>(
        pick(buttonId("button.shape_drawing"), buttonId("button.container_reset_held")));
    g_bindCollInt = reinterpret_cast<BindRegFn>(pick(buttonId("#item_id_aux"), buttonId("#charged_item")));
    if (g_bindCollInt != nullptr && g_bindCollInt == g_bindInt) {
        g_bindCollInt = nullptr;
    }

    if (const auto* ops = static_cast<const std::uintptr_t*>(
            opsBeforeImmediate(ctor, kCtorBytes, buttonId("#inventory_selected_item")))) {
        if (memory::isReadable(ops, 24) && memory::inGameModule(ops)) {
            const auto* invoke = reinterpret_cast<const std::uint8_t*>(ops[2]);
            constexpr std::uint8_t kPat[] = {0x41, 0x0F, 0xB7, 0x12, 0x48,
                                             0x8B, 0x01, 0x48, 0x8B, 0x80};
            if (invoke != nullptr && memory::inGameModule(invoke)
                && memory::isReadable(invoke, 0x200)) {
                for (std::size_t i = 0; i + sizeof(kPat) + 4 <= 0x200; ++i) {
                    if (std::memcmp(invoke + i, kPat, sizeof(kPat)) == 0) {
                        std::int32_t disp = 0;
                        std::memcpy(&disp, invoke + i + sizeof(kPat), 4);
                        if (disp > 0 && disp < 0x1000 && disp % 8 == 0) {
                            g_idAuxSlot = disp;
                        }
                        break;
                    }
                }
            }
        }
    }
    if (const auto* ops = static_cast<const std::uintptr_t*>(
            opsBeforeImmediate(ctor, kCtorBytes, buttonId("#item_id_aux")))) {
        if (memory::isReadable(ops, 24) && memory::inGameModule(ops)) {
            const auto* invoke = reinterpret_cast<const std::uint8_t*>(ops[2]);
            if (invoke != nullptr && memory::inGameModule(invoke) && memory::isReadable(invoke, 0x40)) {
                for (std::size_t i = 0; i + 5 <= 0x40; ++i) {
                    if (invoke[i] != 0xE8) {
                        continue;
                    }
                    std::int32_t rel = 0;
                    std::memcpy(&rel, invoke + i + 1, 4);
                    const auto* target = invoke + i + 5 + rel;
                    if (memory::inGameModule(target) && memory::isExecutable(target, 1)) {
                        g_screenGetItem = reinterpret_cast<ScreenGetItemFn>(const_cast<std::uint8_t*>(target));
                    }
                    break;
                }
            }
        }
    }
    g_bindReady = g_bindBool != nullptr && g_bindInt != nullptr && g_bindText != nullptr
                  && g_regClick != nullptr && g_regEvent != nullptr;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto rva = [base](const void* p) -> std::uintptr_t {
        return p != nullptr ? reinterpret_cast<std::uintptr_t>(p) - base : 0;
    };
    log().info(L"ContainerUi: bindings {} (bool {:#x} int {:#x} text {:#x} click {:#x} event {:#x} "
               L"idAux vtable +{:#x} screen item {:#x} collection int {:#x})",
               g_bindReady ? L"ready" : L"NOT usable",
               rva(reinterpret_cast<const void*>(g_bindBool)),
               rva(reinterpret_cast<const void*>(g_bindInt)),
               rva(reinterpret_cast<const void*>(g_bindText)),
               rva(reinterpret_cast<const void*>(g_regClick)),
               rva(reinterpret_cast<const void*>(g_regEvent)), g_idAuxSlot,
               rva(reinterpret_cast<const void*>(g_screenGetItem)),
               rva(reinterpret_cast<const void*>(g_bindCollInt)));
}

bool readPointer(const void* at, void*& out)
{
    std::uint64_t v = 0;
    if (!readQwordGuarded(at, v)) {
        return false;
    }
    out = reinterpret_cast<void*>(v);
    return true;
}

void* g_constructed = nullptr;

void adoptController(void* ctrl)
{
    void* const sm = static_cast<std::byte*>(ctrl) + g_smOffset;
    if (sm == g_screen.sm && g_screen.ctrl != nullptr) {
        return;
    }
    if (g_screen.ctrl != nullptr) {
        forgetScreen();
    }
    void* mc = nullptr;
    void* vt = nullptr;
    void* mcVt = nullptr;
    if (!readPointer(static_cast<std::byte*>(ctrl) + g_mcOffset, mc) || mc == nullptr
        || !readPointer(ctrl, vt) || vt == nullptr || !memory::inGameModule(vt)
        || !readPointer(mc, mcVt) || mcVt == nullptr || !memory::inGameModule(mcVt)) {
        return;
    }
    g_screen.sm = sm;
    g_screen.ctrl = ctrl;
    g_screen.mc = mc;
    g_screen.kind = reinterpret_cast<std::uintptr_t>(vt);
    g_screen.aliveMs = GetTickCount64();
    static const std::string kNone = "tk_none_collection";
    bool faulted = false;
    g_screen.emptyItem = callGetItemGuarded(mc, &kNone, 0, faulted);
    if (faulted || g_screen.emptyItem == nullptr || !memory::inGameModule(g_screen.emptyItem)) {
        if (faulted) {
            noteFault(L"getItem");
        }
        g_screen = ScreenState{};
        return;
    }
    if (!g_loggedScreen.exchange(true)) {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        log().info(L"ContainerUi: a container screen (controller vtable RVA {:#x}, state machine "
                   L"+{:#x}, manager +{:#x})",
                   g_screen.kind - base, g_smOffset, g_mcOffset);
    }
}

void adoptScreen(void* sm)
{
    adoptController(static_cast<std::byte*>(sm) - g_smOffset);
}

void updatePitch(const std::string& coll, int index, POINT cursor)
{
    if (coll == g_lastPitchColl && g_lastPitchIndex >= 0 && std::abs(index - g_lastPitchIndex) == 1
        && (index / 9) == (g_lastPitchIndex / 9)) {
        const int dx = std::abs(cursor.x - g_lastPitchCursor.x);
        const int dy = std::abs(cursor.y - g_lastPitchCursor.y);
        if (dx >= 8 && dx <= 400 && dy * 3 < dx) {
            g_pitch = (g_pitch == 0) ? dx : (g_pitch * 3 + dx) / 4;
        }
    }
    g_lastPitchColl = coll;
    g_lastPitchIndex = index;
    g_lastPitchCursor = cursor;
}

}

void setListener(Listener* listener)
{
    g_listener = listener;
}

void addObserver(Listener* observer)
{
    for (Listener*& slot : g_observers) {
        if (slot == observer) {
            return;
        }
    }
    for (Listener*& slot : g_observers) {
        if (slot == nullptr) {
            slot = observer;
            return;
        }
    }
    log().warn(L"ContainerUi: no room for another observer ({})", kMaxObservers);
}

void removeObserver(Listener* observer)
{
    for (Listener*& slot : g_observers) {
        if (slot == observer) {
            slot = nullptr;
        }
    }
}

void onScansReady()
{
    if (g_ready) {
        return;
    }
    Scanner& scanner = Scanner::instance();
    g_getItem = scanner.addressAs<GetItemFn>(Target::ContainerGetItem);
    g_isNull = scanner.addressAs<IsNullFn>(Target::ItemStackIsNull);
    g_maxStack = scanner.addressAs<MaxStackFn>(Target::ItemStackMaxStackSize);
    g_matches = scanner.addressAs<MatchesFn>(Target::ItemStackMatches);
    if (std::byte* wrap = scanner.address(Target::ItemStackMatchesWrapper)) {
        g_matchFlags = memory::ripTarget(wrap, 15);
    }
    if (std::byte* site = scanner.address(Target::ContainerSmOffsetSite)) {
        std::int32_t disp = 0;
        if (memory::isReadable(site + 0x18, 4)) {
            std::memcpy(&disp, site + 0x18, 4);
            g_smOffset = disp;
        }
    }
    if (std::byte* site = scanner.address(Target::ContainerMcOffsetSite)) {
        std::int32_t disp = 0;
        if (memory::isReadable(site + 3, 4)) {
            std::memcpy(&disp, site + 3, 4);
            g_mcOffset = disp;
        }
    }
    g_storageInfo = scanner.addressAs<StorageInfoFn>(Target::ItemStorageInfo);
    g_compoundGet = scanner.addressAs<CompoundGetFn>(Target::CompoundTagGet);
    if (const std::byte* fn = reinterpret_cast<const std::byte*>(g_storageInfo);
        fn != nullptr && memory::isReadable(fn, 0x200)) {
        for (std::size_t k = 0; k + 9 <= 0x200; ++k) {
            const auto b = [&](std::size_t i) { return static_cast<std::uint8_t>(fn[k + i]); };
            if (b(0) == 0x48 && b(1) == 0x8B && b(2) == 0x4B && b(4) == 0x48 && b(5) == 0x85 && b(6) == 0xC9
                && b(7) == 0x0F && b(8) == 0x84) {
                g_stackUserDataOffset = b(3);
                break;
            }
        }
    }
    if (const std::byte* site = scanner.address(Target::ItemCreativeCategoryStore);
        site != nullptr && memory::isReadable(site + 0x35, 4)) {
        std::int32_t disp = 0;
        std::memcpy(&disp, site + 0x35, 4);
        if (disp > 0x40 && disp < 0x400) {
            g_itemCategoryOffset = disp;
        }
    }
    const bool offsetsOk = g_smOffset > 0 && g_smOffset < 0x4000 && g_mcOffset > 0
                           && g_mcOffset < 0x4000;
    g_ready = g_getItem != nullptr && g_isNull != nullptr && g_maxStack != nullptr
              && g_matches != nullptr && g_matchFlags != nullptr && offsetsOk
              && scanner.found(Target::ContainerSmHandle);
    if (g_ready) {
        log().info(L"ContainerUi: ready (state machine +{:#x}, manager +{:#x}; storage {} nbt {} +{:#x}; "
                   L"item category +{:#x})",
                   g_smOffset, g_mcOffset, g_storageInfo != nullptr, g_compoundGet != nullptr,
                   g_stackUserDataOffset, g_itemCategoryOffset);
    } else {
        log().warn(L"ContainerUi: not usable (getItem {} isNull {} max {} match {} flags {} "
                   L"sm +{:#x} mc +{:#x})",
                   g_getItem != nullptr, g_isNull != nullptr, g_maxStack != nullptr,
                   g_matches != nullptr, g_matchFlags != nullptr, g_smOffset, g_mcOffset);
    }
}

bool available()
{
    return g_ready;
}

void setSmOriginal(SmHandleFn original)
{
    g_smOriginal = original;
}

bool onSmHandle(void* sm, std::uint32_t id, int state, const void* coll, int index, int& result)
{
    if (!g_ready || sm == nullptr || g_synthDepth.load(std::memory_order_relaxed) > 0) {
        return false;
    }
    char name[64]{};
    if (coll != nullptr && !readStdStringGuarded(coll, name, sizeof(name))) {
        return false;
    }
    adoptScreen(sm);
    if (g_screen.sm != sm) {
        return false;
    }
    ++g_stats.smEvents;
    const std::string collName(name);
    if (id == button::kHover && state == kStateHeld && index >= 0 && !collName.empty()) {
        ++g_stats.hoverEvents;
        POINT cursor{};
        GetCursorPos(&cursor);
        g_hoverColl = collName;
        g_hoverIndex = index;
        g_hoverCursor = cursor;
        g_hoverMs = GetTickCount64();
        g_screen.positions[SlotKey{collName, index}] = cursor;
        updatePitch(collName, index, cursor);
    }
    for (Listener* const one : g_observers) {
        if (one != nullptr) {
            one->onSlotButton(id, state, collName, index);
        }
    }
    if (g_listener != nullptr && g_listener->onSlotButton(id, state, collName, index)) {
        result = 0;
        return true;
    }
    if (id != button::kHover) {
        if (state == kStatePressed) {
            g_pending.id = id;
            g_pending.coll = collName;
            g_pending.index = index;
        } else if (state == kStateReleased && g_pending.id == id) {
            g_pending = PendingPress{};
        }
    }
    return false;
}

std::uint32_t onScreenTickHook(void* ctrl)
{
    ++g_stats.tickCalls;
    if (!g_ready || ctrl == nullptr) {
        return 0;
    }
    if (g_screen.ctrl == nullptr && ctrl == g_constructed) {
        adoptController(ctrl);
    }
    if (ctrl != g_screen.ctrl) {
        return 0;
    }
    g_screen.aliveMs = GetTickCount64();
    g_screen.aliveNow = true;
    ++g_stats.ticks;
    if (g_listener != nullptr) {
        g_listener->onScreenTick();
    }
    for (Listener* const one : g_observers) {
        if (one != nullptr) {
            one->onScreenTick();
        }
    }
    g_screen.aliveNow = false;
    const bool refresh = g_refresh;
    g_refresh = false;
    return refresh ? 1u : 0u;
}

void setCtorAddress(const void* ctor)
{
    resolveBinders(static_cast<const std::byte*>(ctor));
}

void setHudCtorAddress(const void* ctor)
{
    constexpr std::size_t kHudCtorBytes = 0x5000;
    const auto* const begin = static_cast<const std::byte*>(ctor);
    if (begin == nullptr || !memory::isReadable(begin, kHudCtorBytes)) {
        log().warn(L"ContainerUi: the HUD controller constructor could not be read; no float bindings");
        return;
    }
    const std::vector<const std::byte*> xs =
        callsAfterImmediate(begin, kHudCtorBytes, buttonId("#hotbar_offset_x"));
    const std::vector<const std::byte*> ys =
        callsAfterImmediate(begin, kHudCtorBytes, buttonId("#hotbar_offset_y"));
    const std::byte* found = nullptr;
    int candidates = 0;
    for (const std::byte* x : xs) {
        if (x == found || std::find(ys.begin(), ys.end(), x) == ys.end()) {
            continue;
        }
        found = x;
        ++candidates;
    }
    auto* const fn = reinterpret_cast<BindRegFn>(const_cast<std::byte*>(found));
    if (candidates != 1 || fn == g_bindBool || fn == g_bindInt || fn == g_bindText) {
        log().warn(L"ContainerUi: the float binder was not found in the HUD constructor ({} candidate(s))",
                   candidates);
        return;
    }
    g_bindFloat = fn;
    log().info(L"ContainerUi: float binder at RVA {:#x} (HUD constructor)",
               reinterpret_cast<std::uintptr_t>(found)
                   - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
}

namespace {
std::atomic<std::ptrdiff_t> g_hudMcOffset{-1};
std::atomic<bool> g_hudMcWarned{false};
}

void setHudGetItemSite(const void* site, const void* getItem)
{
    const auto* const at = static_cast<const std::byte*>(site);
    constexpr std::size_t kSiteBytes = 18;
    if (at == nullptr || getItem == nullptr || !memory::isReadable(at, kSiteBytes)) {
        log().warn(L"ContainerUi: the HUD getItem site could not be read; the offhand slot stays empty");
        return;
    }
    std::int32_t disp = 0;
    std::int32_t rel = 0;
    std::memcpy(&disp, at + 3, sizeof(disp));
    std::memcpy(&rel, at + 14, sizeof(rel));
    const std::byte* const callee = at + kSiteBytes + rel;
    if (callee != static_cast<const std::byte*>(getItem)) {
        log().warn(L"ContainerUi: the HUD getItem site does not call ContainerGetItem (RVA {:#x}); not used",
                   reinterpret_cast<std::uintptr_t>(callee)
                       - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
        return;
    }
    if (disp <= 0 || disp >= 0x4000 || (disp % 8) != 0) {
        log().warn(L"ContainerUi: the HUD container manager offset {:#x} looks wrong; not used", disp);
        return;
    }
    g_hudMcOffset.store(disp, std::memory_order_release);
    log().info(L"ContainerUi: HUD container manager at ctrl +{:#x} (site RVA {:#x})", disp,
               reinterpret_cast<std::uintptr_t>(at) - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
}

void* hudContainerManager(void* ctrl)
{
    const std::ptrdiff_t offset = g_hudMcOffset.load(std::memory_order_acquire);
    if (offset < 0 || ctrl == nullptr) {
        return nullptr;
    }
    void* mc = nullptr;
    void* vt = nullptr;
    if (!readPointer(static_cast<std::byte*>(ctrl) + offset, mc) || !memory::plausiblePointer(mc)
        || !readPointer(mc, vt) || vt == nullptr || !memory::inGameModule(vt)) {
        if (!g_hudMcWarned.exchange(true, std::memory_order_relaxed)) {
            log().warn(L"ContainerUi: the HUD container manager at ctrl +{:#x} could not be read", offset);
        }
        return nullptr;
    }
    return mc;
}

void addHudObserver(HudCreatedFn fn)
{
    const int n = g_hudObserverCount.load(std::memory_order_acquire);
    if (fn == nullptr || n >= kMaxHudObservers) {
        return;
    }
    for (int i = 0; i < n; ++i) {
        if (g_hudObservers[i].load(std::memory_order_relaxed) == fn) {
            return;
        }
    }
    g_hudObservers[n].store(fn, std::memory_order_relaxed);
    g_hudObserverCount.store(n + 1, std::memory_order_release);
}

void onHudConstructed(void* ctrl)
{
    if (ctrl == nullptr || !g_bindReady) {
        return;
    }
    const int n = g_hudObserverCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (const HudCreatedFn fn = g_hudObservers[i].load(std::memory_order_relaxed)) {
            fn(ctrl);
        }
    }
}

void onScreenConstructed(void* ctrl)
{
    if (!g_ready || ctrl == nullptr) {
        return;
    }
    g_constructed = ctrl;
    if (g_listener != nullptr && g_bindReady) {
        g_listener->onScreenCreated(ctrl);
    }
    if (g_bindReady) {
        for (Listener* const one : g_observers) {
            if (one != nullptr) {
                one->onScreenCreated(ctrl);
            }
        }
    }
}

bool bindingsAvailable()
{
    return g_bindReady;
}

namespace {
bool bindWith(BindRegFn reg, const CallableOps& ops, const char* kind, void* ctrl, const char* name,
              std::uintptr_t fn, std::uintptr_t arg)
{
    if (!g_bindReady || reg == nullptr || ctrl == nullptr || name == nullptr) {
        return false;
    }
    const int slot = slotFor(kind, name, fn, arg);
    if (slot < 0) {
        return false;
    }
    const std::uint32_t hash = buttonId(name);
    GameCallable getter;
    getter.ops = &ops;
    getter.capture = &g_slots[slot];
    GameCallable condition;
    condition.ops = &kTrueOps;
    if (!callBindGuarded(reg, ctrl, &hash, &getter, &condition)) {
        noteFault(L"binding registration");
        return false;
    }
    return true;
}
}

bool bindBool(void* ctrl, const char* name, BoolGetter fn, std::uintptr_t arg)
{
    return bindWith(g_bindBool, kBoolOps, "b", ctrl, name, reinterpret_cast<std::uintptr_t>(fn),
                    arg);
}
bool bindInt(void* ctrl, const char* name, IntGetter fn, std::uintptr_t arg)
{
    return bindWith(g_bindInt, kIntOps, "i", ctrl, name, reinterpret_cast<std::uintptr_t>(fn), arg);
}
bool floatBindingsAvailable()
{
    return g_bindReady && g_bindFloat != nullptr;
}

namespace {

struct PersistentCode {
    std::uint8_t boolRead[8];
    std::uint8_t floatRead[12];
    std::uint8_t alwaysTrue[4];
    std::uint8_t move[12];
    std::uint8_t destroy[4];
    std::uint8_t textTramp[8];
    std::uint8_t emptyText[32];
    alignas(8) const void* boolOps[3];
    const void* floatOps[3];
    const void* trueOps[3];
    const void* textOps[3];
};
struct PersistentValues {
    std::uint8_t bools[kPersistentSlots];
    float floats[kPersistentSlots];
};
struct TextSlot {
    void* fn;
    volatile std::uint64_t length;
    char text[kPersistentTextBytes - 16];
};
static_assert(sizeof(TextSlot) == kPersistentTextBytes);
struct PersistentText {
    TextSlot slots[kPersistentTextSlots];
};
static_assert(sizeof(PersistentText) <= 4096 * 3);

PersistentCode* g_persistCode = nullptr;
PersistentValues* g_persistValues = nullptr;
PersistentText* g_persistText = nullptr;
std::once_flag g_persistOnce;
std::atomic<int> g_textInside{0};

void makePersistent()
{
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const SIZE_T page = info.dwPageSize;
    static_assert(sizeof(PersistentCode) < 4096 && sizeof(PersistentValues) < 4096);
    auto* const base = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, page * 5, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (base == nullptr) {
        log().warn(L"ContainerUi: could not allocate the persistent bindings (error {})", GetLastError());
        return;
    }
    auto* const code = reinterpret_cast<PersistentCode*>(base);
    constexpr std::uint8_t kBoolRead[] = {0x48, 0x8B, 0x41, 0x08, 0x0F, 0xB6, 0x00, 0xC3};
    constexpr std::uint8_t kFloatRead[] = {0x48, 0x8B, 0x41, 0x08, 0xF3, 0x0F, 0x10, 0x00, 0xC3};
    constexpr std::uint8_t kTrue[] = {0xB0, 0x01, 0xC3};
    constexpr std::uint8_t kMove[] = {0x48, 0x8B, 0x41, 0x08, 0x48, 0x89, 0x42, 0x08, 0xC3};
    constexpr std::uint8_t kRet[] = {0xC3};
    constexpr std::uint8_t kTextTramp[] = {0x48, 0x8B, 0x42, 0x08, 0xFF, 0x20};
    constexpr std::uint8_t kEmptyText[] = {0x31, 0xC0, 0x48, 0x89, 0x01, 0x48, 0x89, 0x41, 0x08,
                                          0x48, 0x89, 0x41, 0x10, 0x48, 0xC7, 0x41, 0x18, 0x0F,
                                          0x00, 0x00, 0x00, 0x48, 0x89, 0xC8, 0xC3};
    std::memcpy(code->boolRead, kBoolRead, sizeof(kBoolRead));
    std::memcpy(code->floatRead, kFloatRead, sizeof(kFloatRead));
    std::memcpy(code->alwaysTrue, kTrue, sizeof(kTrue));
    std::memcpy(code->move, kMove, sizeof(kMove));
    std::memcpy(code->destroy, kRet, sizeof(kRet));
    std::memcpy(code->textTramp, kTextTramp, sizeof(kTextTramp));
    std::memcpy(code->emptyText, kEmptyText, sizeof(kEmptyText));
    code->boolOps[0] = code->move;
    code->boolOps[1] = code->destroy;
    code->boolOps[2] = code->boolRead;
    code->floatOps[0] = code->move;
    code->floatOps[1] = code->destroy;
    code->floatOps[2] = code->floatRead;
    code->trueOps[0] = code->move;
    code->trueOps[1] = code->destroy;
    code->trueOps[2] = code->alwaysTrue;
    code->textOps[0] = code->move;
    code->textOps[1] = code->destroy;
    code->textOps[2] = code->textTramp;
    DWORD old = 0;
    if (!VirtualProtect(base, page, PAGE_EXECUTE_READ, &old)) {
        log().warn(L"ContainerUi: could not make the persistent bindings executable (error {})", GetLastError());
        VirtualFree(base, 0, MEM_RELEASE);
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), base, page);
    g_persistValues = reinterpret_cast<PersistentValues*>(base + page);
    auto* const text = reinterpret_cast<PersistentText*>(base + page * 2);
    for (int i = 0; i < kPersistentTextSlots; ++i) {
        text->slots[i].fn = code->emptyText;
        text->slots[i].length = 0;
        text->slots[i].text[0] = '\0';
    }
    g_persistText = text;
    g_persistCode = code;
}

bool persistentReady()
{
    std::call_once(g_persistOnce, &makePersistent);
    return g_persistCode != nullptr && g_persistValues != nullptr;
}

bool bindPersistent(BindRegFn reg, const void* const* ops, void* ctrl, const char* name, const volatile void* value)
{
    if (!g_bindReady || reg == nullptr || ctrl == nullptr || name == nullptr || value == nullptr) {
        return false;
    }
    const std::uint32_t hash = buttonId(name);
    GameCallable getter;
    getter.ops = ops;
    getter.capture = const_cast<void*>(value);
    GameCallable condition;
    condition.ops = g_persistCode->trueOps;
    if (!callBindGuarded(reg, ctrl, &hash, &getter, &condition)) {
        noteFault(L"persistent binding registration");
        return false;
    }
    return true;
}

TextSlot* textSlot(int slot)
{
    if (slot < 0 || slot >= kPersistentTextSlots || !persistentReady()) {
        return nullptr;
    }
    return &g_persistText->slots[slot];
}

void* __fastcall invokePersistentText(void* ret, GameCallable* self)
{
    g_textInside.fetch_add(1, std::memory_order_acquire);

    char text[kPersistentTextBytes - 16]{};
    std::uint64_t length = 0;
    if (self != nullptr && self->capture != nullptr) {
        const auto* const slot = static_cast<const TextSlot*>(self->capture);
        length = slot->length;
        if (length > sizeof(text) - 1) {
            length = sizeof(text) - 1;
        }
        std::memcpy(text, slot->text, static_cast<std::size_t>(length));
    }
    text[length] = '\0';

    auto* const out = static_cast<unsigned char*>(ret);
    std::memset(out, 0, 0x20);
    void* buf = nullptr;
    if (length > 15) {
        buf = hooks::callGameAllocate(static_cast<std::size_t>(length) + 1);
        if (buf == nullptr) {
            length = 15;
            text[length] = '\0';
        }
    }
    if (buf != nullptr) {
        std::memcpy(buf, text, static_cast<std::size_t>(length));
        static_cast<char*>(buf)[length] = '\0';
        const auto ptr = reinterpret_cast<std::uintptr_t>(buf);
        std::memcpy(out, &ptr, sizeof(ptr));
        std::memcpy(out + 0x10, &length, sizeof(length));
        std::memcpy(out + 0x18, &length, sizeof(length));
    } else {
        std::memcpy(out, text, static_cast<std::size_t>(length));
        const std::uint64_t capacity = 15;
        std::memcpy(out + 0x10, &length, sizeof(length));
        std::memcpy(out + 0x18, &capacity, sizeof(capacity));
    }

    g_textInside.fetch_sub(1, std::memory_order_release);
    return ret;
}

}

volatile std::uint8_t* persistentBool(int slot)
{
    if (slot < 0 || slot >= kPersistentSlots || !persistentReady()) {
        return nullptr;
    }
    return &g_persistValues->bools[slot];
}

volatile float* persistentFloat(int slot)
{
    if (slot < 0 || slot >= kPersistentSlots || !persistentReady()) {
        return nullptr;
    }
    return &g_persistValues->floats[slot];
}

bool bindPersistentBool(void* ctrl, const char* name, int slot)
{
    volatile std::uint8_t* const value = persistentBool(slot);
    return value != nullptr && bindPersistent(g_bindBool, g_persistCode->boolOps, ctrl, name, value);
}

bool bindPersistentFloat(void* ctrl, const char* name, int slot)
{
    volatile float* const value = persistentFloat(slot);
    return value != nullptr && bindPersistent(g_bindFloat, g_persistCode->floatOps, ctrl, name, value);
}

bool writePersistentText(int slot, const char* text, std::size_t length)
{
    TextSlot* const s = textSlot(slot);
    if (s == nullptr || text == nullptr) {
        return false;
    }
    if (length > sizeof(s->text) - 1) {
        length = sizeof(s->text) - 1;
    }
    s->length = 0;
    std::memcpy(s->text, text, length);
    s->text[length] = '\0';
    s->length = length;
    return true;
}

bool bindPersistentText(void* ctrl, const char* name, int slot)
{
    TextSlot* const s = textSlot(slot);
    if (s == nullptr) {
        return false;
    }
    InterlockedExchangePointer(&s->fn, reinterpret_cast<void*>(&invokePersistentText));
    return bindPersistent(g_bindText, g_persistCode->textOps, ctrl, name, s);
}

void detachPersistentText()
{
    if (g_persistText == nullptr || g_persistCode == nullptr) {
        return;
    }
    for (int i = 0; i < kPersistentTextSlots; ++i) {
        InterlockedExchangePointer(&g_persistText->slots[i].fn, g_persistCode->emptyText);
        g_persistText->slots[i].length = 0;
        g_persistText->slots[i].text[0] = '\0';
    }
    for (int waited = 0; waited < 200 && g_textInside.load(std::memory_order_acquire) != 0; ++waited) {
        Sleep(1);
    }
    if (g_textInside.load(std::memory_order_acquire) != 0) {
        log().warn(L"ContainerUi: a persistent text getter is still running after 200 ms");
    }
}
bool bindText(void* ctrl, const char* name, TextGetter fn, std::uintptr_t arg)
{
    return bindWith(g_bindText, kTextOps, "t", ctrl, name, reinterpret_cast<std::uintptr_t>(fn),
                    arg);
}

bool onButtonPressed(void* ctrl, const char* buttonName, ButtonHandler fn, std::uintptr_t arg)
{
    if (!g_bindReady || ctrl == nullptr || buttonName == nullptr) {
        return false;
    }
    const int slot = slotFor("p", buttonName, reinterpret_cast<std::uintptr_t>(fn), arg);
    if (slot < 0) {
        return false;
    }
    GameCallable handler;
    handler.ops = &kButtonOps;
    handler.capture = &g_slots[slot];
    if (!callClickRegGuarded(g_regClick, ctrl, buttonId(buttonName), &handler)) {
        noteFault(L"button registration");
        return false;
    }
    return true;
}

bool onButtonHovered(void* ctrl, const char* buttonName, ButtonHandler fn, std::uintptr_t arg)
{
    if (!g_bindReady || ctrl == nullptr || buttonName == nullptr) {
        return false;
    }
    const int slot = slotFor("h", buttonName, reinterpret_cast<std::uintptr_t>(fn), arg);
    if (slot < 0) {
        return false;
    }
    GameCallable first;
    first.ops = &kButtonOps;
    first.capture = &g_slots[slot];
    GameCallable second = first;
    const std::uint32_t id = buttonId(buttonName);
    if (!callEventRegGuarded(g_regEvent, ctrl, id, false, 1, &first)
        || !callEventRegGuarded(g_regEvent, ctrl, id, true, 0, &second)) {
        noteFault(L"hover registration");
        return false;
    }
    return true;
}

void requestRefresh()
{
    g_refresh = true;
}

bool collectionBindingsAvailable()
{
    return g_bindReady && g_bindCollInt != nullptr && g_screenGetItem != nullptr;
}

bool bindCollectionInt(void* ctrl, const char* name, CollIntGetter fn, std::uintptr_t arg)
{
    if (!collectionBindingsAvailable() || ctrl == nullptr || name == nullptr || fn == nullptr) {
        return false;
    }
    const std::uint32_t hash = buttonId(name);
    GameCallable getter;
    getter.ops = &kCollIntOps;
    getter.capture = ctrl;
    getter.spare[0] = reinterpret_cast<std::uintptr_t>(fn);
    getter.spare[1] = arg;
    GameCallable condition;
    condition.ops = &kTrueOps;
    if (!callBindGuarded(g_bindCollInt, ctrl, &hash, &getter, &condition)) {
        noteFault(L"collection binding registration");
        return false;
    }
    return true;
}

const void* screenStackOf(void* ctrl, const std::string& coll, int index)
{
    if (g_screenGetItem == nullptr || ctrl == nullptr || index < 0) {
        return nullptr;
    }
    bool faulted = false;
    const void* one = callScreenGetItemGuarded(reinterpret_cast<const void*>(g_screenGetItem), ctrl, &coll,
                                               index, faulted);
    if (faulted) {
        noteFault(L"screen getItem (collection value)");
        return nullptr;
    }
    return one;
}

namespace {

using ItemMaxStackFn = int(__fastcall*)(const void* item, const void* descriptor);
std::atomic<int> g_itemMaxStackSlot{-2};

int callItemMaxStackGuarded(ItemMaxStackFn fn, const void* item, const void* descriptor, bool& faulted)
{
    faulted = false;
    __try {
        return fn(item, descriptor);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return 0;
    }
}

int findItemMaxStackSlot()
{
    const auto* fn = reinterpret_cast<const std::uint8_t*>(g_maxStack);
    if (fn == nullptr || !memory::isReadable(fn, 0x80)) {
        return -1;
    }
    for (std::size_t i = 0; i + 10 <= 0x80; ++i) {
        if (fn[i] == 0x48 && fn[i + 1] == 0x8B && fn[i + 2] == 0x06 && fn[i + 3] == 0x48 && fn[i + 4] == 0x8B
            && fn[i + 5] == 0x80) {
            std::int32_t disp = 0;
            std::memcpy(&disp, fn + i + 6, 4);
            if (disp > 0 && disp < 0x1000 && disp % 8 == 0) {
                log().info(L"ContainerUi: Item max stack size = vtable +{:#x}", disp);
                return disp;
            }
        }
    }
    log().warn(L"ContainerUi: Item max stack size slot not found (full shulker icons are off)");
    return -1;
}

}

int maxStackOfItem(const void* item)
{
    if (item == nullptr) {
        return 0;
    }
    int slot = g_itemMaxStackSlot.load();
    if (slot == -2) {
        slot = findItemMaxStackSlot();
        g_itemMaxStackSlot.store(slot);
    }
    if (slot < 0) {
        return 0;
    }
    void* vt = nullptr;
    void* fn = nullptr;
    if (!readPointer(item, vt) || vt == nullptr || !memory::inGameModule(vt)
        || !readPointer(static_cast<const std::byte*>(vt) + slot, fn) || fn == nullptr
        || !memory::inGameModule(fn)) {
        return 0;
    }
    alignas(16) std::byte descriptor[64]{};
    bool faulted = false;
    const int max = callItemMaxStackGuarded(reinterpret_cast<ItemMaxStackFn>(fn), item, descriptor, faulted);
    if (faulted) {
        noteFault(L"Item::getMaxStackSize");
        return 0;
    }
    return (max >= 1 && max <= 255) ? max : 0;
}

int idAuxOfItem(const void* item, int aux)
{
    if (g_idAuxSlot < 0 || item == nullptr || !memory::isReadable(item, 8)) {
        return 0;
    }
    std::uint64_t vt = 0;
    if (!readQwordGuarded(item, vt) || vt == 0
        || !memory::inGameModule(reinterpret_cast<const void*>(vt))) {
        return 0;
    }
    std::uint64_t fn = 0;
    if (!readQwordGuarded(reinterpret_cast<const std::byte*>(vt) + g_idAuxSlot, fn) || fn == 0
        || !memory::inGameModule(reinterpret_cast<const void*>(fn))) {
        return 0;
    }
    bool faulted = false;
    const int value = callIdAuxGuarded(reinterpret_cast<IdAuxFn>(fn), item,
                                       static_cast<std::uint32_t>(aux) & 0xFFFFu, faulted);
    if (faulted) {
        noteFault(L"item icon id");
        return 0;
    }
    return value;
}

void onScreenDestroyed(void* ctrl)
{
    if (ctrl != nullptr && ctrl == g_constructed) {
        g_constructed = nullptr;
    }
    if (ctrl != nullptr && ctrl == g_screen.ctrl) {
        forgetScreen();
    }
}

Stats stats()
{
    return g_stats;
}

bool hasScreen()
{
    return g_ready && g_screen.ctrl != nullptr && g_screen.mc != nullptr;
}

void* screenController()
{
    return hasScreen() ? g_screen.ctrl : nullptr;
}

std::uintptr_t screenKind()
{
    return hasScreen() ? g_screen.kind : 0;
}

int collectionSize(const std::string& coll)
{
    if (!hasScreen()) {
        return 0;
    }
    auto it = g_screen.sizes.find(coll);
    if (it != g_screen.sizes.end()) {
        return it->second;
    }
    const std::string& name = intern(coll);
    int size = 0;
    for (; size < kMaxCollection; ++size) {
        bool faulted = false;
        const void* item = callGetItemGuarded(g_screen.mc, &name, size, faulted);
        if (faulted) {
            noteFault(L"getItem");
            size = 0;
            break;
        }
        if (item == nullptr || item == g_screen.emptyItem) {
            break;
        }
    }
    if (size >= kMaxCollection) {
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 4) {
            log().warn(L"ContainerUi: could not count the slots of \"{}\"; not used",
                       std::wstring(coll.begin(), coll.end()));
        }
        size = 0;
    }
    g_screen.sizes.emplace(coll, size);
    return size;
}

const void* stackAt(const std::string& coll, int index)
{
    if (!hasScreen() || index < 0) {
        return nullptr;
    }
    const std::string& name = intern(coll);
    bool faulted = false;
    if (g_screenGetItem != nullptr) {
        if (index >= collectionSize(coll)) {
            return nullptr;
        }
        const void* shown = callScreenGetItemGuarded(
            reinterpret_cast<const void*>(g_screenGetItem), g_screen.ctrl, &name, index, faulted);
        if (!faulted && shown != nullptr) {
            return shown;
        }
        if (faulted) {
            noteFault(L"screen getItem");
        }
        faulted = false;
    }
    const void* item = callGetItemGuarded(g_screen.mc, &name, index, faulted);
    if (faulted) {
        noteFault(L"getItem");
        return nullptr;
    }
    if (item == nullptr || item == g_screen.emptyItem) {
        return nullptr;
    }
    return item;
}

const void* cursorStack()
{
    static const std::string kCursor = "cursor_items";
    return stackAt(kCursor, 0);
}

bool isEmpty(const void* stack)
{
    if (stack == nullptr || g_isNull == nullptr) {
        return true;
    }
    std::uint8_t count = 0;
    if (!readByteGuarded(static_cast<const std::byte*>(stack) + kStackCountOffset, count)
        || count == 0) {
        return true;
    }
    bool faulted = false;
    const bool empty = callIsNullGuarded(stack, faulted);
    if (faulted) {
        noteFault(L"isNull");
        return true;
    }
    return empty;
}

int countOf(const void* stack)
{
    if (isEmpty(stack)) {
        return 0;
    }
    std::uint8_t count = 0;
    readByteGuarded(static_cast<const std::byte*>(stack) + kStackCountOffset, count);
    return count;
}

int maxStackOf(const void* stack)
{
    if (isEmpty(stack) || g_maxStack == nullptr) {
        return 0;
    }
    bool faulted = false;
    const int max = callMaxStackGuarded(stack, faulted);
    if (faulted) {
        noteFault(L"getMaxStackSize");
        return 0;
    }
    return max;
}

bool sameItem(const void* a, const void* b)
{
    if (isEmpty(a) || isEmpty(b) || g_matches == nullptr) {
        return false;
    }
    bool faulted = false;
    const bool same = callMatchesGuarded(a, b, faulted);
    if (faulted) {
        noteFault(L"matches");
        return false;
    }
    if (!same) {
        return false;
    }
    return auxOf(a) == auxOf(b);
}

std::uintptr_t itemKey(const void* stack)
{
    if (isEmpty(stack)) {
        return 0;
    }
    void* weak = nullptr;
    if (!readPointer(static_cast<const std::byte*>(stack) + kStackItemOffset, weak)
        || weak == nullptr) {
        return 0;
    }
    void* item = nullptr;
    if (!readPointer(weak, item)) {
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(item);
}

std::string itemName(const void* stack)
{
    const std::uintptr_t item = itemKey(stack);
    if (item == 0) {
        return {};
    }
    char name[128]{};
    if (!readStdStringGuarded(reinterpret_cast<const void*>(item + kItemNameOffset), name,
                              sizeof(name))) {
        return {};
    }
    return name;
}

int auxOf(const void* stack)
{
    if (stack == nullptr) {
        return 0;
    }
    std::uint16_t aux = 0;
    readWordGuarded(static_cast<const std::byte*>(stack) + kStackAuxOffset, aux);
    return static_cast<std::int16_t>(aux);
}

namespace {

using RarityFn = int(__fastcall*)(const void* item);
std::atomic<int> g_raritySlot{-2};

int callRarityGuarded(RarityFn fn, const void* item, bool& faulted)
{
    faulted = false;
    __try {
        return fn(item);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return -1;
    }
}

bool readInt32Guarded(const void* at, std::int32_t& out)
{
    if (!memory::isReadable(at, 4)) {
        return false;
    }
    std::memcpy(&out, at, 4);
    return true;
}

int findRaritySlot()
{
    struct Known {
        const char* name;
        int rarity;
    };
    static const Known kKnown[] = {
        {"minecraft:dirt", 0},        {"minecraft:experience_bottle", 1}, {"minecraft:totem_of_undying", 1},
        {"minecraft:mace", 3},        {"minecraft:heavy_core", 3},        {"minecraft:elytra", 3},
        {"minecraft:dragon_egg", 3},
    };
    std::vector<std::pair<const void*, int>> items;
    for (const Known& k : kKnown) {
        if (const void* item = blocks::itemByName(k.name)) {
            items.emplace_back(item, k.rarity);
        }
    }
    if (items.size() < 4 || items.front().second != 0) {
        return -1;
    }
    void* vt = nullptr;
    if (!readPointer(items.front().first, vt) || vt == nullptr || !memory::inGameModule(vt)) {
        return -1;
    }
    constexpr int kMaxSlot = 160;
    for (int slot = 0; slot < kMaxSlot; ++slot) {
        void* fn = nullptr;
        if (!readPointer(static_cast<const std::byte*>(vt) + slot * 8, fn) || fn == nullptr
            || !memory::inGameModule(fn)) {
            break;
        }
        const auto* b = static_cast<const std::uint8_t*>(fn);
        if (!memory::isReadable(b, 7) || b[0] != 0x8B || b[1] != 0x81 || b[6] != 0xC3) {
            continue;
        }
        std::int32_t disp = 0;
        std::memcpy(&disp, b + 2, 4);
        if (disp <= 0 || disp > 0x2000) {
            continue;
        }
        bool all = true;
        for (const auto& [item, want] : items) {
            std::int32_t v = -1;
            if (!readInt32Guarded(static_cast<const std::byte*>(item) + disp, v) || v != want) {
                all = false;
                break;
            }
        }
        if (all) {
            log().info(L"ContainerUi: item rarity = vtable slot {} (+{:#x}), field +{:#x}", slot, slot * 8, disp);
            return slot;
        }
    }
    log().warn(L"ContainerUi: item rarity getter not found; sorting by rarity falls back to names");
    return -1;
}

}

int itemRarityOf(const void* item)
{
    if (item == nullptr) {
        return -1;
    }
    int slot = g_raritySlot.load();
    if (slot == -2) {
        slot = findRaritySlot();
        if (slot >= 0 || blocks::itemByName("minecraft:dirt") != nullptr) {
            g_raritySlot.store(slot);
        }
    }
    if (slot < 0) {
        return -1;
    }
    void* vt = nullptr;
    void* fn = nullptr;
    if (!readPointer(item, vt) || vt == nullptr || !memory::inGameModule(vt)
        || !readPointer(static_cast<const std::byte*>(vt) + slot * 8, fn) || fn == nullptr
        || !memory::inGameModule(fn)) {
        return -1;
    }
    bool faulted = false;
    const int r = callRarityGuarded(reinterpret_cast<RarityFn>(fn), item, faulted);
    if (faulted) {
        noteFault(L"item rarity");
        return -1;
    }
    return (r >= 0 && r <= 16) ? r : -1;
}

int rarityOf(const void* stack)
{
    const std::uintptr_t item = itemKey(stack);
    return item != 0 ? itemRarityOf(reinterpret_cast<const void*>(item)) : -1;
}

int creativeCategoryOf(const void* stack)
{
    if (g_itemCategoryOffset < 0) {
        return -1;
    }
    const std::uintptr_t item = itemKey(stack);
    std::uint8_t value = 0xFF;
    if (item == 0 || !readByteGuarded(reinterpret_cast<const std::byte*>(item) + g_itemCategoryOffset, value)) {
        return -1;
    }
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 12) {
        log().info(L"ContainerUi: creative category {} = {}", toUtf16(itemName(stack)), value);
    }
    return value <= 6 ? value : -1;
}

bool storageFill(const void* stack, int& current, int& capacity)
{
    current = 0;
    capacity = 0;
    if (g_storageInfo == nullptr || !hasScreen() || g_screen.mc == nullptr || isEmpty(stack)) {
        return false;
    }
    alignas(8) std::byte out[64]{};
    bool faulted = false;
    callStorageInfoGuarded(out, stack, g_screen.mc, faulted);
    if (faulted) {
        noteFault(L"storage info");
        return false;
    }
    if (out[0x0c] == std::byte{0}) {
        return false;
    }
    std::int32_t head = 0;
    std::memcpy(&head, out + 0x00, 4);
    std::memcpy(&current, out + 0x04, 4);
    std::memcpy(&capacity, out + 0x08, 4);
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 3) {
        log().info(L"ContainerUi: storage info {} / {} / {}", head, current, capacity);
    }
    return current >= 0;
}

namespace {

int tagType(const void* tag)
{
    std::uint8_t type = 0xFF;
    if (tag == nullptr || !readByteGuarded(static_cast<const std::byte*>(tag) + 0x28, type)) {
        return -1;
    }
    return type;
}

const void* compoundGet(const void* compound, std::string_view key)
{
    if (g_compoundGet == nullptr || compound == nullptr) {
        return nullptr;
    }
    bool faulted = false;
    const void* tag = callCompoundGetGuarded(compound, &key, faulted);
    if (faulted) {
        noteFault(L"CompoundTag::get");
        return nullptr;
    }
    return tag;
}

}

bool nbtContents(const void* stack, int& usedSlots, int& totalItems)
{
    usedSlots = 0;
    totalItems = 0;
    if (g_compoundGet == nullptr || g_stackUserDataOffset < 0 || isEmpty(stack)) {
        return false;
    }
    void* root = nullptr;
    if (!readPointer(static_cast<const std::byte*>(stack) + g_stackUserDataOffset, root) || root == nullptr) {
        return false;
    }
    void* rootVtable = nullptr;
    if (!readPointer(root, rootVtable) || rootVtable == nullptr || !memory::inGameModule(rootVtable)) {
        return false;
    }
    const void* list = compoundGet(root, "Items");
    if (list == nullptr || tagType(list) != 9) {
        return false;
    }
    void* first = nullptr;
    void* last = nullptr;
    if (!readPointer(static_cast<const std::byte*>(list) + 0x08, first)
        || !readPointer(static_cast<const std::byte*>(list) + 0x10, last)) {
        return false;
    }
    const std::ptrdiff_t bytes = static_cast<const std::byte*>(last) - static_cast<const std::byte*>(first);
    if (bytes < 0 || bytes % 8 != 0 || bytes / 8 > 256) {
        return false;
    }
    for (std::ptrdiff_t i = 0; i < bytes / 8; ++i) {
        void* elem = nullptr;
        void* vt = nullptr;
        if (!readPointer(static_cast<const std::byte*>(first) + i * 8, elem) || elem == nullptr
            || !readPointer(elem, vt) || vt != rootVtable) {
            continue;
        }
        const void* count = compoundGet(elem, "Count");
        std::uint8_t n = 0;
        if (count == nullptr || tagType(count) != 1
            || !readByteGuarded(static_cast<const std::byte*>(count) + 0x08, n) || n == 0) {
            continue;
        }
        ++usedSlots;
        totalItems += n;
    }
    return true;
}

const void* userDataOf(const void* stack)
{
    if (g_stackUserDataOffset < 0 || isEmpty(stack)) {
        return nullptr;
    }
    void* root = nullptr;
    if (!readPointer(static_cast<const std::byte*>(stack) + g_stackUserDataOffset, root)) {
        return nullptr;
    }
    return root;
}

std::string enchantKey(const void* stack)
{
    if (g_compoundGet == nullptr) {
        return {};
    }
    const void* const root = userDataOf(stack);
    void* rootVtable = nullptr;
    if (root == nullptr || !readPointer(root, rootVtable) || rootVtable == nullptr
        || !memory::inGameModule(rootVtable)) {
        return {};
    }
    const void* const list = compoundGet(root, "ench");
    if (list == nullptr || tagType(list) != 9) {
        return {};
    }
    void* first = nullptr;
    void* last = nullptr;
    if (!readPointer(static_cast<const std::byte*>(list) + 0x08, first)
        || !readPointer(static_cast<const std::byte*>(list) + 0x10, last)
        || first == nullptr || last == nullptr) {
        return {};
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(first);
    const auto end = reinterpret_cast<std::uintptr_t>(last);
    if (end < begin || (end - begin) % sizeof(void*) != 0) {
        return {};
    }
    const std::size_t count = (std::min)(static_cast<std::size_t>((end - begin) / sizeof(void*)),
                                         std::size_t{64});
    std::vector<std::pair<int, int>> enchants;
    enchants.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        void* elem = nullptr;
        void* vt = nullptr;
        if (!readPointer(static_cast<const std::byte*>(first) + i * sizeof(void*), elem)
            || elem == nullptr || !readPointer(elem, vt) || vt != rootVtable) {
            continue;
        }
        const void* const idTag = compoundGet(elem, "id");
        const void* const levelTag = compoundGet(elem, "lvl");
        std::uint16_t id = 0;
        std::uint16_t level = 0;
        if (tagType(idTag) != 2 || tagType(levelTag) != 2
            || !readWordGuarded(static_cast<const std::byte*>(idTag) + 0x08, id)
            || !readWordGuarded(static_cast<const std::byte*>(levelTag) + 0x08, level)) {
            continue;
        }
        enchants.emplace_back(static_cast<std::int16_t>(id), static_cast<std::int16_t>(level));
    }
    return formatEnchantKey(std::move(enchants));
}

int nbtTagCount(const void* stack)
{
    const void* const root = userDataOf(stack);
    void* rootVtable = nullptr;
    if (root == nullptr) {
        return 0;
    }
    std::uint64_t size = 0;
    if (!readPointer(root, rootVtable) || rootVtable == nullptr || !memory::inGameModule(rootVtable)
        || !memory::copyGuarded(static_cast<const std::byte*>(root) + 0x10, &size, sizeof(size)) || size > 4096) {
        return -1;
    }
    return static_cast<int>(size);
}

bool nbtInt(const void* stack, std::string_view key, std::int32_t& out)
{
    if (g_compoundGet == nullptr) {
        return false;
    }
    const void* const root = userDataOf(stack);
    void* rootVtable = nullptr;
    if (root == nullptr || !readPointer(root, rootVtable) || rootVtable == nullptr
        || !memory::inGameModule(rootVtable)) {
        return false;
    }
    const void* const tag = compoundGet(root, key);
    return tag != nullptr && tagType(tag) == 3
        && memory::copyGuarded(static_cast<const std::byte*>(tag) + 0x08, &out, sizeof(out));
}

bool itemsListBounds(const void* root, const void*& first, const void*& last)
{
    first = nullptr;
    last = nullptr;
    if (g_compoundGet == nullptr || root == nullptr) {
        return false;
    }
    const void* list = compoundGet(root, "Items");
    if (list == nullptr || tagType(list) != 9) {
        return false;
    }
    void* a = nullptr;
    void* b = nullptr;
    if (!readPointer(static_cast<const std::byte*>(list) + 0x08, a)
        || !readPointer(static_cast<const std::byte*>(list) + 0x10, b)) {
        return false;
    }
    first = a;
    last = b;
    return true;
}

bool nbtItemsOfTag(const void* root, std::vector<NbtItem>& out)
{
    out.clear();
    if (g_compoundGet == nullptr || root == nullptr) {
        return false;
    }
    void* rootVtable = nullptr;
    if (!readPointer(root, rootVtable) || rootVtable == nullptr || !memory::inGameModule(rootVtable)) {
        return false;
    }
    const void* list = compoundGet(root, "Items");
    if (list == nullptr) {
        return true;
    }
    if (tagType(list) != 9) {
        return false;
    }
    void* first = nullptr;
    void* last = nullptr;
    if (!readPointer(static_cast<const std::byte*>(list) + 0x08, first)
        || !readPointer(static_cast<const std::byte*>(list) + 0x10, last)) {
        return false;
    }
    const std::ptrdiff_t bytes = static_cast<const std::byte*>(last) - static_cast<const std::byte*>(first);
    if (bytes < 0 || bytes % 8 != 0 || bytes / 8 > 256) {
        return false;
    }
    auto readSmall = [](const void* tag, int type, int& value) {
        if (tag == nullptr || tagType(tag) != type) {
            return false;
        }
        if (type == 1) {
            std::uint8_t v = 0;
            if (!readByteGuarded(static_cast<const std::byte*>(tag) + 0x08, v)) {
                return false;
            }
            value = static_cast<std::int8_t>(v);
            return true;
        }
        std::uint16_t v = 0;
        if (!readWordGuarded(static_cast<const std::byte*>(tag) + 0x08, v)) {
            return false;
        }
        value = static_cast<std::int16_t>(v);
        return true;
    };
    for (std::ptrdiff_t i = 0; i < bytes / 8; ++i) {
        void* elem = nullptr;
        void* vt = nullptr;
        if (!readPointer(static_cast<const std::byte*>(first) + i * 8, elem) || elem == nullptr
            || !readPointer(elem, vt) || vt != rootVtable) {
            continue;
        }
        NbtItem one;
        int count = 0;
        if (!readSmall(compoundGet(elem, "Count"), 1, count) || count <= 0) {
            continue;
        }
        one.count = count & 0xFF;
        readSmall(compoundGet(elem, "Slot"), 1, one.slot);
        one.slot &= 0xFF;
        readSmall(compoundGet(elem, "Damage"), 2, one.aux);
        const void* const name = compoundGet(elem, "Name");
        if (name != nullptr && tagType(name) == 8) {
            char text[128]{};
            if (readStdStringGuarded(static_cast<const std::byte*>(name) + 0x08, text, sizeof(text))) {
                one.name = text;
            }
        }
        if (one.name.empty()) {
            continue;
        }
        if (const void* const userTag = compoundGet(elem, "tag"); userTag != nullptr && tagType(userTag) == 10) {
            void* tagVt = nullptr;
            if (readPointer(userTag, tagVt) && tagVt == rootVtable) {
                const void* const ench = compoundGet(userTag, "ench");
                one.enchanted = ench != nullptr && tagType(ench) == 9;
            }
        }
        one.elem = elem;
        out.push_back(std::move(one));
    }
    return true;
}

int screenCollectionSize(const std::string& coll)
{
    if (!hasScreen() || g_screenGetItem == nullptr) {
        return 0;
    }
    const std::string& name = intern(coll);
    bool faulted = false;
    const void* none = callScreenGetItemGuarded(reinterpret_cast<const void*>(g_screenGetItem), g_screen.ctrl,
                                                &name, 1 << 20, faulted);
    if (faulted || none == nullptr) {
        return 0;
    }
    constexpr int kMaxScreenCollection = 4096;
    int size = 0;
    for (; size < kMaxScreenCollection; ++size) {
        const void* one = callScreenGetItemGuarded(reinterpret_cast<const void*>(g_screenGetItem), g_screen.ctrl,
                                                   &name, size, faulted);
        if (faulted || one == nullptr || one == none) {
            break;
        }
    }
    return faulted ? 0 : size;
}

const void* screenStackAt(const std::string& coll, int index)
{
    if (!hasScreen() || g_screenGetItem == nullptr || index < 0) {
        return nullptr;
    }
    const std::string& name = intern(coll);
    bool faulted = false;
    const void* one = callScreenGetItemGuarded(reinterpret_cast<const void*>(g_screenGetItem), g_screen.ctrl,
                                               &name, index, faulted);
    return faulted ? nullptr : one;
}

namespace {

bool sendButton(std::uint32_t id, const std::string* coll, int index)
{
    if (!hasScreen() || g_smOriginal == nullptr) {
        return false;
    }
    g_synthDepth.fetch_add(1, std::memory_order_relaxed);
    bool ok = true;
    if (g_pending.id != 0) {
        const std::string& pc = intern(g_pending.coll);
        bool faulted = false;
        callSmGuarded(g_smOriginal, g_screen.sm, g_pending.id, kStateReleased, &pc, g_pending.index,
                      faulted);
        g_pending = PendingPress{};
        if (faulted) {
            noteFault(L"state machine (release)");
        }
    }
    for (int state : {kStatePressed, kStateHeld, kStateReleased}) {
        bool faulted = false;
        callSmGuarded(g_smOriginal, g_screen.sm, id, state, coll, index, faulted);
        if (faulted) {
            noteFault(L"state machine");
            ok = false;
            break;
        }
    }
    g_synthDepth.fetch_sub(1, std::memory_order_relaxed);
    return ok;
}

}

bool click(const std::string& coll, int index, Click kind)
{
    std::uint32_t id = 0;
    switch (kind) {
    case Click::Left:
        id = button::kTakeAllPlaceAll;
        break;
    case Click::Right:
        id = button::kTakeHalfPlaceOne;
        break;
    case Click::Shift:
        id = button::kAutoPlace;
        break;
    case Click::DropOne:
        id = button::kDropOne;
        break;
    case Click::DropAll:
        id = button::kDropAll;
        break;
    case Click::Double:
        id = button::kCoalesce;
        break;
    }
    if (index < 0) {
        return false;
    }
    return sendButton(id, &intern(coll), index);
}

bool press(std::uint32_t id, const std::string& coll, int index)
{
    return sendButton(id, &intern(coll), index);
}

bool dropCursor(bool all)
{
    static const std::string kEmpty;
    return sendButton(all ? button::kCursorDropAll : button::kCursorDropOne, &kEmpty, -1);
}

bool hovered(std::string& coll, int& index)
{
    if (!lastHovered(coll, index)) {
        return false;
    }
    POINT now{};
    GetCursorPos(&now);
    const int pitch = (g_pitch > 0) ? g_pitch : 40;
    return std::abs(now.x - g_hoverCursor.x) < pitch && std::abs(now.y - g_hoverCursor.y) < pitch;
}

bool lastHovered(std::string& coll, int& index)
{
    if (!hasScreen() || g_hoverIndex < 0 || g_hoverColl.empty()) {
        return false;
    }
    coll = g_hoverColl;
    index = g_hoverIndex;
    return true;
}

int slotPitchPixels()
{
    return g_pitch;
}

bool slotScreenPos(const std::string& coll, int index, int& x, int& y)
{
    auto it = g_screen.positions.find(SlotKey{coll, index});
    if (it != g_screen.positions.end()) {
        x = it->second.x;
        y = it->second.y;
        return true;
    }
    if (g_pitch <= 0) {
        return false;
    }
    for (const auto& [key, pos] : g_screen.positions) {
        if (key.coll != coll) {
            continue;
        }
        const int cols = 9;
        x = pos.x + (index % cols - key.index % cols) * g_pitch;
        y = pos.y + (index / cols - key.index / cols) * g_pitch;
        return true;
    }
    return false;
}

}
