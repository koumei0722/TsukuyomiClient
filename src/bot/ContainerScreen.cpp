#include "ContainerScreen.h"
#include "BotNet.h"
#include "BotSignatures.h"
#include "core/Logger.h"
#include "game/GameData.h"
#include "hooks/Detours.h"
#include "memory/Memory.h"
#include <Windows.h>
#include <atomic>
#include <cstring>
#include <intrin.h>
#include <utility>

namespace tsukuyomi::bot::containerscreen {
namespace {
constexpr std::uint8_t kChestType = 2;
constexpr int kSlots = 54;
constexpr std::uint8_t kNoWindow = 0xff;
const char kScreenName[] = "chest.large_chest_screen";
const char kSmallScreenName[] = "chest.small_chest_screen";

using CreateFn = void(__fastcall*)(void* factory, void* out, const void* name, void* player, const void* position, std::int64_t actor);
using PushFn = void(__fastcall*)(void* stack, void* scene, bool);
using ContainerCtorFn = void(__fastcall*)(void* self, std::uint8_t type, const void* name, bool customName);
using ResizeFn = void(__fastcall*)(void* vector, std::size_t count);
using GetterFn = void*(__fastcall*)(void* self);
using HolderFn = void*(__fastcall*)(void* self, void* out);
using DeleteFn = void*(__fastcall*)(void* self, unsigned flags);
using CountsFn = void(__fastcall*)(void* counts);

struct Layout {
    void* containerAt = nullptr;
    void* inRange = nullptr;
    std::uint32_t clientField = 0, stackSlot = 0, factorySlot = 0, openModelField = 0;
    CreateFn create = nullptr;
    std::uint32_t pushSlot = 0;
    ContainerCtorFn containerCtor = nullptr;
    void* simpleVtable = nullptr;
    std::uint32_t sizeField = 0, itemsField = 0;
    ResizeFn resize = nullptr;
    std::uint32_t positionField = 0, actorField = 0, typeField = 0, idField = 0;
    void* modelVtable = nullptr;
    void* weakRef = nullptr;
    void* nameAt = nullptr;
    void* customNameAt = nullptr;
    std::uint32_t refPosition = 0, refRuntime = 0, refWeak = 0, runtimeField = 0;
};
Layout g;
bool g_ready = false;
std::atomic<bool> g_hooked{false};

std::atomic<bool> g_active{false};
std::int32_t g_position[3] = {};
void* g_container = nullptr;
std::atomic<std::uint8_t> g_window{kNoWindow};
std::atomic<bool> g_assigned{false};
std::string g_title;
void* g_scene = nullptr;
void* g_counts = nullptr;

struct Counts {
    void* const* vtable;
    volatile long uses;
    volatile long weaks;
    void* container;
};
void __fastcall countsDestroy(Counts* self);
void __fastcall countsDeleteThis(Counts*) {}
void* __fastcall countsGetDeleter(const Counts*, const void*) { return nullptr; }
void* const kCountsVtable[] = {reinterpret_cast<void*>(&countsDestroy), reinterpret_cast<void*>(&countsDeleteThis),
                               reinterpret_cast<void*>(&countsGetDeleter)};
Counts* g_owner = nullptr;
bool g_gone = false;
int g_logs = 0;
bool canLog() { return g_logs < 40 && ++g_logs; }

int filter(unsigned code) { return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH; }
std::int32_t disp32(const std::byte* at) { std::int32_t value = 0; std::memcpy(&value, at, 4); return value; }
const std::byte* relTarget(const std::byte* site, std::ptrdiff_t field, std::ptrdiff_t end) { return site + end + disp32(site + field); }
std::uint32_t field(const std::byte* site, std::ptrdiff_t at) { return static_cast<std::uint32_t>(disp32(site + at)); }
bool inMainModule(const void* address)
{
    static const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return value >= base && value < base + 0x20000000;
}

void releaseGuarded(void* counts)
{
    if (!counts) return;
    __try {
        auto* const bytes = static_cast<std::byte*>(counts);
        if (_InterlockedDecrement(reinterpret_cast<volatile long*>(bytes + 8)) != 0) return;
        auto** const vtable = *static_cast<void***>(counts);
        reinterpret_cast<CountsFn>(vtable[0])(counts);
        if (_InterlockedDecrement(reinterpret_cast<volatile long*>(bytes + 0xc)) == 0) reinterpret_cast<CountsFn>(vtable[1])(counts);
    } __except (filter(GetExceptionCode())) {}
}
long usesGuarded(const void* counts)
{
    __try { return *reinterpret_cast<const volatile long*>(static_cast<const std::byte*>(counts) + 8); }
    __except (filter(GetExceptionCode())) { return -1; }
}
bool destroyContainerGuarded(void* container)
{
    __try {
        auto** const vtable = *static_cast<void***>(container);
        reinterpret_cast<DeleteFn>(vtable[0])(container, 1);
        return true;
    } __except (filter(GetExceptionCode())) { return false; }
}
void __fastcall countsDestroy(Counts* self)
{
    if (void* const container = std::exchange(self->container, nullptr); container && !destroyContainerGuarded(container))
        log().warn(L"Bot: destroying the inventory view container faulted");
}
void releaseOwner(Counts* owner)
{
    if (owner && _InterlockedDecrement(&owner->uses) == 0) countsDestroy(owner);
}
void* makeContainerGuarded(const std::string* title)
{
    const std::size_t bytes = g.itemsField + 0x18;
    void* const memory = hooks::callGameAllocate(bytes);
    if (!memory) return nullptr;
    __try {
        std::memset(memory, 0, bytes);
        g.containerCtor(memory, 0, title, true);
        auto* const at = static_cast<std::byte*>(memory);
        *reinterpret_cast<void**>(at) = g.simpleVtable;
        *reinterpret_cast<std::int32_t*>(at + g.sizeField) = kSlots;
        std::memset(at + g.itemsField, 0, 0x18);
        g.resize(at + g.itemsField, kSlots);
        return memory;
    } __except (filter(GetExceptionCode())) { return nullptr; }
}

enum class OpenResult { Opened, Busy, NoClient, NoScene, Faulted };
OpenResult openGuarded(void* player, const std::string* screen)
{
    void* holder[3] = {};
    __try {
        auto* const p = static_cast<std::byte*>(player);
        if (*reinterpret_cast<void**>(p + g.openModelField) != nullptr) return OpenResult::Busy;
        void* const client = *reinterpret_cast<void**>(p + g.clientField);
        if (!memory::plausiblePointer(client)) return OpenResult::NoClient;
        auto** const vtable = *static_cast<void***>(client);
        void* const factory = reinterpret_cast<GetterFn>(vtable[g.factorySlot / sizeof(void*)])(client);
        reinterpret_cast<HolderFn>(vtable[g.stackSlot / sizeof(void*)])(client, holder);
        void* const stack = holder[0] && *static_cast<const bool*>(holder[0]) ? holder[2] : nullptr;
        if (!factory || !stack) { releaseGuarded(holder[1]); return OpenResult::NoClient; }
        g_active.store(true, std::memory_order_release);
        void* scene[2] = {};
        g.create(factory, scene, screen, player, g_position, -1);
        if (!scene[0]) { releaseGuarded(scene[1]); releaseGuarded(holder[1]); return OpenResult::NoScene; }
        if (scene[1]) _InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<std::byte*>(scene[1]) + 8));
        void* argument[2] = {scene[0], scene[1]};
        reinterpret_cast<PushFn>((*static_cast<void***>(stack))[g.pushSlot / sizeof(void*)])(stack, argument, false);
        g_scene = scene[0]; g_counts = scene[1];
        releaseGuarded(holder[1]);
        return OpenResult::Opened;
    } __except (filter(GetExceptionCode())) { return OpenResult::Faulted; }
}

