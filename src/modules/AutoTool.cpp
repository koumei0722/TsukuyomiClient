#include "modules/AutoTool.h"
#include "input/GameButtons.h"

#include "config/Config.h"
#include "config/WriteSwitches.h"

#include "core/Logger.h"
#include "game/GameData.h"
#include "game/HolderTable.h"
#include "hooks/Detours.h"
#include "input/Foreground.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "modules/HandRestock.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <vector>

namespace tsukuyomi {

namespace {

int accessViolationFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
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

bool writeIntGuarded(void* address, int value)
{
    __try {
        *static_cast<int*>(address) = value;
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool readByteGuarded(const void* address, unsigned char& value)
{
    __try {
        value = *static_cast<const unsigned char*>(address);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool probeSlotSpeeds(void** itemSlot, std::byte* slotZero, void* savedItem, ptrdiff_t stride,
                     int slotCount, void* rcx, void* rdx, void* r8, void* r9, float* speeds)
{
    __try {
        for (int slot = 0; slot < slotCount; ++slot) {
            *itemSlot = slotZero + stride * slot;
            speeds[slot] = hooks::callGetDestroySpeed(rcx, rdx, r8, r9);
        }
        *itemSlot = savedItem;
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        *itemSlot = savedItem;
        return false;
    }
}

struct DamageParams {
    float base = 0.0f;
    bool attribute = false;
    bool item = true;
    bool enchant = true;
    bool effects = false;
};
static_assert(sizeof(DamageParams) == 8);

using DamageCalcFn = float(__fastcall*)(void* attacker, void* target, const DamageParams* params);

bool probeSlotDamages(void* calc, int* selected, int current, void* attacker, void* target, int slotCount,
                      float* damages)
{
    const DamageParams params;
    __try {
        for (int slot = 0; slot < slotCount; ++slot) {
            *selected = slot;
            damages[slot] = reinterpret_cast<DamageCalcFn>(calc)(attacker, target, &params);
        }
        *selected = current;
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        *selected = current;
        return false;
    }
}

bool destroyTransactionGuarded(void* tx)
{
    __try {
        void** const vt = *static_cast<void***>(tx);
        using DeleteFn = void(__fastcall*)(void*, int);
        reinterpret_cast<DeleteFn>(vt[0])(tx, 1);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
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

bool readSlotValue(void* holder, ptrdiff_t offset, int slotCount, int& slot)
{
    if (holder == nullptr) {
        return false;
    }
    const void* const field = static_cast<const std::byte*>(holder) + offset;
    int value = -1;
    if (!readIntGuarded(field, value)) {
        return false;
    }
    if (value < 0 || value >= slotCount) {
        return false;
    }
    slot = value;
    return true;
}

}

bool AutoTool::looksLikeSlotArray(const std::byte* slotZero, ptrdiff_t stride, int slotCount, bool* faulted)
{
    if (faulted != nullptr) {
        *faulted = false;
    }
    if (!memory::plausiblePointer(slotZero)) {
        if (faulted != nullptr && slotZero != nullptr) {
            *faulted = true;
        }
        return false;
    }

    void* first = nullptr;
    if (!readPointerGuarded(slotZero, first)) {
        if (faulted != nullptr) {
            *faulted = true;
        }
        return false;
    }
    if (!mainModule().contains(first)) {
        return false;
    }
    for (int slot = 1; slot < slotCount; ++slot) {
        void* value = nullptr;
        if (!readPointerGuarded(slotZero + stride * slot, value)) {
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

void AutoTool::trackHolder(void* holder)
{
    if (holder == nullptr) {
        return;
    }
    const std::lock_guard<std::mutex> lock(m_holdersMutex);
    const unsigned long long seq = ++m_holderSeq;
    for (Tracked& one : m_holders) {
        if (one.holder == holder) {
            one.seenAt = seq;
            return;
        }
    }
    Tracked* victim = &m_holders[0];
    for (Tracked& one : m_holders) {
        if (one.holder == nullptr) {
            victim = &one;
            break;
        }
        if (one.seenAt < victim->seenAt) {
            victim = &one;
        }
    }
    victim->holder = holder;
    victim->seenAt = seq;
}

void AutoTool::forgetHolder(void* holder)
{
    if (holder == nullptr) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(m_holdersMutex);
        for (Tracked& one : m_holders) {
            if (one.holder == holder) {
                one.holder = nullptr;
                one.seenAt = 0;
            }
        }
    }
    void* expected = holder;
    m_owner.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

std::size_t AutoTool::snapshotHolders(void** out, std::size_t capacity)
{
    Tracked copy[kHolderSlots];
    {
        const std::lock_guard<std::mutex> lock(m_holdersMutex);
        std::copy(std::begin(m_holders), std::end(m_holders), std::begin(copy));
    }
    std::sort(std::begin(copy), std::end(copy),
              [](const Tracked& a, const Tracked& b) { return a.seenAt > b.seenAt; });
    std::size_t count = 0;
    for (const Tracked& one : copy) {
        if (one.holder != nullptr && count < capacity) {
            out[count++] = one.holder;
        }
    }
    return count;
}

void* AutoTool::playerOfHolder(void* holder)
{
    void* container = nullptr;
    void* player = nullptr;
    if (!memory::plausiblePointer(holder)
        || !readPointerGuarded(static_cast<std::byte*>(holder) + kContainerOffset, container)
        || !memory::plausiblePointer(container)
        || !readPointerGuarded(static_cast<std::byte*>(container) + kContainerPlayerOffset, player)) {
        return nullptr;
    }
    return player;
}

bool AutoTool::canTellLive() const
{
    const std::ptrdiff_t inventory = m_attack.inventory;
    return m_attack.localPlayerVtable != nullptr && GameData::instance().knowsServerPlayer() && inventory > 0x100
           && inventory < 0x4000 && inventory % 8 == 0;
}

std::byte* AutoTool::contextPlayer(void* context, int* side) const
{
    if (side != nullptr) {
        *side = 0;
    }
    void* field = nullptr;
    if (!canTellLive() || !memory::plausiblePointer(context) || !readPointerGuarded(context, field)
        || !memory::plausiblePointer(field)) {
        return nullptr;
    }
    auto* const player = static_cast<std::byte*>(field) - kContextPlayerOffset;
    void* vtable = nullptr;
    if (!readPointerGuarded(player, vtable)) {
        return nullptr;
    }
    const int kind = vtable == m_attack.localPlayerVtable ? 1 : (GameData::instance().isServerPlayer(player) ? 2 : 0);
    if (kind == 0) {
        return nullptr;
    }
    if (side != nullptr) {
        *side = kind;
    }
    return player;
}

bool AutoTool::speedQueryIsForeign(void* context) const
{
    int side = 0;
    std::byte* const player = contextPlayer(context, &side);
    if (player == nullptr || side != 2) {
        return false;
    }
    void* holder = nullptr;
    void* const own = m_ownClient.load(std::memory_order_acquire);
    std::byte* mine = nullptr;
    std::byte* theirs = nullptr;
    if (own == nullptr || !readPointerGuarded(player + m_attack.inventory, holder)
        || !resolveSlots(own, mine) || !resolveSlots(holder, theirs)) {
        return true;
    }
    std::int32_t a[kInventorySlots] = {};
    std::int32_t b[kInventorySlots] = {};
    if (!readServerNetIds(mine, a) || !readServerNetIds(theirs, b)) {
        return true;
    }
    return !compareNetIds(a, b, kInventorySlots).sameOwner();
}

bool AutoTool::readServerNetIds(const std::byte* slots, std::int32_t* out)
{
    for (int slot = 0; slot < kInventorySlots; ++slot) {
        const std::byte* const stack = slots + kSlotStride * slot;
        void* item = nullptr;
        unsigned char count = 0;
        unsigned char tag = 0xFF;
        int value = 0;
        if (!readPointerGuarded(stack + kItemOffset, item) || !readByteGuarded(stack + kCountOffset, count)
            || !readByteGuarded(stack + kNetTagOffset, tag) || !readIntGuarded(stack + kNetValueOffset, value)) {
            return false;
        }
        out[slot] = (item != nullptr && count > 0 && tag == 0) ? value : 0;
    }
    return true;
}

bool AutoTool::holderIsLive(void* holder, int* side) const
{
    if (side != nullptr) {
        *side = 0;
    }
    if (!canTellLive()) {
        return true;
    }
    const void* const localVtable = m_attack.localPlayerVtable;
    const std::ptrdiff_t inventory = m_attack.inventory;
    const GameData& game = GameData::instance();
    void* const player = playerOfHolder(holder);
    void* vtable = nullptr;
    void* back = nullptr;
    if (!memory::plausiblePointer(player) || !readPointerGuarded(player, vtable)) {
        return false;
    }
    const int kind = vtable == localVtable ? 1 : (game.isServerPlayer(player) ? 2 : 0);
    if (kind == 0 || !readPointerGuarded(static_cast<std::byte*>(player) + inventory, back) || back != holder) {
        return false;
    }
    if (side != nullptr) {
        *side = kind;
    }
    return true;
}

bool AutoTool::resolveSlots(void* holder, std::byte*& slots, bool* faulted)
{
    bool dummy = false;
    bool& bad = faulted != nullptr ? *faulted : dummy;
    bad = false;
    if (holder == nullptr) {
        return false;
    }
    void* container = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(holder) + kContainerOffset, container)) {
        bad = true;
        return false;
    }
    if (container == nullptr) {
        return false;
    }
    if (!memory::plausiblePointer(container)) {
        bad = true;
        return false;
    }
    void* head = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(container) + kSlotsOffset, head)) {
        bad = true;
        return false;
    }
    if (head == nullptr) {
        return false;
    }
    auto* const array = static_cast<std::byte*>(head);
    if (!looksLikeSlotArray(array, kSlotStride, kSlotCount, &bad)) {
        return false;
    }
    slots = array;
    return true;
}

bool AutoTool::findOwner(void* item, Owner& out, const void* queryPlayer)
{
    if (item == nullptr) {
        return false;
    }
    void* candidates[kHolderSlots + 2] = {};
    std::size_t count = 0;
    const auto add = [&candidates, &count](void* holder) {
        if (holder == nullptr) {
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (candidates[i] == holder) {
                return;
            }
        }
        candidates[count++] = holder;
    };
    add(m_ownClient.load(std::memory_order_acquire));
    void* const cached = m_owner.load(std::memory_order_acquire);
    add(cached);
    void* tracked[kHolderSlots] = {};
    const std::size_t seen = snapshotHolders(tracked, kHolderSlots);
    for (std::size_t i = 0; i < seen; ++i) {
        add(tracked[i]);
    }

    const auto* const itemAt = static_cast<const std::byte*>(item);
    bool anyResolved = false;
    bool faultedAt[kHolderSlots + 2] = {};
    for (std::size_t i = 0; i < count; ++i) {
        std::byte* slots = nullptr;
        if (!resolveSlots(candidates[i], slots, &faultedAt[i])) {
            if (faultedAt[i]) {
                forgetHolder(candidates[i]);
            }
            continue;
        }
        anyResolved = true;
        const ptrdiff_t offset = itemAt - slots;
        if (offset < 0 || offset >= kSlotStride * kSlotCount || offset % kSlotStride != 0) {
            continue;
        }
        if (!holderIsLive(candidates[i])) {
            continue;
        }
        out.holder = candidates[i];
        out.slots = slots;
        out.slot = static_cast<int>(offset / kSlotStride);
        if (candidates[i] != cached) {
            m_owner.store(candidates[i], std::memory_order_release);
            noteOwnerOnce(candidates[i], seen);
        }
        return true;
    }
    if (anyResolved) {
        return false;
    }

    for (std::size_t i = 0; i < count; ++i) {
        if (faultedAt[i]) {
            continue;
        }
        if (canTellLive() && (queryPlayer == nullptr || playerOfHolder(candidates[i]) != queryPlayer)) {
            continue;
        }
        int slot = -1;
        if (!readSlotValue(candidates[i], kSelectedSlotOffset, kSlotCount, slot)) {
            continue;
        }
        auto* const head = const_cast<std::byte*>(itemAt) - kSlotStride * slot;
        if (!memory::isReadable(head - kSlotStride,
                                static_cast<size_t>(kSlotStride) * (kSlotCount + 1))
            || !looksLikeSlotArray(head, kSlotStride, kSlotCount)) {
            continue;
        }
        void* before = nullptr;
        void* first = nullptr;
        if (!readPointerGuarded(head - kSlotStride, before) || !readPointerGuarded(head, first)
            || before == first) {
            continue;
        }
        out.holder = candidates[i];
        out.slots = head;
        out.slot = slot;
        if (!m_fallbackLogged.exchange(true, std::memory_order_relaxed)) {
            log().warn(L"AutoTool: could not follow the inventory of any slot holder; using the "
                       L"selected slot number instead (another player's hotbar can be mistaken "
                       L"for yours on a server)");
        }
        return true;
    }
    return false;
}

void AutoTool::noteOwnerOnce(void* holder, std::size_t seen)
{
    for (void* const one : m_ownerLogged) {
        if (one == holder) {
            return;
        }
    }
    const int at = m_ownerLogs.fetch_add(1, std::memory_order_relaxed);
    if (at >= static_cast<int>(std::size(m_ownerLogged))) {
        return;
    }
    m_ownerLogged[at] = holder;
    log().info(L"AutoTool: the hotbar in use belongs to slot holder {:#x} ({} holder(s) seen)",
               reinterpret_cast<uintptr_t>(holder), seen);
}

bool AutoTool::sameHotbar(const std::byte* a, const std::byte* b)
{
    if (a == nullptr || b == nullptr || a == b) {
        return false;
    }
    bool anyItem = false;
    for (int slot = 0; slot < kSlotCount; ++slot) {
        void* itemA = nullptr;
        void* itemB = nullptr;
        unsigned char countA = 0;
        unsigned char countB = 0;
        const ptrdiff_t at = kSlotStride * slot;
        if (!readPointerGuarded(a + at + kItemOffset, itemA)
            || !readPointerGuarded(b + at + kItemOffset, itemB)
            || !readByteGuarded(a + at + kCountOffset, countA)
            || !readByteGuarded(b + at + kCountOffset, countB)) {
            return false;
        }
        if (itemA != itemB || countA != countB) {
            return false;
        }
        anyItem = anyItem || (itemA != nullptr && countA != 0);
    }
    return anyItem;
}

void* AutoTool::findTwin(const Owner& owner)
{
    const bool typed = canTellLive();
    int wanted = 0;
    if (typed) {
        int ownerSide = 0;
        if (!holderIsLive(owner.holder, &ownerSide) || ownerSide == 0) {
            return nullptr;
        }
        wanted = ownerSide == 1 ? 2 : 1;
    }

    void* candidates[kHolderSlots + 1] = {};
    std::size_t count = snapshotHolders(candidates, kHolderSlots);
    if (void* const own = m_ownClient.load(std::memory_order_acquire);
        own != nullptr && std::find(candidates, candidates + count, own) == candidates + count) {
        candidates[count++] = own;
    }
    for (std::size_t i = 0; i < count; ++i) {
        void* const other = candidates[i];
        if (other == owner.holder) {
            continue;
        }
        int slot = -1;
        std::byte* slots = nullptr;
        bool faulted = false;
        if (!readSlotValue(other, kSelectedSlotOffset, kSlotCount, slot) || slot != owner.slot) {
            continue;
        }
        if (!resolveSlots(other, slots, &faulted)) {
            if (faulted) {
                forgetHolder(other);
            }
            continue;
        }
        if (slots == owner.slots) {
            continue;
        }
        if (typed) {
            int side = 0;
            if (!holderIsLive(other, &side) || side != wanted) {
                continue;
            }
        }
        if (sameHotbar(owner.slots, slots)) {
            return other;
        }
    }
    return nullptr;
}

AutoTool& AutoTool::instance()
{
    static AutoTool module;
    return module;
}

bool AutoTool::available() const
{
    const Scanner& scanner = Scanner::instance();
    return scanner.found(Target::GetDestroySpeed) && scanner.found(Target::SetSelectedSlot);
}

MenuItem AutoTool::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(menu::back());
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    {
        MenuItem attack = menu::toggle(
            L"Attack switch", [this] { return attackSwitch(); },
            [this] { m_attackSwitch.store(!attackSwitch(), std::memory_order_relaxed); });
        attack.hidden = writes::blocked("AutoTool:attack");
        children.push_back(std::move(attack));
    }

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void AutoTool::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    m_attackSwitch.store(Config::getBool(section, "attackSwitch", kDefaultAttackSwitch), std::memory_order_relaxed);
}

void AutoTool::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["attackSwitch"] = attackSwitch();
}

void AutoTool::onScansReady()
{
    m_attackButton = GameButtons::instance().watchButton(gamebuttonlogic::button::destroyOrAttack);
    if (!available() && enabled()) {
        log().warn(L"AutoTool: required functions not found, disabling");
        setEnabled(false);
    }
    resolveAttack();
}

thread_local bool AutoTool::t_holdAttackTx = false;
thread_local void* AutoTool::t_holdPlayer = nullptr;

void AutoTool::resolveAttack()
{
    const Scanner& scanner = Scanner::instance();
    AttackApi api;
    api.damageCalc = scanner.address(Target::AttackDamageCalc);
    auto disp32 = [](const std::byte* at) {
        std::int32_t v = 0;
        std::memcpy(&v, at, 4);
        return static_cast<std::ptrdiff_t>(v);
    };
    if (const std::byte* site = scanner.address(Target::AnnouncedSlotSite);
        site != nullptr && memory::isReadable(site, 59)) {
        api.inventory = disp32(site + 3);
        api.announced = disp32(site + 55);
    }
    if (const std::byte* site = scanner.address(Target::TargetCategorySite);
        site != nullptr && memory::isReadable(site, 12)) {
        api.category = disp32(site + 8);
    }
    if (std::byte* ref = scanner.address(Target::PlayerVtableRef)) {
        api.localPlayerVtable = memory::ripTarget(ref, 3);
    }
    if (std::byte* site = scanner.address(Target::ArmorStandVtableSite)) {
        api.armorStandVtable = memory::ripTarget(site, 0x18);
    }
    const bool fieldsOk = api.inventory > 0 && api.inventory < 0x4000 && api.inventory % 8 == 0
                          && api.announced > 0 && api.announced < 0x4000 && api.category > 0
                          && api.category < 0x2000;
    api.ready = api.damageCalc != nullptr && fieldsOk && api.localPlayerVtable != nullptr
                && hooks::attackHooksInstalled() && !writes::blocked("AutoTool:attack");
    m_attack = api;
    log().info(L"AutoTool: attack switching {} (inventory +{:#x}, announced slot +{:#x}, category +{:#x})",
               api.ready ? L"ready" : L"NOT usable", api.inventory, api.announced, api.category);
}

bool AutoTool::onAttack(void* gameMode, void* target, bool direct, const void* hitPos)
{
    if (!enabled() || !m_attack.ready || gameMode == nullptr || target == nullptr) {
        return hooks::callAttackCore(gameMode, target, direct, hitPos);
    }
    void* player = nullptr;
    void* playerVt = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(gameMode) + 0x08, player) || player == nullptr
        || !readPointerGuarded(player, playerVt) || playerVt != m_attack.localPlayerVtable) {
        return hooks::callAttackCore(gameMode, target, direct, hitPos);
    }
    void* inventory = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(player) + m_attack.inventory, inventory) || inventory == nullptr) {
        return hooks::callAttackCore(gameMode, target, direct, hitPos);
    }
    auto* const inv = static_cast<std::byte*>(inventory);
    if (!direct || !attackSwitch()) {
        return callAttackHoldingIfUnannounced(gameMode, target, direct, hitPos, player, inv);
    }
    unsigned char category = 0;
    void* targetVt = nullptr;
    if (!readByteGuarded(static_cast<std::byte*>(target) + m_attack.category, category) || (category & 0x3) == 0
        || (m_attack.armorStandVtable != nullptr && readPointerGuarded(target, targetVt)
            && targetVt == m_attack.armorStandVtable)) {
        return callAttackHoldingIfUnannounced(gameMode, target, direct, hitPos, player, inv);
    }
    int current = -1;
    unsigned char containerId = 0xFF;
    if (!readSlotValue(inventory, kSelectedSlotOffset, kSlotCount, current)
        || !readByteGuarded(inv + 0xB0, containerId) || containerId != 0) {
        return hooks::callAttackCore(gameMode, target, direct, hitPos);
    }
    float damages[kSlotCount] = {};
    if (!probeSlotDamages(m_attack.damageCalc, reinterpret_cast<int*>(inv + kSelectedSlotOffset), current, player,
                          target, kSlotCount, damages)) {
        static std::atomic<int> told{0};
        if (told.fetch_add(1, std::memory_order_relaxed) < 3) {
            log().warn(L"AutoTool: computing the attack damage faulted; attacking without switching");
        }
        return hooks::callAttackCore(gameMode, target, direct, hitPos);
    }
    constexpr float kEpsilon = 0.0001f;
    float best = damages[current];
    int bestSlot = current;
    for (int slot = 0; slot < kSlotCount; ++slot) {
        if (damages[slot] > best + kEpsilon) {
            best = damages[slot];
            bestSlot = slot;
        }
    }
    const unsigned long long now = GetTickCount64();
    m_lastAttackMs.store(now, std::memory_order_relaxed);
    if (bestSlot != current) {
        bool noRoom = false;
        {
            const std::lock_guard<std::mutex> lock(m_stateMutex);
            Switch* found = nullptr;
            Switch* empty = nullptr;
            for (Switch& one : m_switches) {
                if (one.owner == inventory) {
                    found = &one;
                    break;
                }
                if (one.owner == nullptr && empty == nullptr) {
                    empty = &one;
                }
            }
            if (found == nullptr && empty != nullptr) {
                empty->owner = inventory;
                empty->twin = nullptr;
                empty->originalSlot = current;
            } else if (found == nullptr) {
                noRoom = true;
            }
        }
        if (!noRoom && applySlot(inventory, nullptr, bestSlot)) {
            m_attackOwner.store(inventory, std::memory_order_release);
            m_attackSlot.store(bestSlot, std::memory_order_release);
            if (m_attackLogs.fetch_add(1, std::memory_order_relaxed) < 16) {
                log().info(L"AutoTool: attack: slot {} -> {} (damage {:.2f} -> {:.2f})", current + 1, bestSlot + 1,
                           damages[current], best);
            }
        }
    }
    return callAttackHoldingIfUnannounced(gameMode, target, direct, hitPos, player, inv);
}

