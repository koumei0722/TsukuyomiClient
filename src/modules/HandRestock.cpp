#include "modules/HandRestock.h"

#include <Windows.h>

#include "core/Logger.h"
#include "game/ItemStackOps.h"
#include "game/ItemStackRequest.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

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

bool readU16Guarded(const void* address, std::uint16_t& value)
{
    __try {
        value = *static_cast<const std::uint16_t*>(address);
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

using SwapSlotsRaw = void(__fastcall*)(void*, int, int);

int faultFilter(EXCEPTION_POINTERS* info, const void** faultPc, const void** faultAddress)
{
    const unsigned long code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    *faultPc = info->ExceptionRecord->ExceptionAddress;
    *faultAddress = info->ExceptionRecord->NumberParameters >= 2
                        ? reinterpret_cast<const void*>(
                              info->ExceptionRecord->ExceptionInformation[1])
                        : nullptr;
    return EXCEPTION_EXECUTE_HANDLER;
}

bool callSwapGuarded(SwapSlotsRaw fn, void* container, int slotA, int slotB,
                     const void** faultPc, const void** faultAddress)
{
    __try {
        fn(container, slotA, slotB);
        return true;
    } __except (faultFilter(GetExceptionInformation(), faultPc, faultAddress)) {
        return false;
    }
}

struct ModuleRange {
    const std::byte* base = nullptr;
    size_t size = 0;

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

}

HandRestock& HandRestock::instance()
{
    static HandRestock module;
    return module;
}

bool HandRestock::available() const
{

    return ItemStackRequest::instance().available();
}

void HandRestock::onScansReady()
{
    m_swapSlots = Scanner::instance().addressAs<SwapSlotsFn>(Target::SwapSlots);
    if (m_swapSlots == nullptr) {
        log().warn(L"HandRestock: swapSlots was not found, "
                   L"the hand will be refilled on the server but may look stale");
    }

    ItemStackRequest::instance().onScansReady();

    ItemStackOps::instance().onScansReady();
}

void HandRestock::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section.erase("inventoryKey");

    section.erase("testKeys");

    section.erase("keepServerInventoryOpen");
}

void HandRestock::onEnabledChanged(bool )
{

    for (int spot = 0; spot < kSpotCount; ++spot) {
        m_last[spot] = HandState{};
        m_pending[spot] = Pending{};
    }
}

void HandRestock::onSetSelectedSlot(void* holder)
{

    if (holder == nullptr) {
        return;
    }

    void* const previous = m_holder.exchange(holder, std::memory_order_acq_rel);
    if (previous != holder) {

        m_holderAlt.store(previous, std::memory_order_release);
    }
}

void HandRestock::onPlayerViewUpdate()
{

    if (!enabled()) {

        clearOutstanding();
        for (int spot = 0; spot < kSpotCount; ++spot) {
            m_last[spot] = HandState{};
            m_pending[spot] = Pending{};
        }
        return;
    }

    serveOutstanding();

    servePending(kSpotHand);
    servePending(kSpotOffhand);

    Inventory inventory;
    if (!resolveClient(inventory)) {

        for (int spot = 0; spot < kSpotCount; ++spot) {
            m_last[spot] = HandState{};
        }
        return;
    }

    const bool clientSide = isClientSidePlayer(inventory.playerRaw);
    if (!m_loggedClientSide || clientSide != m_clientSideKnown) {
        m_loggedClientSide = true;
        m_clientSideKnown = clientSide;
        log().info(L"HandRestock: watching the {} inventory", clientSide
                       ? L"client-side"
                       : L"first reachable one (could not tell the client side apart)");
    }

    SlotView hand;
    if (readSlot(inventory.slots, inventory.hand, hand)) {
        watch(kSpotHand, inventory, hand);
    } else {
        m_last[kSpotHand] = HandState{};
    }

    SlotView offhand;
    if (inventory.offhand != nullptr && readStackAt(inventory.offhand, offhand)) {
        watch(kSpotOffhand, inventory, offhand);
    } else {
        m_last[kSpotOffhand] = HandState{};
    }
}

void HandRestock::noteDeliberateMove()
{

    const Clock::time_point until = Clock::now() + std::chrono::milliseconds(kIgnoreMoveMs);
    for (int spot = 0; spot < kSpotCount; ++spot) {
        m_ignoreUntil[spot] = until;

        m_pending[spot] = Pending{};
    }
}

void HandRestock::watch(Spot spot, const Inventory& inventory, const SlotView& view)
{
    const int slot = (spot == kSpotHand) ? inventory.hand : -1;

    const int total = (view.item != nullptr && view.count > 0)
                          ? countItem(inventory, view.item, view.aux)
                          : 0;

    const HandState previous = m_last[spot];
    m_last[spot] = HandState{inventory.container, slot,     view.item,
                             view.block,          view.aux, view.count,
                             total};

    const bool wentEmpty = previous.item != nullptr && previous.count > 0
                           && (view.item == nullptr || view.count == 0);
    const bool sameSpot = previous.container == inventory.container && previous.slot == slot;
    const bool ranOut = sameSpot && wentEmpty;
    if (!ranOut) {

        if (wentEmpty) {
            log().info(L"HandRestock: the {} went empty but the holder moved "
                       L"(container {:#x} -> {:#x}, slot {} -> {}), not counting it as used up",
                       (spot == kSpotHand) ? L"hand" : L"offhand",
                       reinterpret_cast<uintptr_t>(previous.container),
                       reinterpret_cast<uintptr_t>(inventory.container), previous.slot, slot);
        }
        return;
    }

    if (Clock::now() < m_ignoreUntil[spot]) {
        dropPending(spot, L"it was moved on purpose, not used up");
        return;
    }

    const Clock::time_point now = Clock::now();
    m_pending[spot].active = true;
    m_pending[spot].destSlot = slot;
    m_pending[spot].item = previous.item;
    m_pending[spot].block = previous.block;
    m_pending[spot].aux = previous.aux;
    m_pending[spot].totalBefore = previous.total;
    m_pending[spot].at = now + std::chrono::milliseconds(kSettleMs);
    m_pending[spot].giveUpAt = now + std::chrono::milliseconds(kGiveUpMs);
}

void HandRestock::dropPending(Spot spot, const wchar_t* why)
{

    log().info(L"HandRestock: gave up refilling the {} ({})",
               (spot == kSpotHand) ? L"hand" : L"offhand", why);
    m_pending[spot] = Pending{};
}

void HandRestock::servePending(Spot spot)
{
    if (!m_pending[spot].active) {
        return;
    }

    const Clock::time_point now = Clock::now();
    if (now < m_pending[spot].at) {
        return;
    }

    if (now >= m_pending[spot].giveUpAt) {
        dropPending(spot, L"timed out before it could be sent");
        return;
    }

    if (now < m_nextRefillAt[spot]) {
        return;
    }

    const Pending pending = m_pending[spot];

    Inventory inventory;
    if (!resolveClient(inventory)) {

        return;
    }

    if (spot == kSpotHand && inventory.hand != pending.destSlot) {
        dropPending(spot, L"the selected hotbar slot changed while waiting");
        return;
    }
    if (spot == kSpotOffhand && inventory.offhand == nullptr) {

        return;
    }
    if (!emptyEverywhere(spot, pending.destSlot)) {

        dropPending(spot, L"the slot is not empty in every reachable container");
        return;
    }

    const int totalNow = countItem(inventory, pending.item, pending.aux);
    if (totalNow >= pending.totalBefore) {
        log().info(L"HandRestock: the {} went empty but the total did not drop ({} -> {}), "
                   L"treating it as a move, not a use",
                   (spot == kSpotHand) ? L"hand" : L"offhand", pending.totalBefore, totalNow);
        m_pending[spot] = Pending{};
        return;
    }

    SlotView wanted;
    wanted.item = pending.item;
    wanted.block = pending.block;
    wanted.aux = pending.aux;

    const int keepSlot = (spot == kSpotHand) ? pending.destSlot : inventory.hand;
    const int source = findSource(inventory, wanted, keepSlot);
    if (source < 0) {

        dropPending(spot, L"there is no matching stack left to refill from");
        return;
    }

    SlotView sourceView;
    readSlot(inventory.slots, source, sourceView);

    const int applied = applyRefill(spot, pending.destSlot, source);
    if (applied == 0) {

        m_nextRefillAt[spot] = now + std::chrono::milliseconds(kRetryMs);
        return;
    }

    m_pending[spot] = Pending{};
    m_nextRefillAt[spot] = now + std::chrono::milliseconds(kCooldownMs);

    SlotView refilled;
    const bool reread = (spot == kSpotHand)
                            ? readSlot(inventory.slots, pending.destSlot, refilled)
                            : readStackAt(inventory.offhand, refilled);
    if (reread) {
        m_last[spot] = HandState{inventory.container, pending.destSlot, refilled.item,
                                 refilled.block,      refilled.aux,     refilled.count};
    }

    const wchar_t* const where = (spot == kSpotHand) ? L"hand" : L"offhand";

    if (refilled.count == 0) {
        log().warn(L"HandRestock: refilled the {} from slot {} but it still reads empty", where,
                   source);
        return;
    }

    log().success(L"HandRestock: the {} ran out, refilled from slot {} (x{})", where, source,
                  sourceView.count);
}

bool HandRestock::emptyEverywhere(Spot spot, int destSlot) const
{
    void* const holders[2] = {m_holder.load(std::memory_order_acquire),
                              m_holderAlt.load(std::memory_order_acquire)};
    int seen = 0;
    for (void* const holder : holders) {
        Inventory inventory;
        if (!resolve(holder, inventory)) {
            continue;
        }
        SlotView view;
        if (spot == kSpotHand) {
            if (!readSlot(inventory.slots, destSlot, view)) {
                return false;
            }
        } else {

            if (inventory.offhand == nullptr) {
                continue;
            }
            if (!readStackAt(inventory.offhand, view)) {
                return false;
            }
        }
        if (view.item != nullptr && view.count > 0) {
            return false;
        }
        ++seen;
    }
    return seen > 0;
}

int HandRestock::countItem(const Inventory& inventory, void* item, std::uint16_t aux) const
{
    if (item == nullptr) {
        return 0;
    }

    int total = 0;
    for (int slot = 0; slot < kSlotCount; ++slot) {
        SlotView view;
        if (readSlot(inventory.slots, slot, view) && view.item == item && view.aux == aux) {
            total += view.count;
        }
    }

    SlotView offhandView;
    if (inventory.offhand != nullptr && readStackAt(inventory.offhand, offhandView)
        && offhandView.item == item && offhandView.aux == aux) {
        total += offhandView.count;
    }

    total += countUiItems(inventory, item, aux);
    return total;
}

int HandRestock::countUiItems(const Inventory& inventory, void* item, std::uint16_t aux) const
{
    if (inventory.player == nullptr) {
        return 0;
    }

    auto* const base = static_cast<std::byte*>(inventory.player);
    void* first = nullptr;
    void* last = nullptr;
    if (!readPointerGuarded(base + kUiSlotsFirstOffset, first)
        || !readPointerGuarded(base + kUiSlotsLastOffset, last) || first == nullptr
        || last == nullptr) {
        return 0;
    }

    const auto span = static_cast<std::byte*>(last) - static_cast<std::byte*>(first);
    if (span <= 0 || (span % kSlotStride) != 0) {
        return 0;
    }
    const auto count = span / kSlotStride;
    if (count > kUiSlotLimit) {
        return 0;
    }

    int total = 0;
    for (std::ptrdiff_t i = 0; i < count; ++i) {
        SlotView view;
        if (readStackAt(static_cast<std::byte*>(first) + kSlotStride * i, view)
            && view.item == item && view.aux == aux) {
            total += view.count;
        }
    }
    return total;
}

int HandRestock::findSource(const Inventory& inventory, const SlotView& wanted,
                            int keepSlot) const
{
    if (wanted.item == nullptr) {
        return -1;
    }

    const auto matches = [&](const SlotView& view) {
        return view.item == wanted.item && view.aux == wanted.aux && view.count > 0;
    };

    for (int slot = kHotbarSlots; slot < kSlotCount; ++slot) {
        SlotView view;
        if (readSlot(inventory.slots, slot, view) && matches(view)) {
            return slot;
        }
    }

    for (int slot = 0; slot < kHotbarSlots; ++slot) {
        if (slot == keepSlot) {
            continue;
        }
        SlotView view;
        if (readSlot(inventory.slots, slot, view) && matches(view)) {
            return slot;
        }
    }
    return -1;
}

bool HandRestock::looksLikeInventory(std::byte* slots) const
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

bool HandRestock::readStackAt(const std::byte* stack, SlotView& out) const
{
    if (stack == nullptr) {
        return false;
    }

    SlotView view;
    if (!readPointerGuarded(stack, view.vtable)
        || !readPointerGuarded(stack + kItemOffset, view.item)
        || !readPointerGuarded(stack + kBlockOffset, view.block)
        || !readU16Guarded(stack + kAuxOffset, view.aux)
        || !readU8Guarded(stack + kCountOffset, view.count)
        || !readIntGuarded(stack + kNetValueOffset, view.netValue)) {
        return false;
    }
    out = view;
    return true;
}

bool HandRestock::readSlot(std::byte* slots, int index, SlotView& out) const
{
    if (slots == nullptr || index < 0 || index >= kSlotCount) {
        return false;
    }
    return readStackAt(slots + kSlotStride * index, out);
}

bool HandRestock::resolve(void* holder, Inventory& out) const
{
    if (holder == nullptr) {
        return false;
    }

    auto* const base = static_cast<std::byte*>(holder);

    int hand = -1;
    if (!readIntGuarded(base + kSelectedSlotOffset, hand) || hand < 0 || hand >= kHotbarSlots) {
        return false;
    }

    void* container = nullptr;
    if (!readPointerGuarded(base + kContainerOffset, container) || container == nullptr) {
        return false;
    }

    void* slots = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(container) + kSlotsOffset, slots)
        || slots == nullptr) {
        return false;
    }

    auto* const array = static_cast<std::byte*>(slots);
    if (!looksLikeInventory(array)) {
        return false;
    }

    out = Inventory{};
    out.holder = holder;
    out.container = container;
    out.slots = array;
    out.hand = hand;

    void* player = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(container) + kPlayerOffset, player)
        || player == nullptr) {
        return true;
    }

    out.playerRaw = player;

    auto* const playerBytes = static_cast<std::byte*>(player);
    SlotView selectedView;
    SlotView mainhandView;
    SlotView offhandView;
    if (!readSlot(array, hand, selectedView)
        || !readStackAt(playerBytes + kMainhandOffset, mainhandView)
        || !readStackAt(playerBytes + kOffhandOffset, offhandView)) {
        return true;
    }
    if (mainhandView.vtable != selectedView.vtable || mainhandView.item != selectedView.item
        || mainhandView.netValue != selectedView.netValue
        || offhandView.vtable != selectedView.vtable) {
        return true;
    }

    out.player = player;
    out.offhand = playerBytes + kOffhandOffset;
    return true;
}

