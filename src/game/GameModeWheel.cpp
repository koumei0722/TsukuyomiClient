#include "game/GameModeWheel.h"

#include "core/Logger.h"
#include "game/ContainerUi.h"
#include "game/DebugScreenLayout.h"
#include "game/GameModeIds.h"
#include "hooks/Detours.h"
#include "hooks/HookManager.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "modules/DebugKeys.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace tsukuyomi::gamemodewheel {
namespace {

using CreateSceneFn = void(__fastcall*)(void* factory, void* outScene);
using GetStackFn = void*(__fastcall*)(void* client, void* outRef);
using GetFactoryFn = void*(__fastcall*)(void* client);
using PushSceneFn = void(__fastcall*)(void* stack, void* scene, int flags);
using CountsReleaseFn = void(__fastcall*)(void* counts);
using CtorFn = void*(__fastcall*)(void*, void*, void*, void*);
using DtorFn = void*(__fastcall*)(void*, unsigned);
using SelectFn = void(__fastcall*)(void*, int);
using ExitFn = void*(__fastcall*)(void*);
using PropertyFn = void(__fastcall*)(void*, const std::string*, void*);
using NameFn = std::string*(__fastcall*)(std::string*, void*);
using JsonBoolFn = bool(__fastcall*)(const void*);
using JsonIndexFn = const void*(__fastcall*)(const void*, const char*);
using JsonIntFn = int(__fastcall*)(const void*, int);
using SetBoolFn = void(__fastcall*)(void*, const std::string_view*, const bool*);
using SetTextFn = void(__fastcall*)(void*, const std::string_view*, std::string*);

CreateSceneFn g_createWheel = nullptr;
std::int32_t g_stackSlot = 0;
std::int32_t g_factorySlot = 0;
std::int32_t g_pushSlot = 0;
CtorFn g_ctor = nullptr;
DtorFn g_dtor = nullptr;
SelectFn g_selected = nullptr;
PropertyFn g_image = nullptr;
PropertyFn g_imageValid = nullptr;
PropertyFn g_fileSystem = nullptr;
PropertyFn g_emoteValid = nullptr;
NameFn g_name = nullptr;
JsonBoolFn g_isNull = nullptr;
JsonBoolFn g_isObject = nullptr;
JsonBoolFn g_isInt = nullptr;
JsonBoolFn g_isUInt = nullptr;
JsonIndexFn g_index = nullptr;
JsonIntFn g_asInt = nullptr;
SetBoolFn g_setBool = nullptr;
SetTextFn g_setText = nullptr;
const char* g_indexKey = nullptr;
std::int32_t g_helperOffset = 0;
std::atomic<std::uint64_t> g_armedAt{0};
std::atomic<int> g_armedPurpose{0};
std::atomic<int> g_oursPurpose{0};
std::atomic<void*> g_ours{nullptr};
std::atomic<void*> g_oursHelper{nullptr};
bool g_ready = false;

constexpr const char* kImages[] = {
    "textures/blocks/grass_side_carried", "textures/items/iron_sword",
    "textures/items/map_empty", "textures/items/ender_eye"
};
constexpr const char* kNames[] = {"Creative", "Survival", "Adventure", "Spectator"};
constexpr int kModes[] = {gamemode::kCreative, gamemode::kSurvival,
                          gamemode::kAdventure, gamemode::kSpectator};
constexpr std::uint32_t kHashes[] = {
    0x3C3ED48Cu, 0xCEEC9919u, 0xED9E9C91u, 0x6AD60BBEu, 0xA51E43D4u
};

std::byte* callTarget(const std::byte* body, std::size_t offset)
{
    if (body[offset] != std::byte{0xE8}) return nullptr;
    auto* target = static_cast<std::byte*>(memory::ripTarget(body, offset + 1));
    return memory::inGameModule(target) && memory::isExecutable(target, 1) ? target : nullptr;
}

bool bindingBodies(std::byte* site, std::array<void*, 5>& result)
{
    if (site == nullptr || !memory::isReadable(site, 0x3F0)) return false;
    for (std::size_t h = 0; h < 5; ++h) {
        const std::byte* lastLea = nullptr;
        for (std::size_t offset = 0; offset + 10 <= 0x3F0; ++offset) {
            const auto* p = site + offset;
            if (p[0] == std::byte{0x48} && p[1] == std::byte{0x8D}
                && p[2] == std::byte{0x05}) lastLea = p;
            if (p[0] != std::byte{0xC7} || p[1] != std::byte{0x85}
                || p[3] != std::byte{0x03} || p[4] != std::byte{0x00}
                || p[5] != std::byte{0x00}
                || !((p[2] == std::byte{0xF4}) || (p[2] == std::byte{0xE0}))) continue;
            std::uint32_t hash = 0;
            std::memcpy(&hash, p + 6, sizeof(hash));
            if (hash != kHashes[h] || lastLea == nullptr) continue;
            void* table = memory::ripTarget(lastLea, 3);
            if (!memory::inGameModule(table)
                || !memory::isReadable(static_cast<std::byte*>(table) + 0x10, sizeof(void*))) return false;
            std::memcpy(&result[h], static_cast<std::byte*>(table) + 0x10, sizeof(void*));
            if (!memory::inGameModule(result[h]) || !memory::isExecutable(result[h], 1)) return false;
            break;
        }
        if (result[h] == nullptr) return false;
    }
    return true;
}

bool resolveJson(std::byte* body, std::byte* image)
{
    if (!memory::isReadable(body, 0x170) || !memory::isReadable(image, 0x16C)) return false;
    const ScanHit shape = scanRange(std::span(body, std::size_t{0x90}), kEmoteIsValidBindingShape);
    if (shape.count != 1 || shape.address != body) return false;
    if (body[0x45] != std::byte{0x48} || body[0x46] != std::byte{0x8D}
        || body[0x47] != std::byte{0x15}) return false;
    g_isNull = reinterpret_cast<JsonBoolFn>(callTarget(body, 0x2B));
    g_isObject = reinterpret_cast<JsonBoolFn>(callTarget(body, 0x3C));
    g_index = reinterpret_cast<JsonIndexFn>(callTarget(body, 0x4F));
    g_isInt = reinterpret_cast<JsonBoolFn>(callTarget(body, 0x5A));
    g_isUInt = reinterpret_cast<JsonBoolFn>(callTarget(body, 0x66));
    g_asInt = reinterpret_cast<JsonIntFn>(callTarget(body, 0x74));
    g_setBool = reinterpret_cast<SetBoolFn>(callTarget(body, 0x159));
    g_setText = reinterpret_cast<SetTextFn>(callTarget(image, 0x167));
    g_indexKey = static_cast<const char*>(memory::ripTarget(body, 0x48));
    return g_isNull && g_isObject && g_index && g_isInt && g_isUInt && g_asInt
           && g_setBool && g_setText && g_indexKey
           && memory::isReadable(g_indexKey, 7) && std::memcmp(g_indexKey, "#index", 7) == 0;
}

bool ownClosure(void* closure)
{
    void* helper = nullptr;
    return g_ready && closure != nullptr
           && memory::copyGuarded(static_cast<std::byte*>(closure) + 8, &helper, sizeof(helper))
           && helper != nullptr && helper == g_oursHelper.load(std::memory_order_acquire);
}

bool indexRaw(void* bag, int& out)
{
    __try {
        const void* value = static_cast<std::byte*>(bag) + 8;
        if (g_isNull(value) || !g_isObject(value)) return false;
        const void* item = g_index(value, g_indexKey);
        if (item == nullptr || (!g_isInt(item) && !g_isUInt(item))) return false;
        out = g_asInt(item, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct RawName {
    const char* data;
    std::size_t length;
};

bool readNameRaw(const std::string* key, RawName& out)
{
    __try {
        const auto* bytes = reinterpret_cast<const std::byte*>(key);
        const std::size_t length = *reinterpret_cast<const std::size_t*>(bytes + 0x10);
        const std::size_t capacity = *reinterpret_cast<const std::size_t*>(bytes + 0x18);
        if (length == 0 || length > 128 || capacity < length) return false;
        out.data = capacity >= 16 ? *reinterpret_cast<const char* const*>(key)
                                  : reinterpret_cast<const char*>(key);
        out.length = length;
        return out.data != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool setTextRaw(void* bag, const std::string_view* name, std::string* value)
{
    __try {
        g_setText(bag, name, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool setBoolRaw(void* bag, const std::string_view* name, const bool* value)
{
    __try {
        g_setBool(bag, name, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void replaceText(void* bag, const std::string* key, const char* text)
{
    if (bag == nullptr || key == nullptr) return;
    RawName raw{};
    if (!readNameRaw(key, raw)) return;
    const std::string_view name(raw.data, raw.length);
    std::string value(text);
    if (!setTextRaw(bag, &name, &value)) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) log().warn(L"GameModeWheel: text binding could not be written");
    }
}

void replaceBool(void* bag, const std::string* key)
{
    if (bag == nullptr || key == nullptr) return;
    RawName raw{};
    if (!readNameRaw(key, raw)) return;
    const std::string_view name(raw.data, raw.length);
    const bool value = true;
    if (!setBoolRaw(bag, &name, &value)) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) log().warn(L"GameModeWheel: bool binding could not be written");
    }
}

void __fastcall image(void* closure, const std::string* key, void* bag)
{
    int idx = -1;
    if (ownClosure(closure) && indexRaw(bag, idx) && idx >= 0 && idx < 4)
        replaceText(bag, key, kImages[idx]);
    else if (g_image) g_image(closure, key, bag);
}

void __fastcall imageValid(void* closure, const std::string* key, void* bag)
{
    int idx = -1;
    if (ownClosure(closure) && indexRaw(bag, idx) && idx >= 0 && idx < 4)
        replaceBool(bag, key);
    else if (g_imageValid) g_imageValid(closure, key, bag);
}

void __fastcall fileSystem(void* closure, const std::string* key, void* bag)
{
    int idx = -1;
    if (ownClosure(closure) && indexRaw(bag, idx) && idx >= 0 && idx < 4)
        replaceText(bag, key, "InUserPackage");
    else if (g_fileSystem) g_fileSystem(closure, key, bag);
}

void __fastcall emoteValid(void* closure, const std::string* key, void* bag)
{
    int idx = -1;
    if (ownClosure(closure) && indexRaw(bag, idx) && idx >= 0 && idx < 4)
        replaceBool(bag, key);
    else if (g_emoteValid) g_emoteValid(closure, key, bag);
}

bool pausing()
{
    return g_oursPurpose.load(std::memory_order_acquire) == static_cast<int>(Purpose::Pause);
}

std::string* __fastcall name(std::string* out, void* closure)
{
    if (out == nullptr) return out;
    if (!ownClosure(closure)) return g_name ? g_name(out, closure) : out;
    void* helper = nullptr;
    int idx = -1;
    if (!memory::copyGuarded(static_cast<std::byte*>(closure) + 8, &helper, sizeof(helper))
        || !memory::copyGuarded(static_cast<std::byte*>(helper) + 0x64, &idx, sizeof(idx))) {
        return g_name ? g_name(out, closure) : out;
    }
    const char* text = idx >= 0 && idx < 4 && !pausing() ? kNames[idx] : "";
    const std::size_t length = std::strlen(text);
    __try {
        std::memset(out, 0, 0x20);
        std::memcpy(out, text, length);
        *reinterpret_cast<std::size_t*>(reinterpret_cast<std::byte*>(out) + 0x10) = length;
        *reinterpret_cast<std::size_t*>(reinterpret_cast<std::byte*>(out) + 0x18) = 15;
        return out;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return out;
    }
}

void* __fastcall ctor(void* self, void* a2, void* a3, void* a4)
{
    void* result = g_ctor ? g_ctor(self, a2, a3, a4) : self;
    const std::uint64_t armed = g_armedAt.load(std::memory_order_acquire);
    const std::uint64_t now = GetTickCount64();
    if (g_ready && armed != 0 && now >= armed && now - armed <= 1000 && result != nullptr) {
        const int purpose = g_armedPurpose.load(std::memory_order_acquire);
        g_oursPurpose.store(purpose, std::memory_order_release);
        g_oursHelper.store(static_cast<std::byte*>(result) + g_helperOffset, std::memory_order_release);
        g_ours.store(result, std::memory_order_release);
        g_armedAt.store(0, std::memory_order_release);
        if (purpose == static_cast<int>(Purpose::Pause)
            && !containerui::bindPersistentBool(result, dbgscreen::kBindPauseWheel, dbgscreen::kPauseWheelSlot)) {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) log().warn(L"GameModeWheel: the pause wheel binding could not be registered");
        }
    }
    return result;
}

void* __fastcall dtor(void* self, unsigned flags)
{
    if (self == g_ours.load(std::memory_order_acquire)) {
        const bool paused = pausing();
        g_ours.store(nullptr, std::memory_order_release);
        g_oursHelper.store(nullptr, std::memory_order_release);
        g_oursPurpose.store(0, std::memory_order_release);
        if (paused) DebugKeys::instance().onPauseWheelClosed();
    }
    return g_dtor ? g_dtor(self, flags) : self;
}

constexpr std::size_t kScreenExitSlot = 0x60;

bool exitRaw(void* controller)
{
    __try {
        void** vt = *static_cast<void***>(controller);
        reinterpret_cast<ExitFn>(vt[kScreenExitSlot / sizeof(void*)])(controller);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall selected(void* self, int idx)
{
    if (!g_ready || self != g_ours.load(std::memory_order_acquire)) {
        if (g_selected) g_selected(self, idx);
        return;
    }
    if (pausing()) return;
    if (idx >= 0 && idx < 4) DebugKeys::instance().selectGameMode(kModes[idx]);
    if (!exitRaw(self)) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) log().warn(L"GameModeWheel: could not close the wheel");
    }
}

bool pushRaw(void* ci)
{
    __try {
        void* const* vt = *static_cast<void* const* const*>(ci);
        struct alignas(8) StackRef {
            void* alive;
            void* counts;
            void* stack;
        } ref{};
        reinterpret_cast<GetStackFn>(vt[g_stackSlot / 8])(ci, &ref);
        bool ok = false;
        if (ref.alive != nullptr && *static_cast<const volatile std::uint8_t*>(ref.alive) == 1 && ref.stack != nullptr) {
            void* const factory = reinterpret_cast<GetFactoryFn>(vt[g_factorySlot / 8])(ci);
            alignas(16) unsigned char scene[16]{};
            g_createWheel(factory, scene);
            void* const* svt = *static_cast<void* const* const*>(ref.stack);
            reinterpret_cast<PushSceneFn>(svt[g_pushSlot / 8])(ref.stack, scene, 0);
            ok = true;
        }
        if (ref.counts != nullptr) {
            auto* const c = static_cast<std::uint8_t*>(ref.counts);
            void* const* cvt = *reinterpret_cast<void* const* const*>(c);
            if (_InterlockedDecrement(reinterpret_cast<volatile long*>(c + 8)) == 0) {
                reinterpret_cast<CountsReleaseFn>(cvt[0])(c);
                if (_InterlockedDecrement(reinterpret_cast<volatile long*>(c + 0xc)) == 0) {
                    reinterpret_cast<CountsReleaseFn>(cvt[1])(c);
                }
            }
        }
        return ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::int32_t slotAt(const std::byte* at)
{
    if (at[0] != std::byte{0x48} || at[1] != std::byte{0x8B} || at[2] != std::byte{0x80}) return 0;
    std::int32_t slot = 0;
    std::memcpy(&slot, at + 3, sizeof(slot));
    return slot > 0 && slot < 0x2000 && slot % 8 == 0 ? slot : 0;
}

}

bool installHooks()
{
    const Scanner& scanner = Scanner::instance();
    std::byte* ctorBody = scanner.address(Target::EmoteWheelCtor);
    std::byte* bindings = scanner.address(Target::EmoteWheelBindings);
    std::byte* const released = scanner.address(Target::EmoteReleased);
    std::byte* selectedBody = scanner.address(Target::EmoteWheelSelected);
    std::array<void*, 5> body{};
    if (released != nullptr && memory::isReadable(released, 0x270)) {
        g_stackSlot = slotAt(released + 0x1A8);
        g_factorySlot = slotAt(released + 0x249);
        g_pushSlot = slotAt(released + 0x268);
        g_createWheel = reinterpret_cast<CreateSceneFn>(callTarget(released, 0x260));
    }
    if (!ctorBody || !bindings || !g_createWheel || g_stackSlot == 0 || g_factorySlot == 0 || g_pushSlot == 0
        || !selectedBody
        || !memory::isReadable(ctorBody, 0x16F)
        || ctorBody[0x6D] != std::byte{0x48} || ctorBody[0x6E] != std::byte{0x8D}
        || ctorBody[0x6F] != std::byte{0x05}
        || ctorBody[0x168] != std::byte{0x48} || ctorBody[0x169] != std::byte{0x8D}
        || ctorBody[0x16A] != std::byte{0xB2}
        || !bindingBodies(bindings, body)
        || !resolveJson(static_cast<std::byte*>(body[3]), static_cast<std::byte*>(body[0]))) {
        log().warn(L"GameModeWheel: a signature or binding shape was unavailable");
        return false;
    }
    std::memcpy(&g_helperOffset, ctorBody + 0x16B, sizeof(g_helperOffset));
    if (g_helperOffset <= 0 || g_helperOffset > 0x10000) return false;
    void* table = memory::ripTarget(ctorBody, 0x70);
    void* destructor = nullptr;
    if (!memory::inGameModule(table) || !memory::isReadable(table, sizeof(destructor))) return false;
    std::memcpy(&destructor, table, sizeof(destructor));
    if (!memory::inGameModule(destructor) || !memory::isExecutable(destructor, 1)) return false;
    HookManager& hooks = HookManager::instance();
    bool okay = true;
    okay &= hooks.create(ctorBody, reinterpret_cast<void*>(&ctor),
                         reinterpret_cast<void**>(&g_ctor), L"EmoteWheelCtor");
    okay &= hooks.create(destructor, reinterpret_cast<void*>(&dtor),
                         reinterpret_cast<void**>(&g_dtor), L"EmoteWheelDtor");
    okay &= hooks.create(selectedBody, reinterpret_cast<void*>(&selected),
                         reinterpret_cast<void**>(&g_selected), L"EmoteWheelSelected");
    okay &= hooks.create(body[0], reinterpret_cast<void*>(&image),
                         reinterpret_cast<void**>(&g_image), L"EmoteWheelImage");
    okay &= hooks.create(body[1], reinterpret_cast<void*>(&imageValid),
                         reinterpret_cast<void**>(&g_imageValid), L"EmoteWheelImageValid");
    okay &= hooks.create(body[2], reinterpret_cast<void*>(&fileSystem),
                         reinterpret_cast<void**>(&g_fileSystem), L"EmoteWheelFileSystem");
    okay &= hooks.create(body[3], reinterpret_cast<void*>(&emoteValid),
                         reinterpret_cast<void**>(&g_emoteValid), L"EmoteWheelEmoteValid");
    okay &= hooks.create(body[4], reinterpret_cast<void*>(&name),
                         reinterpret_cast<void**>(&g_name), L"EmoteWheelName");
    g_ready = okay;
    return okay;
}

bool open(Purpose purpose)
{
    if (!g_ready) return false;
    void* ci = hooks::gameClientInstance();
    if (ci == nullptr || !memory::isReadable(ci, sizeof(void*))) return false;
    if (g_ours.load(std::memory_order_acquire) != nullptr) return false;
    g_armedPurpose.store(static_cast<int>(purpose), std::memory_order_release);
    g_armedAt.store(GetTickCount64(), std::memory_order_release);
    const bool pushed = pushRaw(ci);
    const bool built = g_ours.load(std::memory_order_acquire) != nullptr;
    if (!built) g_armedAt.store(0, std::memory_order_release);
    if (!pushed || !built) {
        static std::atomic<int> warned{0};
        if (warned.fetch_add(1) < 3)
            log().warn(L"GameModeWheel: the wheel was not opened (pushed {} / constructed {})", pushed, built);
    }
    return built;
}

void closeOurs()
{
    void* const ours = g_ours.load(std::memory_order_acquire);
    if (ours != nullptr && !exitRaw(ours)) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) log().warn(L"GameModeWheel: could not close the wheel");
    }
}

}
