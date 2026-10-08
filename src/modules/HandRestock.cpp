#include "modules/HandRestock.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "config/Config.h"
#include "core/Logger.h"
#include "game/GameData.h"
#include "game/BlockRegistry.h"
#include "game/ChatCommand.h"
#include "game/ChatCommandParse.h"
#include "game/ContainerUi.h"
#include "game/UiProbe.h"
#include "game/ItemStackOps.h"
#include "game/ItemStackRequest.h"
#include "input/Foreground.h"
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

bool durabilityRaw(const void* stack, std::int32_t slot, void* damageValue, int& max, int& damage)
{
    __try {
        auto* const bytes = static_cast<const std::byte*>(stack);
        void* const weak = *reinterpret_cast<void* const*>(bytes + 0x08);
        void* const item = weak != nullptr ? *static_cast<void* const*>(weak) : nullptr;
        if (item == nullptr) {
            return false;
        }
        void** const vt = *static_cast<void***>(item);
        using MaxDamageFn = short(__fastcall*)(const void*);
        using DamageValueFn = short(__fastcall*)(const void*);
        max = reinterpret_cast<MaxDamageFn>(vt[slot / 8])(item);
        damage = max > 0 ? reinterpret_cast<DamageValueFn>(damageValue)(stack) : 0;
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
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
    chatcommand::registerModuleCommand({"restock"},
        "/tk restock add|remove|list|clear [item]",
        [this](const std::vector<std::string>& args) { return restockCommand(args); },
        [this](const std::vector<std::string>& words, std::size_t argument, std::string_view suggestion) {
            if (argument != 3 || words.size() < 3) return true;
            const std::string& action = words[2];
            if (action != "remove" && action != "rm" && action != "del") return true;
            const std::string name = chatcommand::normalizeItem(suggestion);
            std::lock_guard lock(m_excludedMutex);
            return m_excluded.contains(name);
        });
    m_swapSlots = Scanner::instance().addressAs<SwapSlotsFn>(Target::SwapSlots);
    if (m_swapSlots == nullptr) {
        log().warn(L"HandRestock: swapSlots was not found, "
                   L"the hand will be refilled on the server but may look stale");
    }

    ItemStackRequest::instance().onScansReady();

    ItemStackOps::instance().onScansReady();

    if (std::byte* const ref = Scanner::instance().address(Target::PlayerVtableRef); ref != nullptr) {
        m_localPlayerVtable.store(memory::ripTarget(ref, kLocalPlayerVtableDisp), std::memory_order_release);
    }
    if (const std::byte* const site = Scanner::instance().address(Target::AnnouncedSlotSite);
        site != nullptr && memory::isReadable(site, 7)) {
        std::int32_t disp = 0;
        std::memcpy(&disp, site + 3, sizeof(disp));
        m_inventoryDisp = (disp > 0x100 && disp < 0x4000 && disp % 8 == 0) ? disp : 0;
    }
    log().info(L"HandRestock: telling your inventory apart by {} (inventory field +{:#x})",
               m_localPlayerVtable.load() != nullptr ? L"the LocalPlayer type" : L"nothing (the newest holder is used)",
               m_inventoryDisp);

    if (const std::byte* const at = Scanner::instance().address(Target::ItemMaxDamageSlotSite);
        at != nullptr && memory::isReadable(at + 6, 4)) {
        std::int32_t slot = 0;
        std::memcpy(&slot, at + 6, sizeof(slot));
        m_maxDamageSlot = slot > 0 && slot < 0x1000 && slot % 8 == 0 ? slot : 0;
    }
    m_damageValue = Scanner::instance().address(Target::ItemStackDamageValue);
    log().info(L"HandRestock: durability {}, swap action {}",
               m_maxDamageSlot != 0 && m_damageValue != nullptr ? L"ready" : L"missing",
               ItemStackRequest::instance().canSwap() ? L"ready" : L"missing");
}

MenuItem HandRestock::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    children.push_back(menu::toggle(
        L"Swap low durability tools", [this] { return m_swapLowDurability.load(std::memory_order_relaxed); },
        [this] {
            m_swapLowDurability.store(!m_swapLowDurability.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }));
    children.push_back(menu::number(
        L"Durability threshold",
        [this] { return static_cast<float>(m_durabilityThreshold.load(std::memory_order_relaxed)); },
        [this](float value) {
            m_durabilityThreshold.store(std::clamp(static_cast<int>(std::lround(value)), kMinDurabilityThreshold,
                                                   kMaxDurabilityThreshold),
                                        std::memory_order_relaxed);
        },
        true, static_cast<float>(kMinDurabilityThreshold), static_cast<float>(kMaxDurabilityThreshold)));

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void HandRestock::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    m_swapLowDurability.store(Config::getBool(section, "swapLowDurability", false), std::memory_order_relaxed);
    m_durabilityThreshold.store(std::clamp(Config::getInt(section, "durabilityThreshold", kDefaultDurabilityThreshold),
                                           kMinDurabilityThreshold, kMaxDurabilityThreshold),
                                std::memory_order_relaxed);
    std::set<std::string> names;
    if (const auto it = section.find("excluded"); it != section.end() && it->is_array()) {
        for (const auto& value : *it) {
            if (value.is_string()) names.insert(chatcommand::normalizeItem(value.get<std::string>()));
        }
    }
    std::lock_guard lock(m_excludedMutex);
    m_excluded = std::move(names);
}

void HandRestock::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    {
        std::lock_guard lock(m_excludedMutex);
        section["excluded"] = m_excluded;
    }
    section["swapLowDurability"] = m_swapLowDurability.load(std::memory_order_relaxed);
    section["durabilityThreshold"] = m_durabilityThreshold.load(std::memory_order_relaxed);
}