bool samePosition(const void* position)
{
    __try { return std::memcmp(position, g_position, sizeof g_position) == 0; }
    __except (filter(GetExceptionCode())) { return false; }
}
bool claimModelGuarded(void* model)
{
    __try {
        auto* const m = static_cast<std::byte*>(model);
        if (*reinterpret_cast<void**>(m) != g.modelVtable || m[g.typeField] != static_cast<std::byte>(kChestType)
            || *reinterpret_cast<const std::int64_t*>(m + g.actorField) != -1
            || std::memcmp(m + g.positionField, g_position, sizeof g_position) != 0)
            return false;
        const auto window = g_window.load(std::memory_order_relaxed);
        if (static_cast<std::uint8_t>(m[g.idField]) != window) m[g.idField] = static_cast<std::byte>(window);
        return true;
    } __except (filter(GetExceptionCode())) { return false; }
}
bool writeGameStringGuarded(void* out, const char* text, std::size_t size)
{
    auto* const at = static_cast<std::byte*>(out);
    void* heap = nullptr;
    if (size >= 16) {
        if (size >= 0x1000 - 1) return false;
        heap = hooks::callGameAllocate(size + 1);
        if (!heap) return false;
    }
    __try {
        std::memset(at, 0, 0x20);
        char* const data = heap ? static_cast<char*>(heap) : reinterpret_cast<char*>(at);
        std::memcpy(data, text, size);
        data[size] = '\0';
        if (heap) *reinterpret_cast<void**>(at) = heap;
        *reinterpret_cast<std::size_t*>(at + 0x10) = size;
        *reinterpret_cast<std::size_t*>(at + 0x18) = heap ? size : 15;
        return true;
    } __except (filter(GetExceptionCode())) { return false; }
}
void fillWeakRefGuarded(std::byte* out, void* container, Counts* owner)
{
    __try {
        if (*reinterpret_cast<const std::int64_t*>(out) != -1 || std::memcmp(out + g.refPosition, g_position, sizeof g_position) != 0
            || *reinterpret_cast<const std::uint32_t*>(out + g.refRuntime)
                != *reinterpret_cast<const std::uint32_t*>(static_cast<const std::byte*>(container) + g.runtimeField)
            || *reinterpret_cast<void**>(out + g.refWeak) || *reinterpret_cast<void**>(out + g.refWeak + 8))
            return;
        _InterlockedIncrement(&owner->weaks);
        *reinterpret_cast<void**>(out + g.refWeak) = container;
        *reinterpret_cast<void**>(out + g.refWeak + 8) = owner;
    } __except (filter(GetExceptionCode())) {}
}
}