bool AutoTool::callAttackHoldingIfUnannounced(void* gameMode, void* target, bool direct, const void* hitPos,
                                              void* player, std::byte* inv)
{
    int announced = -1;
    int nowSlot = -1;
    const bool readOk = readIntGuarded(static_cast<std::byte*>(player) + m_attack.announced, announced)
                        && readIntGuarded(inv + kSelectedSlotOffset, nowSlot);
    if (!readOk || announced == nowSlot) {
        return hooks::callAttackCore(gameMode, target, direct, hitPos);
    }
    t_holdAttackTx = true;
    t_holdPlayer = player;
    const bool result = hooks::callAttackCore(gameMode, target, direct, hitPos);
    t_holdAttackTx = false;
    t_holdPlayer = nullptr;
    return result;
}

bool AutoTool::onSendTransaction(void* player, void** transaction)
{
    if (!t_holdAttackTx || player != t_holdPlayer || transaction == nullptr || *transaction == nullptr) {
        return false;
    }
    if (m_heldCount >= kMaxHeld) {
        return false;
    }
    int slot = -1;
    void* inventory = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(player) + m_attack.inventory, inventory) || inventory == nullptr
        || !readSlotValue(inventory, kSelectedSlotOffset, kSlotCount, slot)) {
        return false;
    }
    Held& held = m_held[m_heldCount++];
    held.tx = *transaction;
    held.player = player;
    held.slot = slot;
    held.atMs = GetTickCount64();
    *transaction = nullptr;
    return true;
}