bool HandRestock::isClientSidePlayer(void* player) const
{
    if (player == nullptr) {
        return false;
    }
    void* vtable = nullptr;
    if (!readPointerGuarded(player, vtable) || vtable == nullptr) {
        return false;
    }
    std::byte* const ref = Scanner::instance().address(Target::PlayerVtableRef);
    if (ref == nullptr) {
        return false;
    }
    return vtable == memory::ripTarget(ref, kLocalPlayerVtableDisp);
}

bool HandRestock::resolveClient(Inventory& out) const
{
    void* const holders[2] = {m_holder.load(std::memory_order_acquire),
                              m_holderAlt.load(std::memory_order_acquire)};

    Inventory fallback;
    bool haveFallback = false;

    for (void* const holder : holders) {
        Inventory inventory;
        if (!resolve(holder, inventory)) {
            continue;
        }
        if (!haveFallback) {
            fallback = inventory;
            haveFallback = true;
        }
        if (isClientSidePlayer(inventory.playerRaw)) {
            out = inventory;
            return true;
        }
    }

    if (!haveFallback) {
        return false;
    }
    out = fallback;
    return true;
}

bool HandRestock::predictRefill(const Inventory& inventory, Spot spot, int destSlot,
                                int sourceSlot)
{

    if (spot == kSpotOffhand) {

        if (inventory.offhand == nullptr) {
            return false;
        }
        if (!ItemStackOps::instance().swap(inventory.slots + kSlotStride * sourceSlot,
                                           inventory.offhand)) {
            return false;
        }

        notifyRefilled(inventory.container, -1, sourceSlot);
        return true;
    }

    if (m_swapSlots == nullptr) {
        return false;
    }

    const void* faultPc = nullptr;
    const void* faultAddress = nullptr;
    if (callSwapGuarded(reinterpret_cast<SwapSlotsRaw>(m_swapSlots), inventory.container, destSlot,
                        sourceSlot, &faultPc, &faultAddress)) {

        notifyRefilled(inventory.container, destSlot, sourceSlot);
        return true;
    }

    const ModuleRange& module = mainModule();
    const auto rva = module.contains(faultPc)
                         ? static_cast<size_t>(static_cast<const std::byte*>(faultPc) - module.base)
                         : 0;
    log().error(L"HandRestock: swapSlots faulted (pc rva {:#x}, touched {:#x}), dropping the holder",
                rva, reinterpret_cast<uintptr_t>(faultAddress));

    void* expected = inventory.holder;
    m_holder.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    expected = inventory.holder;
    m_holderAlt.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    return false;
}

