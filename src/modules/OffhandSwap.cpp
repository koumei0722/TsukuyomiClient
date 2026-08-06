#include "modules/OffhandSwap.h"

#include <Windows.h>

#include <cstring>
#include <utility>
#include <vector>

#include "config/Config.h"
#include "core/Logger.h"
#include "game/InventoryScreen.h"
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

}

OffhandSwap& OffhandSwap::instance()
{
    static OffhandSwap module;
    return module;
}

bool OffhandSwap::available() const
{

    return Scanner::instance().found(Target::SetSelectedSlot)
           && ItemStackRequest::instance().available();
}

void OffhandSwap::onScansReady()
{

    ItemStackRequest::instance().onScansReady();

    ItemStackOps::instance().onScansReady();

    LegacyTransaction::instance().onScansReady();

    if (!m_hadEnabledSetting) {
        setEnabled(true);
    }
}

void OffhandSwap::onSetSelectedSlot(void* holder)
{

    if (holder == nullptr) {
        return;
    }

    void* const previous = m_holder.exchange(holder, std::memory_order_acq_rel);
    if (previous != holder) {

        m_holderAlt.store(previous, std::memory_order_release);
    }
}

void OffhandSwap::onUpdate()
{

    const bool wants = m_swapKey.triggered();
    if (!enabled() || !input::isGameForeground()) {
        return;
    }
    if (wants) {

        m_requestedAtMs.store(GetTickCount64(), std::memory_order_relaxed);
        m_requestedSlot.store(slotUnderCursor(), std::memory_order_release);
    }
}

int OffhandSwap::slotUnderCursor()
{

    if (!instance().screenSwapEnabled()) {
        return kHeldSlot;
    }

    const InventoryScreen::Hovered hovered = InventoryScreen::instance().hovered();
    switch (hovered.area) {
    case InventoryScreen::Area::Hotbar:
    case InventoryScreen::Area::Inventory:
        return hovered.containerSlot;
    default:

        return kHeldSlot;
    }
}

bool OffhandSwap::onInventoryHotbarKey(const void* controller, int hotbarIndex)
{

    if (!m_screenSwap || !enabled() || hotbarIndex < 0 || hotbarIndex >= kHotbarSlots) {
        return false;
    }

    if (InventoryScreen::readFrom(controller).area != InventoryScreen::Area::Offhand) {
        return false;
    }

    m_requestedAtMs.store(GetTickCount64(), std::memory_order_relaxed);
    m_requestedSlot.store(hotbarIndex, std::memory_order_release);
    return true;
}