bool initialize()
{
    auto* const at = net::site(Site::ContainerAtPosition);
    auto* const range = net::site(Site::LevelContainerInRange);
    auto* const openSite = net::site(Site::ContainerScreenOpenSite);
    auto* const createSite = net::site(Site::ContainerScreenCreateSite);
    auto* const simpleSite = net::site(Site::SimpleContainerCtorSite);
    auto* const modelSite = net::site(Site::LevelContainerModelCtorSite);
    auto* const weakRef = net::site(Site::LevelContainerWeakRef);
    auto* const weakSite = net::site(Site::LevelContainerWeakRefSite);
    auto* const nameAt = net::site(Site::ContainerNameAt);
    auto* const customNameAt = net::site(Site::ContainerCustomNameAt);
    if (!at || !range || !openSite || !createSite || !simpleSite || !modelSite || !weakRef || !weakSite || !nameAt || !customNameAt) {
        log().warn(L"Bot: the chest screen signatures are missing; inventory views are unavailable");
        return false;
    }
    Layout l;
    l.containerAt = at; l.inRange = range;
    l.clientField = field(openSite, 3); l.stackSlot = field(openSite, 0xd); l.factorySlot = field(openSite, 0x2b); l.openModelField = field(openSite, 0x39);
    const auto* small = reinterpret_cast<const char*>(relTarget(createSite, 0x3f, 0x43));
    l.create = reinterpret_cast<CreateFn>(const_cast<std::byte*>(relTarget(createSite, 0x66, 0x6a)));
    l.pushSlot = field(createSite, 0x71);
    l.containerCtor = reinterpret_cast<ContainerCtorFn>(const_cast<std::byte*>(relTarget(simpleSite, 0xd, 0x11)));
    l.simpleVtable = const_cast<std::byte*>(relTarget(simpleSite, 0x15, 0x19));
    l.sizeField = field(simpleSite, 0x22); l.itemsField = field(simpleSite, 0x2d);
    l.resize = reinterpret_cast<ResizeFn>(const_cast<std::byte*>(relTarget(simpleSite, 0x50, 0x54)));
    const auto modelBase = static_cast<std::uint8_t>(modelSite[0x30]);
    l.positionField = field(modelSite, 6) - modelBase; l.actorField = field(modelSite, 0x17) - modelBase;
    l.typeField = field(modelSite, 0x22) - modelBase;
    l.modelVtable = const_cast<std::byte*>(relTarget(modelSite, 0x29, 0x2d));
    l.weakRef = weakRef; l.nameAt = nameAt; l.customNameAt = customNameAt;
    l.refPosition = static_cast<std::uint8_t>(weakSite[0x18]); l.runtimeField = field(weakSite, 0x24);
    l.refRuntime = static_cast<std::uint8_t>(weakSite[0x2a]); l.refWeak = static_cast<std::uint8_t>(weakSite[0x31]);
    DWORD64 imageBase = 0;
    const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(weakSite), &imageBase, nullptr);
    const bool sameFunction = entry && reinterpret_cast<std::byte*>(imageBase + entry->BeginAddress) == weakRef;
    std::uint8_t setter[4] = {}, getter[5] = {};
    void* slots[3] = {};
    char name[sizeof kSmallScreenName] = {};
    const bool read = memory::copyGuarded(l.modelVtable, slots, sizeof slots) && inMainModule(slots[1]) && inMainModule(slots[2])
        && memory::copyGuarded(slots[2], setter, sizeof setter) && memory::copyGuarded(slots[1], getter, sizeof getter)
        && memory::copyGuarded(small, name, sizeof name);
    const bool shaped = read && setter[0] == 0x88 && setter[1] == 0x51 && setter[3] == 0xc3 && getter[0] == 0x0f && getter[1] == 0xb6
        && getter[2] == 0x41 && getter[3] == setter[2] && getter[4] == 0xc3 && std::memcmp(name, kSmallScreenName, sizeof name) == 0;
    l.idField = setter[2];
    const auto plausibleOffset = [](std::uint32_t v) { return v > 0 && v < 0x2000; };
    if (!shaped || !inMainModule(l.create) || !inMainModule(l.containerCtor) || !inMainModule(l.simpleVtable) || !inMainModule(l.resize)
        || !inMainModule(l.modelVtable) || !plausibleOffset(l.clientField) || !plausibleOffset(l.stackSlot) || !plausibleOffset(l.factorySlot)
        || !plausibleOffset(l.openModelField) || !plausibleOffset(l.pushSlot) || !plausibleOffset(l.sizeField) || l.itemsField != l.sizeField + 8
        || !plausibleOffset(l.positionField) || !plausibleOffset(l.actorField) || !plausibleOffset(l.typeField) || !sameFunction
        || l.refPosition + sizeof g_position > l.refRuntime || l.refRuntime >= l.refWeak || !plausibleOffset(l.runtimeField)) {
        log().warn(L"Bot: cannot read the chest screen fields; inventory views are unavailable");
        return false;
    }
    g = l;
    g_ready = true;
    log().info(L"Bot: chest screen without blocks: client +{:#x}, stack slot +{:#x}, factory slot +{:#x}, push slot +{:#x}, "
               L"container {:#x} items +{:#x} runtime id +{:#x}, model position +{:#x} type +{:#x} window +{:#x}, weak ref +{:#x}/+{:#x}/+{:#x}",
               l.clientField, l.stackSlot, l.factorySlot, l.pushSlot, l.sizeField, l.itemsField, l.runtimeField, l.positionField, l.typeField,
               l.idField, l.refPosition, l.refRuntime, l.refWeak);
    return true;
}
void* containerAtTarget() { return g.containerAt; }
void* inRangeTarget() { return g.inRange; }
void* weakRefTarget() { return g.weakRef; }
void* nameAtTarget() { return g.nameAt; }
void* customNameAtTarget() { return g.customNameAt; }
void setHooked(bool hooked) { g_hooked.store(hooked && g_ready, std::memory_order_release); }
bool available() { return g_ready && g_hooked.load(std::memory_order_acquire); }