bool HandRestock::outstandingStillValid() const
{

    void* const holders[2] = {m_holder.load(std::memory_order_acquire),
                              m_holderAlt.load(std::memory_order_acquire)};
    for (void* const holder : holders) {
        Inventory inventory;
        if (!resolve(holder, inventory) || inventory.container != m_outstanding.container) {
            continue;
        }
        if (m_outstanding.source != inventory.slots + kSlotStride * m_outstanding.sourceSlot) {
            continue;
        }
        return true;
    }
    return false;
}

bool HandRestock::rollbackOutstanding()
{
    if (!outstandingStillValid()) {
        return false;
    }

    if (m_outstanding.spot == kSpotHand) {

        if (m_swapSlots == nullptr) {
            return false;
        }
        const void* faultPc = nullptr;
        const void* faultAddress = nullptr;
        if (!callSwapGuarded(reinterpret_cast<SwapSlotsRaw>(m_swapSlots), m_outstanding.container,
                             m_outstanding.destSlot, m_outstanding.sourceSlot, &faultPc,
                             &faultAddress)) {
            return false;
        }

        notifyRefilled(m_outstanding.container, m_outstanding.destSlot, m_outstanding.sourceSlot);
        return true;
    }

    if (!m_outstanding.hasBefore) {
        return false;
    }
    if (!ItemStackOps::instance().assignFrom(m_outstanding.source, m_before)) {
        return false;
    }
    notifyRefilled(m_outstanding.container, -1, m_outstanding.sourceSlot);
    return true;
}

