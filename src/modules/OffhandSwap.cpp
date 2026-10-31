#include "modules/OffhandSwap.h"

#include <Windows.h>

#include <cstring>
#include <utility>
#include <vector>

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Notice.h"
#include "game/InventoryResync.h"
#include "game/ItemStackOps.h"
#include "game/ItemStackRequest.h"
#include "game/LegacyTransaction.h"
#include "input/Foreground.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "modules/HandRestock.h"

namespace tsukuyomi {

namespace {

int accessViolationFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

bool readPointerGuarded(const void* address, void*& value)
{
    __try {
        value = *static_cast<void* const*>(address);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool readIntGuarded(const void* address, int& value)
{
    __try {
        value = *static_cast<const int*>(address);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool readU8Guarded(const void* address, std::uint8_t& value)
{
    __try {
        value = *static_cast<const std::uint8_t*>(address);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool readI32Guarded(const void* address, std::int32_t& value)
{
    __try {
        value = *static_cast<const std::int32_t*>(address);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

struct ModuleRange {
    const std::byte* base = nullptr;
    std::size_t size = 0;

    bool contains(const void* address) const
    {
        const auto* const value = static_cast<const std::byte*>(address);
        return base != nullptr && value >= base && value < base + size;
    }
};

const ModuleRange& mainModule()
{
    static const ModuleRange range = [] {
        ModuleRange result;
        const auto* const base = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));
        if (base == nullptr) {
            return result;
        }
        const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return result;
        }
        const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            return result;
        }
        result.base = base;
        result.size = nt->OptionalHeader.SizeOfImage;
        return result;
    }();
    return range;
}

constexpr std::uintptr_t kUserAddressLimit = 0x0000800000000000ULL;

bool looksLikeHeapObject(const void* value)
{
    const auto address = reinterpret_cast<std::uintptr_t>(value);
    return address != 0 && address % sizeof(void*) == 0 && address < kUserAddressLimit
           && !mainModule().contains(value);
}

std::vector<int> defaultSwapKeys() { return {'F'}; }

}

OffhandSwap& OffhandSwap::instance()
{
    static OffhandSwap module;
    return module;
}

bool OffhandSwap::available() const
{
    return Scanner::instance().found(Target::SetSelectedSlot);
}

void OffhandSwap::onScansReady()
{
    ItemStackRequest::instance().onScansReady();

    ItemStackOps::instance().onScansReady();

    LegacyTransaction::instance().onScansReady();

}

void OffhandSwap::onUpdate()
{
    const bool wants = m_swapKey.triggered();
    if (!enabled() || !input::isInGameplay()) {
        return;
    }
    if (wants) {
        m_requestedAtMs.store(GetTickCount64(), std::memory_order_relaxed);
        m_requestedSlot.store(kHeldSlot, std::memory_order_release);
    }
}

void OffhandSwap::onPlayerViewUpdate()
{
    if (!enabled()) {
        m_requestedSlot.store(kNoRequest, std::memory_order_release);
        return;
    }

    const int slot = m_requestedSlot.load(std::memory_order_acquire);
    if (slot == kNoRequest) {
        return;
    }

    const unsigned long long requestedAt = m_requestedAtMs.load(std::memory_order_relaxed);
    if (const unsigned long long nowMs = GetTickCount64(); nowMs > requestedAt + kStaleRequestMs) {
        int expected = slot;
        if (m_requestedSlot.compare_exchange_strong(expected, kNoRequest, std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            log().warn(L"OffhandSwap: dropped a swap pressed {} ms ago (the game left the world or a menu was open "
                       L"before it could run)",
                       nowMs - requestedAt);
        }
        return;
    }

    int expected = slot;
    if (!m_requestedSlot.compare_exchange_strong(expected, kNoRequest, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
        return;
    }
    apply();
}

void OffhandSwap::onInventoryContent(const void* payload)
{
    if (payload == nullptr || !memory::isReadable(payload, kPacketMinSize)) {
        return;
    }
    const auto* const base = static_cast<const std::byte*>(payload);
    const int containerId =
        static_cast<int>(*reinterpret_cast<const std::int8_t*>(base + kPacketContainerIdOffset));
    ItemStackRequest::instance().noteInventoryContent(containerId);
    InventoryResync::instance().onInventoryContent(containerId, payload);
}

bool OffhandSwap::readStack(const std::byte* stack, StackView& out) const
{
    if (stack == nullptr) {
        return false;
    }
    StackView view;
    if (!readPointerGuarded(stack, view.vtable)
        || !readPointerGuarded(stack + kStackItemOffset, view.item)
        || !readU8Guarded(stack + kStackCountOffset, view.count)
        || !readI32Guarded(stack + kStackNetValueOffset, view.netValue)) {
        return false;
    }
    out = view;
    return true;
}

bool OffhandSwap::looksLikeInventory(std::byte* slots) const
{
    if (slots == nullptr) {
        return false;
    }

    void* first = nullptr;
    if (!readPointerGuarded(slots, first)) {
        return false;
    }
    if (!mainModule().contains(first)) {
        return false;
    }

    for (int slot = 1; slot < kSlotCount; ++slot) {
        void* value = nullptr;
        if (!readPointerGuarded(slots + kSlotStride * slot, value) || value != first) {
            return false;
        }
    }
    return true;
}

bool OffhandSwap::resolve(void* holder, Hands& out) const
{
    if (holder == nullptr) {
        return false;
    }

    auto* const base = static_cast<std::byte*>(holder);

    int selected = -1;
    if (!readIntGuarded(base + kSelectedSlotOffset, selected) || selected < 0
        || selected >= kHotbarSlots) {
        return false;
    }

    void* container = nullptr;
    if (!readPointerGuarded(base + kContainerOffset, container)
        || !looksLikeHeapObject(container)) {
        return false;
    }

    void* slots = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(container) + kSlotsOffset, slots)
        || !looksLikeHeapObject(slots)) {
        return false;
    }
    auto* const array = static_cast<std::byte*>(slots);
    if (!looksLikeInventory(array)) {
        return false;
    }

    void* player = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(container) + kPlayerOffset, player)
        || !looksLikeHeapObject(player)) {
        return false;
    }

    auto* const playerBytes = static_cast<std::byte*>(player);
    std::byte* const offhand = playerBytes + kOffhandOffset;
    std::byte* const mainhand = playerBytes + kMainhandOffset;

    StackView selectedView;
    StackView mainhandView;
    if (!readStack(array + kSlotStride * selected, selectedView)
        || !readStack(mainhand, mainhandView)) {
        return false;
    }
    if (mainhandView.vtable != selectedView.vtable || mainhandView.item != selectedView.item
        || mainhandView.count != selectedView.count) {
        return false;
    }

    StackView offhandView;
    if (!readStack(offhand, offhandView) || offhandView.vtable != selectedView.vtable) {
        return false;
    }

    out.holder = holder;
    out.container = container;
    out.player = player;
    out.slots = array;
    out.offhand = offhand;
    out.selected = selected;
    return true;
}

int OffhandSwap::swapWithOffhand(const Hands* hands, int count, int slot)
{
    int applied = 0;
    for (int i = 0; i < count; ++i) {
        const int target = targetSlot(hands[i], slot);
        std::byte* const stack = hands[i].slots + kSlotStride * target;
        if (!ItemStackOps::instance().swap(stack, hands[i].offhand)) {
            continue;
        }
        ++applied;

        ItemStackOps::instance().notifySlotChanged(hands[i].container, target);
    }
    return applied;
}

bool OffhandSwap::apply()
{
    constexpr int slot = kHeldSlot;

    void* holders[2] = {};
    HandRestock::instance().ownHolders(holders[0], holders[1]);
    Hands hands[1];
    if (!resolve(holders[0], hands[0])) {
        log().info(L"OffhandSwap: could not reach the hands for slot {} (holder {:#x})", slot,
                   reinterpret_cast<std::uintptr_t>(holders[0]));
        const unsigned long long now = GetTickCount64();
        if (now >= m_nextWarnMs.load(std::memory_order_acquire)) {
            m_nextWarnMs.store(now + kWarnIntervalMs, std::memory_order_release);
            log().warn(L"OffhandSwap: could not reach the hands, "
                       L"switch your hotbar slot once (1-9) and try again");
        }
        return false;
    }
    m_nextWarnMs.store(0, std::memory_order_release);

    HandRestock::instance().noteDeliberateMove();

    const int firstSlot = targetSlot(hands[0], slot);
    std::byte* const firstStack = hands[0].slots + kSlotStride * firstSlot;

    StackView handView;
    StackView offhandView;
    const bool bothRead = readStack(firstStack, handView) && readStack(hands[0].offhand, offhandView);

    if (bothRead && handView.item == nullptr && offhandView.item == nullptr) {
        log().info(L"OffhandSwap: slot {} and the offhand are both empty, nothing to "
                   L"swap (the server may still be writing the offhand back)",
                   firstSlot);
        return false;
    }

    if (!LegacyTransaction::instance().swap(
            hands[0].player, LegacyTransaction::inventorySlot(firstSlot, firstStack),
            LegacyTransaction::offhand(hands[0].offhand))) {
        if (!LegacyTransaction::instance().available()) {
            notice::failOnce("OffhandSwap.legacy",
                             L"OffhandSwap: the legacy inventory path is unavailable, so swapping with the "
                             L"offhand is off",
                             "OffhandSwap cannot swap with the offhand: the legacy inventory path is unavailable");
        }
        log().warn(L"OffhandSwap: the legacy swap for slot {} could not be sent; nothing changed", firstSlot);
        return false;
    }

    const int applied = swapWithOffhand(hands, 1, slot);
    if (applied == 0) {
        log().warn(L"OffhandSwap: the server was told to swap but the client side "
                   L"could not follow, the view may be stale until you reopen it");
        return false;
    }
    InventoryResync::instance().markStale();

    static int logs1 = 0;
    if (logs1 < 200) {
        ++logs1;
        log().success(L"OffhandSwap: swapped slot {} with the offhand through the legacy path", firstSlot);
    }
    return true;
}

MenuItem OffhandSwap::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    children.push_back(menu::keybind(
        L"Swap key", [this] { return m_swapKey.combo(); },
        [this](std::vector<int> combo) {
            m_swapKey.set(std::move(combo));
            log().info(L"OffhandSwap: swap key set to {}", m_swapKey.name());
        },
        defaultSwapKeys()));
    bindPad(children.back(), m_swapKey);

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void OffhandSwap::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);

    const auto it = section.find("swapKeys");
    if (it == section.end() || !it->is_array()) {
        m_swapKey.set(defaultSwapKeys());
        return;
    }
    std::vector<int> combo;
    for (const auto& value : *it) {
        if (value.is_number_integer()) {
            combo.push_back(value.get<int>());
        }
    }
    m_swapKey.set(std::move(combo));
}

void OffhandSwap::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["swapKeys"] = m_swapKey.combo();
}

}