void* onContainerAt(ContainerAtFn original, void* player, std::uint8_t type, const void* position)
{
    if (type == kChestType && g_active.load(std::memory_order_acquire) && position && samePosition(position)) return g_container;
    return original(player, type, position);
}
void* onWeakRef(WeakRefFn original, void* model, void* out)
{
    void* const result = original(model, out);
    if (g_active.load(std::memory_order_acquire) && out && g_container && g_owner) fillWeakRefGuarded(static_cast<std::byte*>(out), g_container, g_owner);
    return result;
}
void* onNameAt(NameAtFn original, void* model, void* out, const void* position)
{
    if (g_active.load(std::memory_order_acquire) && out && position && samePosition(position)
        && writeGameStringGuarded(out, g_title.data(), g_title.size()))
        return out;
    return original(model, out, position);
}
bool onCustomNameAt(CustomNameAtFn original, void* controller, const void* position)
{
    if (g_active.load(std::memory_order_acquire) && position && samePosition(position)) return true;
    return original(controller, position);
}
bool onInRange(InRangeFn original, void* model, float pickRange)
{
    if (g_active.load(std::memory_order_acquire) && model && claimModelGuarded(model)) {
        if (!g_assigned.exchange(true, std::memory_order_acq_rel) && canLog())
            log().info(L"Bot: the chest screen took window {}", g_window.load(std::memory_order_relaxed));
        return true;
    }
    return original(model, pickRange);
}