void HandRestock::notifyRefilled(void* container, int destSlot, int sourceSlot)
{
    if (container == nullptr) {
        return;
    }
    if (destSlot >= 0) {
        ItemStackOps::instance().notifySlotChanged(container, destSlot);
    }
    if (sourceSlot >= 0) {
        ItemStackOps::instance().notifySlotChanged(container, sourceSlot);
    }
}

void HandRestock::clearOutstanding()
{
    if (m_outstanding.hasBefore) {

        ItemStackOps::instance().destroyClone(m_before);
    }
    m_outstanding = Outstanding{};
}

void HandRestock::serveOutstanding()
{
    if (!m_outstanding.active) {
        return;
    }

    const wchar_t* const where = (m_outstanding.spot == kSpotHand) ? L"hand" : L"offhand";

    int result = 0;
    if (!ItemStackRequest::instance().takeResponse(m_outstanding.requestId, result)) {

        if (Clock::now() >= m_outstanding.giveUpAt) {
            log().warn(L"HandRestock: the server did not answer refill request {}, "
                       L"the {} and the server may disagree",
                       m_outstanding.requestId, where);
            clearOutstanding();
        }
        return;
    }

    if (result == ItemStackRequest::kResultSuccess) {
        clearOutstanding();
        return;
    }

    const bool restored = rollbackOutstanding();
    clearOutstanding();

    if (restored) {
        log().warn(L"HandRestock: the server refused to refill the {} (result {}), "
                   L"put the item back",
                   where, result);
        return;
    }
    log().error(L"HandRestock: the server refused to refill the {} (result {}) and the item could "
                L"not be put back, the client may disagree with the server",
                where, result);
}