std::string HandRestock::mainHandItemName() const
{
    Inventory inventory;
    SlotView view;
    if (!resolveClient(inventory) || !readSlot(inventory.slots, inventory.hand, view)
        || view.item == nullptr || view.count == 0) return {};
    return containerui::itemName(inventory.slots + kSlotStride * inventory.hand);
}

chatcommand::Reply HandRestock::restockCommand(const std::vector<std::string>& args)
{
    const auto parsed = chatcommand::restockAction(args);
    if (parsed == chatcommand::RestockAction::Invalid)
        return {{"Usage: /tk restock add|remove|list|clear [item]"}, true};
    const std::string& action = args[0];
    if (action == "list") {
        std::vector<std::string> names;
        {
            std::lock_guard lock(m_excludedMutex);
            names.assign(m_excluded.begin(), m_excluded.end());
        }
        return {chatcommand::listLines(names)};
    }
    if (action == "clear") {
        std::size_t count;
        {
            std::lock_guard lock(m_excludedMutex);
            count = m_excluded.size();
            m_excluded.clear();
        }
        if (count != 0) uiprobe::markSettingsDirty();
        return {{"Cleared " + std::to_string(count) + " exclusions"}};
    }
    const std::string item = args.size() == 2 ? chatcommand::normalizeItem(args[1])
                                                : chatcommand::normalizeItem(mainHandItemName());
    if (item == "minecraft:") return {{"Nothing in your main hand"}, true};
    if (blocks::itemByName(item) == nullptr) return {{"Unknown item: " + item}, true};
    const bool adding = action == "add";
    bool changed;
    std::size_t count;
    {
        std::lock_guard lock(m_excludedMutex);
        changed = adding ? m_excluded.insert(item).second : m_excluded.erase(item) != 0;
        count = m_excluded.size();
    }
    if (changed) uiprobe::markSettingsDirty();
    if (adding) return {{changed ? "Added " + item + " to the HandRestock exclusions ("
                                 + std::to_string(count) + " items)" : item + " is already excluded"}};
    return {{changed ? "Removed " + item + " from the HandRestock exclusions" : item + " is not excluded"}};
}

void HandRestock::onEnabledChanged(bool )
{
    m_resetRequested.store(true, std::memory_order_release);
}

void HandRestock::onSetSelectedSlot(void* holder)
{
    if (holder == nullptr) {
        return;
    }

    if (holderIsLocal(holder)) {
        if (m_clientHolder.exchange(holder, std::memory_order_acq_rel) != holder) {
            m_faultedHolder.store(nullptr, std::memory_order_release);
        }
        return;
    }
    m_holders.record(holder);
}

void HandRestock::onPlayerViewUpdate()
{
    if (m_resetRequested.exchange(false, std::memory_order_acq_rel)) {
        for (int spot = 0; spot < kSpotCount; ++spot) {
            m_last[spot] = HandState{};
            m_pending[spot] = Pending{};
        }
    }
    if (!enabled()) {
        clearOutstanding();
        for (int spot = 0; spot < kSpotCount; ++spot) {
            m_last[spot] = HandState{};
            m_pending[spot] = Pending{};
        }
        return;
    }

    serveOutstanding();

    if (!input::isInGameplay()) {
        for (int spot = 0; spot < kSpotCount; ++spot) {
            if (m_pending[spot].active) {
                dropPending(static_cast<Spot>(spot),
                            L"not on the game screen (a menu or another screen is open)");
            }
            m_last[spot] = HandState{};
        }
        return;
    }

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

    checkDurability(inventory);
}