void AutoTool::flushHeld(bool dropAll)
{
    std::size_t lastReady = kMaxHeld;
    if (!dropAll) {
        for (std::size_t i = 0; i < m_heldCount; ++i) {
            int announced = -1;
            void* vt = nullptr;
            if (readPointerGuarded(m_held[i].player, vt) && vt == m_attack.localPlayerVtable
                && readIntGuarded(static_cast<std::byte*>(m_held[i].player) + m_attack.announced, announced)
                && announced == m_held[i].slot) {
                lastReady = i;
            }
        }
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < m_heldCount; ++i) {
        const Held one = m_held[i];
        bool send = false;
        bool drop = dropAll;
        const unsigned long long waited = GetTickCount64() - one.atMs;
        if (!drop) {
            void* vt = nullptr;
            int announced = -1;
            const bool alive = readPointerGuarded(one.player, vt) && vt == m_attack.localPlayerVtable
                               && readIntGuarded(static_cast<std::byte*>(one.player) + m_attack.announced,
                                                 announced);
            if (!alive) {
                drop = true;
            } else if (announced == one.slot) {
                send = true;
            } else if ((lastReady != kMaxHeld && i < lastReady) || waited > kHeldTimeoutMs) {
                drop = true;
            }
        }
        if (!send && !drop) {
            m_held[kept++] = one;
            continue;
        }
        void* tx = one.tx;
        if (send) {
            hooks::callSendComplexTx(one.player, &tx);
            if (m_attackLogs.load(std::memory_order_relaxed) < 16) {
                log().info(L"AutoTool: sent the held attack after the switch was announced ({} ms)", waited);
            }
        } else if (!dropAll) {
            static std::atomic<int> told{0};
            if (told.fetch_add(1, std::memory_order_relaxed) < 5) {
                log().warn(L"AutoTool: dropped a held attack (the slot switch was not announced; {} ms)", waited);
            }
        }
        if (tx != nullptr) {
            destroyTransactionGuarded(tx);
        }
    }
    for (std::size_t i = kept; i < m_heldCount; ++i) {
        m_held[i] = Held{};
    }
    m_heldCount = kept;
}