int HandRestock::applyRefill(Spot spot, int destSlot, int sourceSlot)
{

    if (m_outstanding.active) {
        return 0;
    }

    Inventory inventory;
    if (!resolveClient(inventory)) {
        return 0;
    }
    if (spot == kSpotOffhand && inventory.offhand == nullptr) {

        return 0;
    }

    std::byte* const sourceStack = inventory.slots + kSlotStride * sourceSlot;

    const bool sent =
        (spot == kSpotOffhand)
            ? ItemStackRequest::instance().requestSwap(
                  ItemStackRequest::inventorySlot(inventory.slots, sourceSlot),
                  ItemStackRequest::offhandSlot(inventory.offhand))
            : ItemStackRequest::instance().requestMove(inventory.slots, sourceSlot, destSlot);

    if (!sent) {

        if (!m_warnedNoRequest) {
            m_warnedNoRequest = true;
            log().warn(L"HandRestock: could not send the refill request for the {} "
                       L"(open your inventory once with E so the mod can tell the server), "
                       L"server thinks the screen is {}",
                       (spot == kSpotHand) ? L"hand" : L"offhand",
                       ItemStackRequest::instance().serverInventoryOpen() ? L"open" : L"closed");
        }
        return 0;
    }
    m_warnedNoRequest = false;

    const std::int32_t requestId = ItemStackRequest::instance().lastRequestId();

    const bool stashed = ItemStackOps::instance().cloneTo(m_before, sourceStack);

    if (!predictRefill(inventory, spot, destSlot, sourceSlot)) {
        if (stashed) {
            ItemStackOps::instance().destroyClone(m_before);
        }
        if (!m_warnedNoPredict) {
            m_warnedNoPredict = true;
            log().warn(L"HandRestock: the refill request for the {} went out but the client side "
                       L"could not be updated, it may look stale until you use the item",
                       (spot == kSpotHand) ? L"hand" : L"offhand");
        }

        return 1;
    }
    m_warnedNoPredict = false;

    m_outstanding = Outstanding{};
    m_outstanding.active = true;
    m_outstanding.hasBefore = stashed;
    m_outstanding.requestId = requestId;
    m_outstanding.spot = spot;
    m_outstanding.container = inventory.container;
    m_outstanding.source = sourceStack;
    m_outstanding.sourceSlot = sourceSlot;
    m_outstanding.destSlot = destSlot;
    m_outstanding.giveUpAt = Clock::now() + std::chrono::milliseconds(kResponseWaitMs);

    return 1;
}

}