bool HandRestock::durabilityOf(const std::byte* stack, int& max, int& damage) const
{
    if (stack == nullptr || m_maxDamageSlot == 0 || m_damageValue == nullptr) {
        return false;
    }
    return durabilityRaw(stack, m_maxDamageSlot, m_damageValue, max, damage);
}

int HandRestock::findDurabilitySource(const Inventory& inventory, void* item, int keepSlot, int threshold) const
{
    int best = -1;
    int bestRemaining = threshold;
    const auto consider = [&](int slot) {
        SlotView view;
        if (slot == keepSlot || !readSlot(inventory.slots, slot, view) || view.item != item || view.count == 0) {
            return;
        }
        int max = 0;
        int damage = 0;
        if (!durabilityOf(inventory.slots + kSlotStride * slot, max, damage) || max <= 0) {
            return;
        }
        if (max - damage > bestRemaining) {
            best = slot;
            bestRemaining = max - damage;
        }
    };
    for (int slot = kHotbarSlots; slot < kSlotCount; ++slot) {
        consider(slot);
    }
    for (int slot = 0; slot < kHotbarSlots; ++slot) {
        consider(slot);
    }
    return best;
}

void HandRestock::checkDurability(const Inventory& inventory)
{
    if (!m_swapLowDurability.load(std::memory_order_relaxed)) {
        return;
    }
    if (m_maxDamageSlot == 0 || m_damageValue == nullptr) {
        if (!m_warnedNoDurability) {
            m_warnedNoDurability = true;
            log().warn(L"HandRestock: the durability functions were not found, low durability tools are not swapped");
        }
        return;
    }
    if (m_outstanding.active) {
        return;
    }

    const int threshold = std::clamp(m_durabilityThreshold.load(std::memory_order_relaxed), kMinDurabilityThreshold,
                                     kMaxDurabilityThreshold);
    const Clock::time_point now = Clock::now();
    for (int i = 0; i < kSpotCount; ++i) {
        const Spot spot = static_cast<Spot>(i);
        if (m_pending[spot].active || now < m_nextDurabilityAt[spot]) {
            continue;
        }
        const std::byte* const stack =
            (spot == kSpotHand) ? (inventory.hand >= 0 && inventory.hand < kHotbarSlots
                                       ? inventory.slots + kSlotStride * inventory.hand
                                       : nullptr)
                                : inventory.offhand;
        SlotView view;
        if (stack == nullptr || !readStackAt(stack, view) || view.item == nullptr || view.count == 0) {
            m_noReplacementItem[spot] = nullptr;
            continue;
        }
        int max = 0;
        int damage = 0;
        if (!durabilityOf(stack, max, damage) || max <= 0) {
            continue;
        }
        const int remaining = max - damage;
        if (remaining > threshold) {
            m_noReplacementItem[spot] = nullptr;
            continue;
        }
        {
            std::lock_guard lock(m_excludedMutex);
            if (!m_last[spot].name.empty() && m_excluded.contains(chatcommand::normalizeItem(m_last[spot].name))) {
                continue;
            }
        }

        const int source = findDurabilitySource(inventory, view.item, inventory.hand, threshold);
        const wchar_t* const where = (spot == kSpotHand) ? L"hand" : L"offhand";
        if (source < 0) {
            if (m_noReplacementItem[spot] != view.item) {
                m_noReplacementItem[spot] = view.item;
                log().info(L"HandRestock: the {} has {} durability left but nothing with more than {} is left "
                           L"to swap in", where, remaining, threshold);
            }
            continue;
        }
        m_noReplacementItem[spot] = nullptr;

        if (!ItemStackRequest::instance().canSwap()) {
            if (!m_warnedNoSwapAction) {
                m_warnedNoSwapAction = true;
                log().warn(L"HandRestock: the swap action was not found, low durability tools are not swapped");
            }
            return;
        }

        int sourceMax = 0;
        int sourceDamage = 0;
        durabilityOf(inventory.slots + kSlotStride * source, sourceMax, sourceDamage);
        const int applied = applyRefill(spot, inventory.hand, source, true);
        if (applied == 0) {
            m_nextDurabilityAt[spot] = now + std::chrono::milliseconds(kRetryMs);
            return;
        }
        m_nextDurabilityAt[spot] = now + std::chrono::milliseconds(kDurabilityCooldownMs);
        static int logs1 = 0;
        if (logs1 < 200) {
            ++logs1;
            log().success(L"HandRestock: the {} had {} durability left, swapped it with slot {} ({} left)", where,
                          remaining, source, sourceMax - sourceDamage);
        }
        return;
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
                             view.aux, view.count,
                             total};
    if (view.item != nullptr && view.count > 0) {
        m_last[spot].name = previous.item == view.item ? previous.name
            : containerui::itemName(spot == kSpotHand ? inventory.slots + kSlotStride * slot
                                               : inventory.offhand);
    }

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

    {
        std::lock_guard lock(m_excludedMutex);
        if (!previous.name.empty() && m_excluded.contains(chatcommand::normalizeItem(previous.name))) {
            dropPending(spot, L"the item is excluded");
            return;
        }
    }

    if (Clock::now() < m_ignoreUntil[spot]) {
        dropPending(spot, L"it was moved on purpose, not used up");
        return;
    }

    const Clock::time_point now = Clock::now();
    m_pending[spot] = Pending{};
    m_pending[spot].active = true;
    m_pending[spot].destSlot = slot;
    m_pending[spot].container = inventory.container;
    m_pending[spot].item = previous.item;
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
    if (m_pending[spot].waitContent) {
        const auto behind = static_cast<std::int32_t>(ItemStackRequest::instance().inventoryContentSerial()
                                                       - m_pending[spot].contentSerial);
        if (behind < 0 && now < m_pending[spot].contentDeadline) {
            return;
        }
        m_pending[spot].waitContent = false;
        m_pending[spot].at = now + std::chrono::milliseconds(kContentSettleMs);
        return;
    }

    if (m_outstanding.active) {
        m_pending[spot].giveUpAt = std::max(m_pending[spot].giveUpAt, now + std::chrono::milliseconds(kGiveUpMs));
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

    if (pending.container != nullptr && inventory.container != pending.container) {
        dropPending(spot, L"your inventory is not the one it was found in (another world?)");
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
    if (!pending.retry && totalNow >= pending.totalBefore) {
        log().info(L"HandRestock: the {} went empty but the total did not drop ({} -> {}), "
                   L"treating it as a move, not a use",
                   (spot == kSpotHand) ? L"hand" : L"offhand", pending.totalBefore, totalNow);
        m_pending[spot] = Pending{};
        return;
    }

    SlotView wanted;
    wanted.item = pending.item;
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
    if (m_outstanding.active) {
        m_outstanding.retryAs = pending;
        m_outstanding.retryAs.active = true;
    }

    SlotView refilled;
    const bool reread = (spot == kSpotHand)
                            ? readSlot(inventory.slots, pending.destSlot, refilled)
                            : readStackAt(inventory.offhand, refilled);
    if (reread) {
        m_last[spot] = HandState{inventory.container, pending.destSlot, refilled.item,
                                 refilled.aux, refilled.count};
    }

    const wchar_t* const where = (spot == kSpotHand) ? L"hand" : L"offhand";

    if (refilled.count == 0) {
        log().warn(L"HandRestock: refilled the {} from slot {} but it still reads empty", where,
                   source);
        return;
    }

    static int logs2 = 0;
    if (logs2 < 200) {
        ++logs2;
        log().success(L"HandRestock: the {} ran out, refilled from slot {} (x{})", where, source,
                      sourceView.count);
    }
}

bool HandRestock::emptyEverywhere(Spot spot, int destSlot) const
{
    Own own;
    if (!resolveOwn(own, true)) {
        return false;
    }
    const Inventory* const inventories[2] = {&own.client, own.haveServer ? &own.server : nullptr};
    int seen = 0;
    for (const Inventory* const one : inventories) {
        if (one == nullptr) {
            continue;
        }
        const Inventory& inventory = *one;
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

bool HandRestock::forEachInventorySlot(const std::function<void(const void*, int)>& fn) const
{
    if (!fn) {
        return false;
    }
    Inventory inventory;
    if (!resolveClient(inventory) || inventory.slots == nullptr) {
        return false;
    }
    for (int slot = 0; slot < kSlotCount; ++slot) {
        SlotView view;
        if (!readSlot(inventory.slots, slot, view)) {
            continue;
        }
        if (view.item == nullptr || view.count == 0) {
            continue;
        }
        fn(view.block, static_cast<int>(view.count));
    }
    if (inventory.offhand != nullptr) {
        SlotView view;
        if (readStackAt(inventory.offhand, view) && view.item != nullptr && view.count > 0) {
            fn(view.block, static_cast<int>(view.count));
        }
    }
    return true;
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

bool HandRestock::looksLikeInventory(std::byte* slots, bool* faulted) const
{
    if (faulted != nullptr) {
        *faulted = false;
    }
    if (!memory::plausiblePointer(slots)) {
        if (faulted != nullptr && slots != nullptr) {
            *faulted = true;
        }
        return false;
    }

    void* first = nullptr;
    if (!readPointerGuarded(slots, first)) {
        if (faulted != nullptr) {
            *faulted = true;
        }
        return false;
    }
    if (!mainModule().contains(first)) {
        return false;
    }

    for (int slot = 1; slot < kSlotCount; ++slot) {
        void* value = nullptr;
        if (!readPointerGuarded(slots + kSlotStride * slot, value)) {
            if (faulted != nullptr) {
                *faulted = true;
            }
            return false;
        }
        if (value != first) {
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
        || !readIntGuarded(stack + kNetValueOffset, view.netValue)
        || !readU8Guarded(stack + kNetTagOffset, view.netTag)) {
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

bool HandRestock::alreadyChecked(void* holder, void* container, std::byte* slots) const
{
    for (const Checked& one : m_checked) {
        if (one.holder == holder && one.container == container && one.slots == slots) {
            return true;
        }
    }
    return false;
}

void HandRestock::rememberChecked(void* holder, void* container, std::byte* slots) const
{
    for (Checked& one : m_checked) {
        if (one.holder == holder) {
            one = Checked{holder, container, slots};
            return;
        }
    }
    m_checked[m_checkedNext] = Checked{holder, container, slots};
    m_checkedNext = (m_checkedNext + 1) % std::size(m_checked);
}

bool HandRestock::resolve(void* holder, Inventory& out, bool* faulted) const
{
    if (faulted != nullptr) {
        *faulted = false;
    }
    if (holder == nullptr) {
        return false;
    }
    const auto fault = [faulted] {
        if (faulted != nullptr) {
            *faulted = true;
        }
        return false;
    };

    auto* const base = static_cast<std::byte*>(holder);

    int hand = -1;
    if (!readIntGuarded(base + kSelectedSlotOffset, hand)) {
        return fault();
    }
    if (hand < 0 || hand >= kHotbarSlots) {
        return false;
    }

    void* container = nullptr;
    if (!readPointerGuarded(base + kContainerOffset, container)) {
        return fault();
    }
    if (container == nullptr) {
        return false;
    }
    if (!memory::plausiblePointer(container)) {
        return fault();
    }

    void* slots = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(container) + kSlotsOffset, slots)) {
        return fault();
    }
    if (slots == nullptr) {
        return false;
    }
    if (!memory::plausiblePointer(slots)) {
        return fault();
    }

    auto* const array = static_cast<std::byte*>(slots);

    if (!alreadyChecked(holder, container, array)) {
        bool unreadable = false;
        if (!looksLikeInventory(array, &unreadable)) {
            return unreadable ? fault() : false;
        }
        rememberChecked(holder, container, array);
    }

    out = Inventory{};
    out.holder = holder;
    out.container = container;
    out.slots = array;
    out.hand = hand;

    void* player = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(container) + kPlayerOffset, player)
        || !memory::plausiblePointer(player)) {
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
    void* const local = localPlayerVtable();
    return local != nullptr && vtable == local;
}

void* HandRestock::localPlayerVtable() const
{
    return m_localPlayerVtable.load(std::memory_order_acquire);
}

bool HandRestock::holderIsLocal(void* holder) const
{
    void* const local = localPlayerVtable();
    if (holder == nullptr || local == nullptr) {
        return false;
    }
    void* container = nullptr;
    void* player = nullptr;
    void* vtable = nullptr;
    return memory::plausiblePointer(holder)
           && readPointerGuarded(static_cast<std::byte*>(holder) + kContainerOffset, container)
           && memory::plausiblePointer(container)
           && readPointerGuarded(static_cast<std::byte*>(container) + kPlayerOffset, player)
           && memory::plausiblePointer(player) && readPointerGuarded(player, vtable) && vtable == local;
}

void* HandRestock::holderOfPlayer(void* player) const
{
    if (player == nullptr || m_inventoryDisp == 0) {
        return nullptr;
    }
    if (playerFaulted(player)) {
        return nullptr;
    }
    void* holder = nullptr;
    if (!memory::plausiblePointer(player)) {
        return nullptr;
    }
    if (!readPointerGuarded(static_cast<std::byte*>(player) + m_inventoryDisp, holder)) {
        notePlayerFaulted(player);
        return nullptr;
    }
    if (!memory::plausiblePointer(holder)) {
        return nullptr;
    }
    return holder;
}

bool HandRestock::playerFaulted(void* player) const
{
    const unsigned long long serial = GameData::instance().playerSerial();
    const unsigned long long now = GetTickCount64();
    for (const FaultedPlayer& one : m_faultedPlayers) {
        if (one.player.load(std::memory_order_acquire) == player && one.serial.load(std::memory_order_acquire) == serial
            && now - one.at.load(std::memory_order_acquire) < kFaultedPlayerForgetMs) {
            return true;
        }
    }
    return false;
}

void HandRestock::notePlayerFaulted(void* player) const
{
    if (player == nullptr) {
        return;
    }
    const unsigned long long serial = GameData::instance().playerSerial();
    FaultedPlayer& one = m_faultedPlayers[m_faultedPlayerNext.fetch_add(1, std::memory_order_relaxed) % 2];
    one.serial.store(serial, std::memory_order_release);
    one.at.store(GetTickCount64(), std::memory_order_release);
    one.player.store(player, std::memory_order_release);
}

bool HandRestock::resolveOwn(Own& out, bool wantServer) const
{
    out = Own{};

    if (localPlayerVtable() == nullptr) {
        void* holders[HolderTable::kCapacity] = {};
        const std::size_t count = m_holders.snapshot(holders, HolderTable::kCapacity);
        for (std::size_t i = 0; i < count; ++i) {
            bool faulted = false;
            if (resolve(holders[i], out.client, &faulted)) {
                out.haveClient = true;
                break;
            }
            if (faulted) {
                m_holders.forget(holders[i]);
            }
        }
        return out.haveClient;
    }

    const auto tryClient = [this, &out](void* holder, void* expectedPlayer, bool* faultedOut) {
        if (holder == nullptr || holder == m_faultedHolder.load(std::memory_order_acquire)) {
            return false;
        }
        Inventory inventory;
        bool faulted = false;
        if (!resolve(holder, inventory, &faulted)) {
            if (faultedOut != nullptr) {
                *faultedOut = faulted;
            }
            return false;
        }
        const bool client = isClientSidePlayer(inventory.playerRaw);
        if (!client || (expectedPlayer != nullptr && inventory.playerRaw != expectedPlayer)) {
            return false;
        }
        out.client = inventory;
        out.haveClient = true;
        return true;
    };
    if (void* const gamePlayer = GameData::instance().player(); gamePlayer != nullptr && gamePlayer != m_lastGamePlayer) {
        void* const holder = holderOfPlayer(gamePlayer);
        bool faulted = false;
        if (tryClient(holder, gamePlayer, &faulted)) {
            m_lastGamePlayer = gamePlayer;
            if (m_clientHolder.exchange(holder, std::memory_order_acq_rel) != holder) {
                m_faultedHolder.store(nullptr, std::memory_order_release);
            }
            m_holders.forget(holder);
            out = Own{};
        } else if (faulted) {
            notePlayerFaulted(gamePlayer);
        }
    }
    void* const seat = m_clientHolder.load(std::memory_order_acquire);
    bool seatFaulted = false;
    if (!tryClient(seat, nullptr, &seatFaulted)) {
        void* const player = GameData::instance().player();
        void* found = nullptr;
        bool gameFaulted = false;
        if (void* const holder = holderOfPlayer(player); tryClient(holder, player, &gameFaulted)) {
            found = holder;
        } else {
            if (gameFaulted) {
                notePlayerFaulted(player);
            }
            void* holders[HolderTable::kCapacity] = {};
            const std::size_t count = m_holders.snapshot(holders, HolderTable::kCapacity);
            for (std::size_t i = 0; i < count && found == nullptr; ++i) {
                bool faulted = false;
                if (tryClient(holders[i], nullptr, &faulted)) {
                    found = holders[i];
                } else if (faulted) {
                    m_holders.forget(holders[i]);
                }
            }
        }
        if (found != nullptr || seatFaulted) {
            void* expected = seat;
            m_clientHolder.compare_exchange_strong(expected, found, std::memory_order_acq_rel);
        }
        if (found != nullptr) {
            m_holders.forget(found);
        }
    }
    if (!out.haveClient) {
        return false;
    }
    if (!wantServer) {
        return true;
    }

    std::int32_t mine[kSlotCount] = {};
    for (int slot = 0; slot < kSlotCount; ++slot) {
        SlotView view;
        if (readSlot(out.client.slots, slot, view) && view.item != nullptr && view.count > 0 && view.netTag == 0) {
            mine[slot] = view.netValue;
        }
    }
    const auto tryServer = [this, &out, &mine](void* holder) {
        if (holder == nullptr || holder == out.client.holder) {
            return false;
        }
        Inventory inventory;
        bool faulted = false;
        if (!resolve(holder, inventory, &faulted)) {
            if (faulted) {
                m_holders.forget(holder);
            }
            return false;
        }
        if (inventory.container == out.client.container || isClientSidePlayer(inventory.playerRaw)) {
            return false;
        }
        const GameData& game = GameData::instance();
        if (!game.knowsServerPlayer() || !game.isServerPlayer(inventory.playerRaw)) {
            return false;
        }
        if (m_inventoryDisp != 0 && holderOfPlayer(inventory.playerRaw) != holder) {
            return false;
        }
        std::int32_t theirs[kSlotCount] = {};
        for (int slot = 0; slot < kSlotCount; ++slot) {
            SlotView view;
            if (readSlot(inventory.slots, slot, view) && view.item != nullptr && view.count > 0 && view.netTag == 0) {
                theirs[slot] = view.netValue;
            }
        }
        if (!compareNetIds(mine, theirs, kSlotCount).sameOwner()) {
            return false;
        }
        out.server = inventory;
        out.haveServer = true;
        return true;
    };
    if (!tryServer(holderOfPlayer(GameData::instance().playerAlt()))) {
        void* holders[HolderTable::kCapacity] = {};
        const std::size_t count = m_holders.snapshot(holders, HolderTable::kCapacity);
        for (std::size_t i = 0; i < count; ++i) {
            if (tryServer(holders[i])) {
                break;
            }
        }
        if (count > 0 && !m_loggedForeign) {
            m_loggedForeign = true;
            log().info(L"HandRestock: {} other slot holder(s) seen (other players, or the server-side copy in a local "
                       L"world); only your own inventory is used",
                       count);
        }
    }
    if (m_loggedServerCopy != (out.haveServer ? 1 : 0) && m_serverCopyLogs < 6) {
        m_loggedServerCopy = out.haveServer ? 1 : 0;
        ++m_serverCopyLogs;
        log().info(L"HandRestock: {}",
                   out.haveServer ? L"found the server-side copy of your inventory (local world)"
                                  : L"no server-side copy of your inventory (a remote server, or it could not be matched)");
    }
    return true;
}

bool HandRestock::resolveClient(Inventory& out) const
{
    Own own;
    if (!resolveOwn(own, false)) {
        return false;
    }
    out = own.client;
    return true;
}

void HandRestock::ownHolders(void*& client, void*& server) const
{
    client = nullptr;
    server = nullptr;
    Own own;
    if (!resolveOwn(own, true)) {
        return;
    }
    client = own.client.holder;
    server = own.haveServer ? own.server.holder : nullptr;
}

void* HandRestock::ownClientHolder() const
{
    Own own;
    return resolveOwn(own, false) ? own.client.holder : nullptr;
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

    m_holders.forget(inventory.holder);
    m_faultedHolder.store(inventory.holder, std::memory_order_release);
    void* expected = inventory.holder;
    m_clientHolder.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    return false;
}

bool HandRestock::outstandingStillValid() const
{
    Inventory inventory;
    if (!resolveClient(inventory) || inventory.container != m_outstanding.container) {
        return false;
    }
    return m_outstanding.source == inventory.slots + kSlotStride * m_outstanding.sourceSlot;
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
        if (!outstandingStillValid()) {
            log().info(L"HandRestock: the world changed while waiting for refill request {}; stopped waiting",
                       m_outstanding.requestId);
            clearOutstanding();
            return;
        }
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

    const Spot spot = m_outstanding.spot;
    const int destSlot = m_outstanding.destSlot;
    void* const container = m_outstanding.container;
    Pending retryAs = m_outstanding.retryAs;
    const bool durabilitySwap = m_outstanding.durabilitySwap;
    const std::uint32_t contentAtSend = m_outstanding.contentSerialAtSend;
    bool intact = false;
    {
        Inventory now;
        SlotView dest;
        SlotView source;
        if (resolveClient(now) && now.container == container
            && ((spot == kSpotHand) ? readSlot(now.slots, destSlot, dest)
                                    : (now.offhand != nullptr && readStackAt(now.offhand, dest)))
            && readSlot(now.slots, m_outstanding.sourceSlot, source)) {
            if (!m_outstanding.durabilitySwap) {
                intact = dest.item == m_outstanding.predItem && dest.count == m_outstanding.predCount
                         && (source.item == nullptr || source.count == 0);
            } else {
                const bool sourceIntact = source.item == m_outstanding.predSourceItem
                                          && source.count == m_outstanding.predSourceCount
                                          && source.netValue == m_outstanding.predSourceNet;
                const bool destIntact = dest.item == m_outstanding.predItem && dest.count == m_outstanding.predCount
                                        && dest.netValue == m_outstanding.predDestNet;
                intact = sourceIntact && (spot == kSpotOffhand || destIntact);
            }
        }
    }
    const bool resent = !intact;
    const bool restored = intact ? rollbackOutstanding() : true;
    clearOutstanding();
    if (resent) {
        log().info(L"HandRestock: the {} no longer shows the refill (the server resent the inventory or it was "
                   L"moved), so nothing is put back", where);
    }

    if (restored) {
        if (!resent) {
            log().warn(L"HandRestock: the server refused to refill the {} (result {}), "
                       L"put the item back",
                       where, result);
        } else {
            log().warn(L"HandRestock: the server refused to refill the {} (result {})", where, result);
        }
        if (durabilitySwap) {
            m_nextDurabilityAt[spot] = Clock::now() + std::chrono::milliseconds(kDurabilityRefusedMs);
            return;
        }
        const bool behind = result == kResultFailedToValidateSrcSlot || result == kResultFailedToValidateDstSlot;
        if (behind && retryAs.active && retryAs.attempts < kMaxRefusedRetries) {
            const Clock::time_point now = Clock::now();
            ++retryAs.attempts;
            retryAs.retry = true;
            retryAs.at = now + std::chrono::milliseconds(kRefusedRetryMs * retryAs.attempts);
            retryAs.waitContent = true;
            retryAs.contentSerial = contentAtSend + 1;
            retryAs.contentDeadline = now + std::chrono::milliseconds(kContentWaitMs);
            retryAs.giveUpAt = retryAs.contentDeadline + std::chrono::milliseconds(kGiveUpMs);
            const bool sameNeed = m_pending[spot].active && m_pending[spot].destSlot == retryAs.destSlot
                                  && m_pending[spot].item == retryAs.item && m_pending[spot].aux == retryAs.aux;
            if (sameNeed) {
                log().info(L"HandRestock: a newer refill of the {} for the same item is replaced by the retry", where);
            } else if (m_pending[spot].active) {
                log().info(L"HandRestock: a newer refill of the {} is already waiting, so the refused one is not "
                           L"retried", where);
                return;
            }
            m_pending[spot] = retryAs;
            const int spotSlot = (spot == kSpotHand) ? destSlot : -1;
            if (m_last[spot].container == container && m_last[spot].slot == spotSlot) {
                HandState empty;
                empty.container = container;
                empty.slot = spotSlot;
                m_last[spot] = empty;
            }
            log().info(L"HandRestock: the server looks behind, refilling the {} again {} "
                       L"(attempt {} of {})",
                       where,
                       retryAs.waitContent ? L"after it resends the inventory" : L"shortly",
                       retryAs.attempts, kMaxRefusedRetries);
        } else if (behind) {
            log().warn(L"HandRestock: gave up refilling the {} after {} refused attempt(s)", where,
                       retryAs.attempts + 1);
        }
        return;
    }
    log().error(L"HandRestock: the server refused to refill the {} (result {}) and the item could "
                L"not be put back, the client may disagree with the server",
                where, result);
    if (durabilitySwap) {
        m_nextDurabilityAt[spot] = Clock::now() + std::chrono::milliseconds(kDurabilityRefusedMs);
    }
}

int HandRestock::applyRefill(Spot spot, int destSlot, int sourceSlot, bool durabilitySwap)
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
            : durabilitySwap
                  ? ItemStackRequest::instance().requestSwap(
                        ItemStackRequest::inventorySlot(inventory.slots, sourceSlot),
                        ItemStackRequest::inventorySlot(inventory.slots, destSlot))
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
    m_outstanding.contentSerialAtSend = ItemStackRequest::instance().inventoryContentSerial();
    {
        SlotView predicted;
        const bool read = (spot == kSpotHand) ? readSlot(inventory.slots, destSlot, predicted)
                                              : (inventory.offhand != nullptr && readStackAt(inventory.offhand, predicted));
        m_outstanding.predItem = read ? predicted.item : nullptr;
        m_outstanding.predCount = read ? predicted.count : 0;
        m_outstanding.predDestNet = read ? predicted.netValue : 0;
        SlotView source;
        const bool readSource = readSlot(inventory.slots, sourceSlot, source);
        m_outstanding.durabilitySwap = durabilitySwap;
        m_outstanding.predSourceItem = readSource ? source.item : nullptr;
        m_outstanding.predSourceCount = readSource ? source.count : 0;
        m_outstanding.predSourceNet = readSource ? source.netValue : 0;
    }

    return 1;
}

}