bool AutoTool::hasSwitchFor(void* owner)
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    for (const Switch& one : m_switches) {
        if (one.owner == owner) {
            return true;
        }
    }
    return false;
}

bool AutoTool::anySwitched()
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    for (const Switch& one : m_switches) {
        if (one.owner != nullptr) {
            return true;
        }
    }
    return false;
}

bool AutoTool::applySlot(void* owner, void* twin, int slot)
{
    if (slot < 0 || slot >= kSlotCount) {
        return false;
    }

    void* const targets[] = {owner, twin};
    bool wrote = false;
    for (void* const holder : targets) {
        if (holder == nullptr) {
            continue;
        }
        int current = -1;
        if (!readSlotValue(holder, kSelectedSlotOffset, kSlotCount, current)) {
            forgetHolder(holder);
            continue;
        }
        void* const field = static_cast<std::byte*>(holder) + kSelectedSlotOffset;
        if (writeIntGuarded(field, slot)) {
            wrote = true;
        } else {
            forgetHolder(holder);
        }
    }
    return wrote;
}

void AutoTool::restoreSlot()
{
    Switch copies[kSwitchSlots];
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        for (std::size_t i = 0; i < kSwitchSlots; ++i) {
            copies[i] = m_switches[i];
            m_switches[i] = Switch{};
        }
    }

    int slot = -1;
    int holders = 0;
    for (const Switch& one : copies) {
        if (one.owner == nullptr || one.originalSlot < 0) {
            continue;
        }
        std::byte* slots = nullptr;
        if (!resolveSlots(one.owner, slots) || !holderIsLive(one.owner)) {
            continue;
        }
        void* twin = one.twin;
        if (twin != nullptr && (!resolveSlots(twin, slots) || !holderIsLive(twin))) {
            twin = nullptr;
        }
        if (applySlot(one.owner, twin, one.originalSlot)) {
            slot = one.originalSlot;
            ++holders;
        }
    }
    if (holders > 0) {
        log().info(L"AutoTool: restored slot {} ({} holder(s))", slot + 1, holders);
    }
}

