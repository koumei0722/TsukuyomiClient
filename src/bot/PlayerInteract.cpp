#include "PlayerInteract.h"
#include "BotNet.h"
#include "BotSignatures.h"
#include "GameCodec.h"
#include "LocalPlayerIds.h"
#include "core/Logger.h"
#include "game/GameData.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "memory/Signatures.h"
#include <Windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>

namespace tsukuyomi::bot::playerinteract {
namespace {
constexpr std::ptrdiff_t kResultText = 0x00, kResultTable = 0x08, kResultTarget = 0x10, kResultPlayer = 0x18, kResultFlags = 0x48;
constexpr std::uint64_t kTextOwned = 1ull << 55;
constexpr std::size_t kTableSlots = 3, kInvokeSlot = 2;

constexpr std::ptrdiff_t kInvContainerOffset = 0xB8, kInvSlotsOffset = 0x198;
constexpr std::size_t kInventorySlots = 36, kArmorSlots = 4;
constexpr std::uint32_t kEquipmentTypeId = 0xB06141A9u;
constexpr std::ptrdiff_t kHandField = 0;
constexpr std::size_t kOffhandSlot = 1, kMaxEquipStride = 0x40;

std::atomic<bool> g_enabled{false};
bool g_hooked = false;
std::uint64_t g_text = 0;
void* g_table[kTableSlots] = {};
std::uint32_t g_categoryField = 0, g_inventoryField = 0, g_itemsOffset = 0, g_stackStride = 0, g_equipStride = 0, g_armorField = 0;
std::atomic<bool> g_layoutRead{false};

std::mutex g_mutex;
std::optional<Request> g_request;
std::atomic<int> g_logs{0};
bool canLog() { return g_logs.fetch_add(1, std::memory_order_relaxed) < 40; }

int filter(unsigned code) { return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH; }
std::int32_t disp32(const std::byte* at) { std::int32_t value = 0; std::memcpy(&value, at, 4); return value; }
void* functionStart(const std::byte* site)
{
    DWORD64 imageBase = 0;
    const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(site), &imageBase, nullptr);
    return entry ? reinterpret_cast<void*>(imageBase + entry->BeginAddress) : nullptr;
}
bool inMainModule(const void* address)
{
    static const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return value >= base && value < base + 0x20000000;
}
bool isPlayerGuarded(const void* actor)
{
    __try { return (static_cast<const std::uint8_t*>(actor)[g_categoryField] & 1) != 0; }
    __except (filter(GetExceptionCode())) { return false; }
}

void __fastcall invoke(void* callable)
{
    void* actor = nullptr;
    if (!memory::copyGuarded(static_cast<const std::byte*>(callable) + 8, &actor, sizeof actor) || !actor) return;
    const auto runtime = readActorRuntimeId(actor);
    if (!runtime) {
        if (canLog()) log().warn(L"Bot: right-clicked a player whose runtime id is unreadable; nothing is opened");
        return;
    }
    { std::lock_guard lock(g_mutex); g_request = Request{actor, *runtime}; }
    if (canLog()) log().info(L"Bot: right-clicked a player (runtime id {})", *runtime);
}

bool fillGuarded(std::byte* out, void* self, void* player)
{
    __try {
        if (*reinterpret_cast<void**>(out + kResultTable) != nullptr) return false;
        if (*reinterpret_cast<std::uint64_t*>(out + kResultText) & kTextOwned) return false;
        if ((static_cast<const std::uint8_t*>(self)[g_categoryField] & 1) == 0) return false;
        *reinterpret_cast<std::uint64_t*>(out + kResultText) = g_text;
        *reinterpret_cast<void**>(out + kResultTarget) = self;
        *reinterpret_cast<void**>(out + kResultPlayer) = player;
        *reinterpret_cast<void**>(out + kResultTable) = g_table;
        out[kResultFlags] = static_cast<std::byte>(static_cast<std::uint8_t>(out[kResultFlags]) | 1);
        return true;
    } __except (filter(GetExceptionCode())) { return false; }
}

const void* stackAt(const std::byte* vector, std::size_t index, std::size_t minimum)
{
    std::uintptr_t range[2] = {};
    if (!g_stackStride || !memory::copyGuarded(vector, range, sizeof range) || range[1] <= range[0]
        || (range[1] - range[0]) % g_stackStride != 0 || (range[1] - range[0]) / g_stackStride < minimum || index >= minimum)
        return nullptr;
    void* first = nullptr;
    void* vtable = nullptr;
    const auto* stack = reinterpret_cast<const std::byte*>(range[0]) + index * g_stackStride;
    if (!memory::copyGuarded(reinterpret_cast<const void*>(range[0]), &first, sizeof first) || !inMainModule(first)
        || !memory::copyGuarded(stack, &vtable, sizeof vtable) || vtable != first)
        return nullptr;
    return stack;
}
const std::byte* pointerAt(const void* base, std::ptrdiff_t offset)
{
    const std::byte* value = nullptr;
    if (!memory::copyGuarded(static_cast<const std::byte*>(base) + offset, &value, sizeof value) || !memory::plausiblePointer(value)) return nullptr;
    return value;
}
}