void OffhandSwap::onPlayerViewUpdate()
{

    if (!enabled()) {
        if (m_pending.active) {

            ItemStackOps::instance().discard();
        }
        m_pending = Pending{};
        m_netIdFix = NetIdFix{};
        m_requestedSlot.store(kNoRequest, std::memory_order_release);
        return;
    }

    servePending();

    serveNetIdFix();

    const int slot = m_requestedSlot.load(std::memory_order_acquire);
    if (slot == kNoRequest) {
        return;
    }

    if (m_pending.active || m_netIdFix.active) {
        if (GetTickCount64() < m_requestedAtMs.load(std::memory_order_relaxed) + kQueueWaitMs) {
            return;
        }

        m_requestedSlot.store(kNoRequest, std::memory_order_release);
        log().warn(L"OffhandSwap: dropped a swap, the previous one is still waiting for the "
                   L"server");
        return;
    }

    int expected = slot;
    if (!m_requestedSlot.compare_exchange_strong(expected, kNoRequest, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {

        return;
    }
    apply(slot);
}

void OffhandSwap::servePending()
{
    if (!m_pending.active) {
        return;
    }

    int result = 0;
    if (!ItemStackRequest::instance().takeResponse(m_pending.requestId, result)) {

        if (GetTickCount64() >= m_pending.giveUpAtMs) {
            log().warn(L"OffhandSwap: the server did not answer request {}, "
                       L"the hotbar and the server may disagree",
                       m_pending.requestId);
            ItemStackOps::instance().discard();
            m_pending = Pending{};
        }
        return;
    }

    if (result == ItemStackRequest::kResultSuccess) {

        ItemStackOps::instance().discard();
        m_pending = Pending{};
        return;
    }

    const bool restored = ItemStackOps::instance().restore(m_pending.hand[0]);

    m_pending = Pending{};

    if (!restored) {
        log().error(L"OffhandSwap: the server refused the swap (result {}) and the hotbar could "
                    L"not be put back, it may disagree with the server",
                    result);
        return;
    }
    log().warn(L"OffhandSwap: the server refused the swap (result {}), put the hotbar back",
               result);
}

void OffhandSwap::onInventoryContent(const void* payload)
{

    if (payload == nullptr || !memory::isReadable(payload, kPacketMinSize)) {
        return;
    }

    const auto* const base = static_cast<const std::byte*>(payload);
    const int containerId =
        static_cast<int>(*reinterpret_cast<const std::int8_t*>(base + kPacketContainerIdOffset));
    if (containerId != kOffhandContainerId) {

        return;
    }

    std::uint64_t range[2]{};
    std::memcpy(range, base + kPacketSlotsOffset, sizeof(range));
    if (range[0] == 0 || range[1] < range[0] + kEntrySize) {
        return;
    }

    const auto* const entry = reinterpret_cast<const std::byte*>(range[0]);
    if (!memory::isReadable(entry, kEntrySize)) {
        return;
    }
    const void* item = nullptr;
    std::int32_t netId = 0;
    std::memcpy(&item, entry + kEntryItemOffset, sizeof(item));
    std::memcpy(&netId, entry + kEntryNetIdOffset, sizeof(netId));
    if (item == nullptr || netId <= 0) {

        log().info(L"OffhandSwap: the server sent an empty offhand");
        return;
    }

    m_offhandNetId.store(netId, std::memory_order_relaxed);
    const unsigned int serial = m_offhandSerial.fetch_add(1, std::memory_order_release) + 1;

    log().info(L"OffhandSwap: the server sent the offhand (net id {}, packet {})", netId, serial);
}

void OffhandSwap::beginNetIdFix(const Hands& hands, int slot)
{
    if (m_netIdFix.active || slot < 0 || slot >= kSlotCount) {
        return;
    }

    std::byte* const hand = hands.slots + kSlotStride * slot;

    m_netIdFix = NetIdFix{};

    StackView handView;
    if (readStack(hand, handView) && handView.item != nullptr) {
        m_netIdFix.hand = hand;

        if (slot == hands.selected && hands.offhand != nullptr) {
            m_netIdFix.mainhand = hands.offhand + (kMainhandOffset - kOffhandOffset);
        }
    }
    m_netIdFix.slot = slot;

    m_netIdFix.offhand = hands.offhand;

    m_netIdFix.serial = m_offhandSerial.load(std::memory_order_acquire);
    m_netIdFix.giveUpAtMs = GetTickCount64() + kNetIdWaitMs;
    m_netIdFix.active = true;

    StackView offhandView;
    readStack(hands.offhand, offhandView);
    m_netIdFix.handValue = handView.netValue;
    m_netIdFix.offhandValue = offhandView.netValue;

    log().info(L"OffhandSwap: waiting for the server ({}, slot {}, net ids {} / {}, packet {})",
               (m_netIdFix.hand != nullptr) ? L"lining up" : L"gate only", slot,
               m_netIdFix.handValue, m_netIdFix.offhandValue, m_netIdFix.serial);
}

void OffhandSwap::serveNetIdFix()
{
    if (!m_netIdFix.active) {
        return;
    }

    if (m_offhandSerial.load(std::memory_order_acquire) == m_netIdFix.serial) {

        if (GetTickCount64() >= m_netIdFix.giveUpAtMs) {

            if (m_netIdFix.hand == nullptr) {

                log().warn(L"OffhandSwap: the server did not send the offhand back after "
                           L"slot {} was moved into it, the next swap may be refused",
                           m_netIdFix.slot);
            } else {
                log().warn(L"OffhandSwap: the server did not send the offhand back, "
                           L"slot {} keeps its old net id and may not be movable in the "
                           L"inventory screen until you use or move it",
                           m_netIdFix.slot);
            }
            m_netIdFix = NetIdFix{};
        }
        return;
    }

    const std::int32_t offhandNetId = m_offhandNetId.load(std::memory_order_relaxed);

    const bool serverHeldNetIds = (offhandNetId == m_netIdFix.handValue);
    const bool serverMovedNetIds = (offhandNetId == m_netIdFix.offhandValue);

    const std::int32_t offhandWanted = serverHeldNetIds ? m_netIdFix.handValue : offhandNetId;

    StackView offhandView;
    if (readStack(m_netIdFix.offhand, offhandView) && offhandView.item != nullptr
        && offhandView.netValue != offhandWanted
        && ItemStackOps::instance().setNetIdValue(m_netIdFix.offhand, offhandWanted)) {
        log().info(L"OffhandSwap: lined the offhand up with the server (net id {} -> {})",
                   offhandView.netValue, offhandWanted);
    }

    if (m_netIdFix.hand == nullptr) {
        log().info(L"OffhandSwap: the offhand came back (net id {}), the gate for slot {} is open",
                   offhandNetId, m_netIdFix.slot);
        m_netIdFix = NetIdFix{};
        return;
    }

    if (serverMovedNetIds) {

        log().info(L"OffhandSwap: the server moved the net ids too, slot {} is already right",
                   m_netIdFix.slot);
        m_netIdFix = NetIdFix{};
        return;
    }

    StackView handView;
    if (!readStack(m_netIdFix.hand, handView) || handView.item == nullptr) {
        m_netIdFix = NetIdFix{};
        return;
    }

    std::int32_t wanted = 0;
    if (serverHeldNetIds) {

        wanted = m_netIdFix.offhandValue;
    } else {

        wanted = offhandNetId - 1;
        if (wanted <= handView.netValue) {
            log().warn(L"OffhandSwap: the offhand came back with net id {} but slot {} holds {} "
                       L"(before: {} / {}), leaving it alone",
                       offhandNetId, m_netIdFix.slot, handView.netValue, m_netIdFix.handValue,
                       m_netIdFix.offhandValue);
            m_netIdFix = NetIdFix{};
            return;
        }
    }

    if (wanted != handView.netValue
        && ItemStackOps::instance().setNetIdValue(m_netIdFix.hand, wanted)) {
        log().success(L"OffhandSwap: lined slot {} up with the server (net id {} -> {}, {})",
                      m_netIdFix.slot, handView.netValue, wanted,
                      serverHeldNetIds ? L"the server kept them in place" : L"newly issued");
    }

    StackView mirrorView;
    if (m_netIdFix.mainhand != nullptr && readStack(m_netIdFix.mainhand, mirrorView)
        && mirrorView.item == handView.item && mirrorView.netValue == handView.netValue) {
        ItemStackOps::instance().setNetIdValue(m_netIdFix.mainhand, wanted);
    }
    m_netIdFix = NetIdFix{};
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
    if (!readPointerGuarded(static_cast<std::byte*>(container) + kSlotsOffset, slots)) {
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

bool OffhandSwap::apply(int slot)
{

    if (slot != kHeldSlot && (slot < 0 || slot >= kSlotCount)) {
        return false;
    }

    Hands hands[2];
    int count = 0;
    void* const holders[2] = {m_holder.load(std::memory_order_acquire),
                              m_holderAlt.load(std::memory_order_acquire)};
    for (void* const holder : holders) {
        Hands resolved;
        if (!resolve(holder, resolved)) {
            continue;
        }

        bool duplicate = false;
        for (int i = 0; i < count; ++i) {
            duplicate = duplicate || hands[i].container == resolved.container;
        }
        if (duplicate) {
            continue;
        }
        hands[count] = resolved;
        ++count;
    }

    if (count == 0) {

        log().info(L"OffhandSwap: could not reach the hands for slot {} "
                   L"(holders {:#x} / {:#x})",
                   slot, reinterpret_cast<std::uintptr_t>(holders[0]),
                   reinterpret_cast<std::uintptr_t>(holders[1]));
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

    const bool serverAuthoritative = count <= 1;

    if (serverAuthoritative) {

        const int firstSlot = targetSlot(hands[0], slot);
        std::byte* const firstStack = hands[0].slots + kSlotStride * firstSlot;

        StackView handView;
        StackView offhandView;
        const bool bothRead =
            readStack(firstStack, handView) && readStack(hands[0].offhand, offhandView);

        if (bothRead && handView.item == nullptr && offhandView.item == nullptr) {
            log().info(L"OffhandSwap: slot {} and the offhand are both empty, nothing to "
                       L"swap (the server may still be writing the offhand back)",
                       firstSlot);
            return false;
        }

        const bool takeOutOnly =
            bothRead && handView.item == nullptr && offhandView.item != nullptr;

        const bool useRequestPath = takeOutOnly;

        if (!useRequestPath && LegacyTransaction::instance().swap(
                hands[0].player, LegacyTransaction::inventorySlot(firstSlot, firstStack),
                LegacyTransaction::offhand(hands[0].offhand))) {

            const int applied = swapWithOffhand(hands, count, slot);
            if (applied == 0) {
                log().warn(L"OffhandSwap: the server was told to swap but the client side "
                           L"could not follow, the view may be stale until you reopen it");
                return false;
            }

            beginNetIdFix(hands[0], firstSlot);

            log().success(L"OffhandSwap: swapped slot {} with the offhand through the "
                          L"legacy path ({} target{})",
                          firstSlot, applied, (applied == 1) ? L"" : L"s");
            return true;
        }

        const ItemStackRequest::SlotRef hand =
            ItemStackRequest::inventorySlot(hands[0].slots, firstSlot);
        const ItemStackRequest::SlotRef offhand =
            ItemStackRequest::offhandSlot(hands[0].offhand);

        if (!ItemStackRequest::instance().requestSwap(hand, offhand)) {

            log().warn(L"OffhandSwap: the swap request for slot {} could not be sent "
                       L"(server thinks the screen is {})",
                       firstSlot,
                       ItemStackRequest::instance().serverInventoryOpen() ? L"open" : L"closed");
            return false;
        }

        ItemStackOps::instance().stash(firstStack);

        const int applied = swapWithOffhand(hands, count, slot);

        m_pending = Pending{};
        m_pending.active = true;
        m_pending.requestId = ItemStackRequest::instance().lastRequestId();
        m_pending.count = count;
        m_pending.selected = firstSlot;
        for (int i = 0; i < count; ++i) {
            m_pending.hand[i] = hands[i].slots + kSlotStride * targetSlot(hands[i], slot);
            m_pending.offhand[i] = hands[i].offhand;
        }
        m_pending.giveUpAtMs = GetTickCount64() + kResponseWaitMs;

        if (applied == 0) {

            log().warn(L"OffhandSwap: the request went out but the client side could not be "
                       L"updated, your hotbar may look stale until you use the item");
            return false;
        }

        log().success(L"OffhandSwap: swapped slot {} with the offhand ({} target{})", firstSlot,
                      applied, (applied == 1) ? L"" : L"s");
        return true;
    }

    const int applied = swapWithOffhand(hands, count, slot);

    if (applied == 0) {
        log().warn(L"OffhandSwap: could not swap the stacks, nothing changed");
        return false;
    }

    log().success(L"OffhandSwap: swapped slot {} with the offhand ({} targets)",
                  targetSlot(hands[0], slot), applied);
    return true;
}

MenuItem OffhandSwap::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(menu::back());
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    children.push_back(menu::keybind(
        L"Swap key", [this] { return m_swapKey.combo(); },
        [this](std::vector<int> combo) {
            m_swapKey.set(std::move(combo));
            log().info(L"OffhandSwap: swap key set to {}", m_swapKey.name());
        }));

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void OffhandSwap::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    m_hadEnabledSetting = section.find("enabled") != section.end();

    m_screenSwap = Config::getBool(section, "screenSwap", false);
    m_legacyBridge = Config::getBool(section, "legacyBridge", false);

    const auto it = section.find("swapKeys");
    if (it == section.end() || !it->is_array()) {
        m_swapKey.set({'F'});
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

    section["screenSwap"] = m_screenSwap;
    section["legacyBridge"] = m_legacyBridge;
}

}