void AutoTool::onSetSelectedSlot(void* rcx, void* rdx, void* r8, void* r9)
{
    trackHolder(rcx);
    if (rcx != nullptr && rcx == m_attackOwner.load(std::memory_order_acquire)) {
        const int slot = static_cast<int>(reinterpret_cast<std::uintptr_t>(rdx) & 0xFFFFFFFFu);
        if (slot != m_attackSlot.load(std::memory_order_acquire)) {
            {
                const std::lock_guard<std::mutex> lock(m_stateMutex);
                for (Switch& one : m_switches) {
                    if (one.owner == rcx) {
                        one = Switch{};
                    }
                }
            }
            m_attackOwner.store(nullptr, std::memory_order_release);
            log().info(L"AutoTool: slot {} chosen by hand after an attack switch; not restoring", slot + 1);
        }
    }
    hooks::callSetSelectedSlot(rcx, rdx, r8, r9);
}

float AutoTool::onGetDestroySpeed(void* rcx, void* rdx, void* r8, void* r9)
{

    if (!enabled() || rcx == nullptr) {
        return hooks::callGetDestroySpeed(rcx, rdx, r8, r9);
    }
    if (speedQueryIsForeign(rcx)) {
        return hooks::callGetDestroySpeed(rcx, rdx, r8, r9);
    }

    auto** const itemSlot =
        reinterpret_cast<void**>(static_cast<std::byte*>(rcx) + kItemPointerOffset);
    void* savedItem = nullptr;
    if (!readPointerGuarded(itemSlot, savedItem) || savedItem == nullptr) {
        return hooks::callGetDestroySpeed(rcx, rdx, r8, r9);
    }

    Owner owner;
    if (!findOwner(savedItem, owner, contextPlayer(rcx))) {
        return hooks::callGetDestroySpeed(rcx, rdx, r8, r9);
    }
    const int currentSlot = owner.slot;

    m_lastSpeedQuery.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);

    float speeds[kSlotCount] = {};
    if (!probeSlotSpeeds(itemSlot, owner.slots, savedItem, kSlotStride, kSlotCount, rcx, rdx, r8,
                         r9, speeds)) {
        forgetHolder(owner.holder);
        log().warn(L"AutoTool: probing the hotbar faulted inside the game; "
                   L"switch hotbar slots once to recover (the module stays on)");
        return hooks::callGetDestroySpeed(rcx, rdx, r8, r9);
    }

    float best = speeds[0];
    for (const float speed : speeds) {
        best = (std::max)(best, speed);
    }

    constexpr float kEpsilon = 0.0001f;
    int bestSlot = currentSlot;
    if (speeds[currentSlot] < best - kEpsilon) {
        for (int slot = 0; slot < kSlotCount; ++slot) {
            if (speeds[slot] >= best - kEpsilon) {
                bestSlot = slot;
                break;
            }
        }
    }

    if (bestSlot != currentSlot) {
        void* twin = hasSwitchFor(owner.holder) ? nullptr : findTwin(owner);
        int fromSlot = -1;
        bool noRoom = false;
        {
            const std::lock_guard<std::mutex> lock(m_stateMutex);
            Switch* found = nullptr;
            Switch* empty = nullptr;
            for (Switch& one : m_switches) {
                if (one.owner == owner.holder) {
                    found = &one;
                    break;
                }
                if (one.owner == nullptr && empty == nullptr) {
                    empty = &one;
                }
            }
            if (found != nullptr) {
                twin = found->twin;
            } else if (empty != nullptr) {
                empty->owner = owner.holder;
                empty->twin = twin;
                empty->originalSlot = currentSlot;
                fromSlot = currentSlot;
            } else {
                noRoom = true;
            }
        }
        if (noRoom) {
            static std::atomic<int> told{0};
            if (told.fetch_add(1, std::memory_order_relaxed) < 3) {
                log().warn(L"AutoTool: too many slot holders are switched at once; leaving this "
                           L"one alone so it can always be restored");
            }
            return speeds[currentSlot];
        }
        if (fromSlot >= 0) {
            log().info(L"AutoTool: slot {} -> {} (speed {:.1f} -> {:.1f}{})", fromSlot + 1,
                       bestSlot + 1, speeds[currentSlot], best,
                       twin != nullptr ? L", server-side copy too" : L"");
        }
        applySlot(owner.holder, twin, bestSlot);
    }

    return speeds[currentSlot];
}