bool initialize()
{
    auto* const function = net::site(Site::ActorGetInteraction);
    auto* const site = net::site(Site::ActorInteractionSite);
    if (!function || !site) return false;
    if (functionStart(site) != function) {
        log().warn(L"Bot: the open-container interaction is not inside the actor's interaction check; right-clicking players is unavailable");
        return false;
    }
    const auto* text = site + 7 + disp32(site + 3);
    std::uint64_t mark = 0;
    std::memcpy(&mark, site + 0x16, sizeof mark);
    g_text = (reinterpret_cast<std::uint64_t>(text) & 0xff00ffffffffffffull) | mark;
    const auto* table = site + 0x33 + disp32(site + 0x2f);
    void* entries[kTableSlots] = {};
    if (!memory::copyGuarded(table, entries, sizeof entries) || !inMainModule(entries[0]) || !inMainModule(entries[1]) || !inMainModule(entries[2])
        || (mark & kTextOwned) || (mark & 0xff00ffffffffffffull) != 0) {
        log().warn(L"Bot: cannot read the open-container interaction; right-clicking players is unavailable");
        return false;
    }
    g_table[0] = entries[0]; g_table[1] = entries[1]; g_table[kInvokeSlot] = reinterpret_cast<void*>(&invoke);
    g_hooked = true;
    return true;
}
void* target() { return net::site(Site::ActorGetInteraction); }
void* onGetInteraction(GetInteractionFn original, void* self, void* out, void* player, void* position, std::uintptr_t fifth)
{
    void* const result = original(self, out, player, position, fifth);
    if (!g_enabled.load(std::memory_order_relaxed) || !g_categoryField || !out || !self || self == player
        || player != GameData::instance().player())
        return result;
    fillGuarded(static_cast<std::byte*>(out), self, player);
    return result;
}
void readLayout()
{
    Scanner& scanner = Scanner::instance();
    if (const auto* at = scanner.address(Target::TargetCategorySite); at && memory::isReadable(at, 12))
        if (const auto field = disp32(at + 8); field > 0 && field < 0x2000) g_categoryField = static_cast<std::uint32_t>(field);
    if (const auto* at = scanner.address(Target::AnnouncedSlotSite); at && memory::isReadable(at, 8))
        if (const auto field = disp32(at + 3); field > 0 && field < 0x4000 && field % 8 == 0) g_inventoryField = static_cast<std::uint32_t>(field);
    if (const auto* at = scanner.address(Target::SimpleContainerGetItem); at && memory::isReadable(at, 0x58)) {
        const auto items = disp32(at + 0x33), stride = disp32(at + 0x54);
        if (items > 0 && items < 0x1000 && items % 8 == 0 && stride >= 0x40 && stride < 0x400) {
            g_itemsOffset = static_cast<std::uint32_t>(items); g_stackStride = static_cast<std::uint32_t>(stride);
        }
    }
    if (const auto* at = scanner.address(Target::ArmorContainerGetter); at && memory::isReadable(at, 0xD0)) {
        const unsigned shift = static_cast<unsigned>(at[0xC9]);
        const std::size_t field = static_cast<std::size_t>(at[0xCE]);
        if (shift >= 3 && shift <= 6 && (std::size_t{1} << shift) <= kMaxEquipStride && field + 8 <= (std::size_t{1} << shift) && field % 8 == 0
            && field != kHandField) {
            g_equipStride = 1u << shift; g_armorField = static_cast<std::uint32_t>(field);
        }
    }
    g_layoutRead.store(true, std::memory_order_release);
    log().info(L"Bot: player inventory view: hook {}, category +{:#x}, inventory +{:#x}, items +{:#x} stride {:#x}, equipment stride {:#x} armor +{:#x}",
        g_hooked ? L"ready" : L"missing", g_categoryField, g_inventoryField, g_itemsOffset, g_stackStride, g_equipStride, g_armorField);
    if (!g_categoryField) log().warn(L"Bot: the actor category field is unreadable; right-clicking players is unavailable");
}
bool available() { return g_hooked && g_layoutRead.load(std::memory_order_acquire) && g_categoryField != 0; }
bool layoutReady() { return g_layoutRead.load(std::memory_order_acquire) && g_inventoryField != 0 && g_stackStride != 0; }
void setEnabled(bool enabled)
{
    g_enabled.store(enabled, std::memory_order_relaxed);
    if (!enabled) { std::lock_guard lock(g_mutex); g_request.reset(); }
}
std::optional<Request> takeRequest()
{
    std::lock_guard lock(g_mutex);
    return std::exchange(g_request, std::nullopt);
}
bool stillValid(const Request& request)
{
    return request.actor && isPlayerGuarded(request.actor) && readActorRuntimeId(request.actor) == request.runtimeId;
}
std::optional<BotSession::InventorySnapshot> snapshot(void* actor)
{
    BotSession::InventorySnapshot out{};
    out.selected = 0; out.serial = 0;
    const std::byte* inventory = g_inventoryField && g_stackStride ? pointerAt(actor, g_inventoryField) : nullptr;
    const std::byte* container = inventory ? pointerAt(inventory, kInvContainerOffset) : nullptr;
    if (!container) {
        if (canLog()) log().warn(L"Bot: cannot read the player's inventory (inventory +{:#x}, stride {:#x})", g_inventoryField, g_stackStride);
        return std::nullopt;
    }
    int filled = 0, unreadable = 0;
    for (std::size_t i = 0; i < kInventorySlots; ++i) {
        const void* stack = stackAt(container + kInvSlotsOffset, i, kInventorySlots);
        if (!stack) { ++unreadable; continue; }
        out.slots[i] = gamecodec::itemFromStack(stack);
        if (!out.slots[i].empty()) ++filled;
    }
    if (unreadable == static_cast<int>(kInventorySlots)) {
        if (canLog()) log().warn(L"Bot: the player's inventory items are unreadable");
        return std::nullopt;
    }
    int armor = 0;
    bool offhand = false;
    alignas(8) std::byte component[kMaxEquipStride] = {};
    if (g_equipStride && g_itemsOffset && GameData::copyComponent(actor, kEquipmentTypeId, g_equipStride, component)) {
        const std::byte* armorContainer = nullptr;
        const std::byte* handContainer = nullptr;
        std::memcpy(&armorContainer, component + g_armorField, sizeof armorContainer);
        std::memcpy(&handContainer, component + kHandField, sizeof handContainer);
        if (memory::plausiblePointer(armorContainer))
            for (std::size_t i = 0; i < kArmorSlots; ++i)
                if (const void* stack = stackAt(armorContainer + g_itemsOffset, i, kArmorSlots)) {
                    out.armor[i] = gamecodec::itemFromStack(stack);
                    if (!out.armor[i].empty()) ++armor;
                }
        if (memory::plausiblePointer(handContainer))
            if (const void* stack = stackAt(handContainer + g_itemsOffset, kOffhandSlot, kOffhandSlot + 1)) {
                out.offhand = gamecodec::itemFromStack(stack);
                offhand = !out.offhand.empty();
            }
    } else if (canLog()) log().warn(L"Bot: cannot read the player's equipment; armor and offhand are shown empty");
    std::wstring hotbar;
    for (std::size_t i = 0; i < 9; ++i) hotbar += out.slots[i].empty() ? L'.' : L'#';
    if (canLog())
        log().info(L"Bot: player inventory snapshot: {} of 36 slot(s) filled (hotbar {}), {} unreadable, armor {}, offhand {}",
            filled, hotbar, unreadable, armor, offhand ? L"yes" : L"no");
    return out;
}

}