std::optional<std::string> open(const std::string& title, std::uint8_t window, std::int32_t x, std::int32_t y, std::int32_t z)
{
    if (!available()) return "the chest screen is unavailable (see Tsukuyomi.log)";
    if (g_scene || g_container || g_owner) return "the previous inventory view is still closing";
    void* const player = GameData::instance().player();
    if (!player) return "the main player is not in a world";
    const std::string screen(kScreenName);
    g_container = makeContainerGuarded(&title);
    if (!g_container) return "cannot create the container";
    g_owner = new Counts{kCountsVtable, 1, 2, g_container};
    g_position[0] = x; g_position[1] = y; g_position[2] = z;
    g_title = title;
    g_window.store(window, std::memory_order_relaxed); g_assigned.store(false, std::memory_order_relaxed); g_gone = false;
    const auto result = openGuarded(player, &screen);
    if (result == OpenResult::Opened) {
        if (canLog()) log().info(L"Bot: pushed the chest screen at ({}, {}, {}) for window {}", x, y, z, window);
        return std::nullopt;
    }
    g_active.store(false, std::memory_order_release);
    if (result == OpenResult::Faulted) {
        g_ready = false;
        g_container = nullptr; g_owner = nullptr; g_scene = nullptr; g_counts = nullptr;
        log().error(L"Bot: opening the chest screen faulted; inventory views are off until the DLL is reloaded");
        return "opening the chest screen faulted (see Tsukuyomi.log)";
    }
    releaseOwner(std::exchange(g_owner, nullptr));
    g_container = nullptr;
    return result == OpenResult::Busy ? "close the current container screen first" : "the game did not create the chest screen";
}
bool ready() { return g_assigned.load(std::memory_order_acquire); }
bool gone() { return g_gone; }
void finish() { g_assigned.store(false, std::memory_order_relaxed); }
void poll()
{
    if (!g_scene && !g_container && !g_owner) return;
    if (g_counts && usesGuarded(g_counts) > 1) return;
    g_gone = true;
    void* const counts = std::exchange(g_counts, nullptr);
    g_scene = nullptr;
    releaseGuarded(counts);
    g_active.store(false, std::memory_order_release);
    g_assigned.store(false, std::memory_order_relaxed);
    g_container = nullptr;
    releaseOwner(std::exchange(g_owner, nullptr));
    if (canLog()) log().info(L"Bot: the chest screen left the scene stack; released it");
}

}