void AutoTool::onUpdate()
{

    if (!anySwitched()) {
        return;
    }

    const auto lastQuery =
        Clock::time_point(Clock::duration(m_lastSpeedQuery.load(std::memory_order_relaxed)));

    const bool idle = (Clock::now() - lastQuery) > std::chrono::milliseconds(kIdleRestoreMs);
    const bool released = !GameButtons::instance().buttonHeld(m_attackButton);
    const bool unfocused = !input::isInGameplay();
    const bool fighting = attackedRecently();

    if (unfocused || ((idle || released) && !fighting)) {
        m_restoreWanted.store(true, std::memory_order_release);
    }
}

void AutoTool::onPlayerViewUpdate()
{
    m_ownClient.store(enabled() ? HandRestock::instance().ownClientHolder() : nullptr, std::memory_order_release);

    if (m_heldCount > 0) {
        flushHeld(false);
    }
    if (m_restoreWanted.exchange(false, std::memory_order_acq_rel)) {
        if (m_heldCount > 0) {
            m_restoreWanted.store(true, std::memory_order_release);
            return;
        }
        if (enabled() && input::isInGameplay() && attackedRecently()) {
            return;
        }
        m_attackOwner.store(nullptr, std::memory_order_release);
        restoreSlot();
    }
}

bool AutoTool::attackedRecently() const
{
    const unsigned long long lastAttack = m_lastAttackMs.load(std::memory_order_relaxed);
    return lastAttack != 0 && GetTickCount64() - lastAttack < kAttackRestoreMs;
}

void AutoTool::onEnabledChanged(bool enabled)
{
    if (enabled) {
        void* any[1] = {};
        if (snapshotHolders(any, 1) == 0) {
            log().info(L"AutoTool: switch hotbar slots once to activate");
        }
    } else {
        m_restoreWanted.store(true, std::memory_order_release);
    }
}

void AutoTool::shutdown()
{
    restoreSlot();
    if (m_heldCount > 0) {
        log().info(L"AutoTool: {} held attack(s) were discarded at unload", m_heldCount);
        flushHeld(true);
    }
}

}
