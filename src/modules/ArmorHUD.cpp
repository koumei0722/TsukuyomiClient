#include "modules/ArmorHUD.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "game/ContainerUi.h"
#include "game/GameData.h"
#include "game/InventoryHudLayout.h"
#include "game/UiProbe.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tsukuyomi {

namespace {

namespace cui = containerui;
namespace ah = armorhud;

constexpr int kSlotVisible = 230, kSlotVMid = 231, kSlotVBottom = 232, kSlotHMid = 233, kSlotHRight = 234;
constexpr int kSlotFirstForm = 235;
constexpr int kSlotBar = 239;
constexpr int kSlotFirstFloat = 64;
constexpr int kSlotFirstText = 338;
static_assert(kSlotFirstForm + ah::kForms <= 239);
static_assert(kSlotFirstFloat + ah::kForms * 2 <= cui::kPersistentSlots);
static_assert(kSlotFirstText + ah::kItems <= cui::kPersistentTextSlots);

constexpr std::uint32_t kEquipmentTypeId = 0xB06141A9u;
constexpr std::size_t kMaxEquipStride = 0x40;
constexpr std::size_t kArmorSlots = 4;

constexpr std::ptrdiff_t kOffhandOffset = 0xD90;
constexpr std::ptrdiff_t kMainhandOffset = 0xE28;

constexpr std::ptrdiff_t kInvContainerOffset = 0xB8;
constexpr std::ptrdiff_t kInvSlotsOffset = 0x198;
constexpr int kHotbarSlots = 9;
constexpr std::size_t kInventorySlots = 36;

void writeBool(int slot, bool value)
{
    if (volatile std::uint8_t* const p = cui::persistentBool(slot)) {
        *p = value ? 1 : 0;
    }
}
void writeFloat(int slot, float value)
{
    if (volatile float* const p = cui::persistentFloat(slot)) {
        *p = value;
    }
}

int readDisp32(const std::byte* at)
{
    std::int32_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

LONG accessFilter(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER
                                                                                 : EXCEPTION_CONTINUE_SEARCH;
}

bool isArmorCollection(const void* str)
{
    constexpr std::size_t kLength = sizeof(ah::kCollection) - 1;
    __try {
        const auto* const bytes = static_cast<const std::byte*>(str);
        if (*reinterpret_cast<const std::size_t*>(bytes + 0x10) != kLength) {
            return false;
        }
        const std::size_t capacity = *reinterpret_cast<const std::size_t*>(bytes + 0x18);
        const char* const text = capacity >= 16 ? *reinterpret_cast<const char* const*>(bytes)
                                                : reinterpret_cast<const char*>(bytes);
        return std::memcmp(text, ah::kCollection, kLength) == 0;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

bool durabilityOf(const void* stack, std::int32_t slot, void* damageValue, int& max, int& damage)
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
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

std::atomic<int> g_bindLogs{0};
constexpr int kMaxBindLogs = 4;
std::atomic<bool> g_armorMissingLogged{false};
std::atomic<bool> g_handsMismatchLogged{false};
std::atomic<unsigned long long> g_asks{0};

std::atomic<void*> g_hudMc{nullptr};

}

ArmorHUD& ArmorHUD::instance()
{
    static ArmorHUD module;
    return module;
}

bool ArmorHUD::available() const
{
    return m_definitionRegistered;
}

void ArmorHUD::onScansReady()
{
    if (!cui::bindingsAvailable() || !cui::floatBindingsAvailable() || cui::persistentBool(0) == nullptr) {
        log().warn(L"ArmorHUD: NOT usable (bindings {} / float bindings {} / value storage {})",
                   cui::bindingsAvailable() ? L"ready" : L"missing",
                   cui::floatBindingsAvailable() ? L"ready" : L"missing",
                   cui::persistentBool(0) != nullptr ? L"ready" : L"missing");
        return;
    }
    Scanner& scanner = Scanner::instance();
    if (const std::byte* const at = scanner.address(Target::ArmorContainerGetter);
        at != nullptr && memory::isReadable(at, 0xD0)) {
        const unsigned shift = static_cast<unsigned>(at[0xC9]);
        const std::size_t field = static_cast<std::size_t>(at[0xCE]);
        if (shift >= 3 && shift <= 6 && (std::size_t{1} << shift) <= kMaxEquipStride
            && field + sizeof(void*) <= (std::size_t{1} << shift) && field % 8 == 0) {
            m_equipStride = std::size_t{1} << shift;
            m_armorField = field;
        }
    }
    if (const std::byte* const at = scanner.address(Target::SimpleContainerGetItem);
        at != nullptr && memory::isReadable(at, 0x58)) {
        const int items = readDisp32(at + 0x33);
        const int stride = readDisp32(at + 0x54);
        if (items > 0 && items < 0x1000 && items % 8 == 0 && stride >= 0x40 && stride < 0x400) {
            m_itemsOffset = static_cast<std::size_t>(items);
            m_stackStride = static_cast<std::size_t>(stride);
        }
    }
    if (const std::byte* const at = scanner.address(Target::ItemMaxDamageSlotSite);
        at != nullptr && memory::isReadable(at + 6, 4)) {
        const int slot = readDisp32(at + 6);
        m_maxDamageSlot = slot > 0 && slot < 0x1000 && slot % 8 == 0 ? slot : 0;
    }
    m_damageValue = scanner.address(Target::ItemStackDamageValue);
    if (const std::byte* const at = scanner.address(Target::AnnouncedSlotSite);
        at != nullptr && memory::isReadable(at, 18)) {
        const int inventory = readDisp32(at + 3);
        const int containerId = readDisp32(at + 11);
        const int selected = static_cast<int>(static_cast<std::uint8_t>(at[17]));
        if (inventory > 0 && inventory < 0x4000 && inventory % 8 == 0 && containerId > 0 && containerId < 0x1000
            && selected > 0 && selected < 0x100) {
            m_inventoryField = inventory;
            m_containerIdField = containerId;
            m_selectedField = selected;
        }
    }

    log().info(L"ArmorHUD: equipment component stride {:#x} armor field {:#x}, container items {:#x} stride {:#x}, "
               L"durability {}, inventory +{:#x} (selected +{:#x} / container id +{:#x})",
               m_equipStride, m_armorField, m_itemsOffset, m_stackStride,
               m_maxDamageSlot != 0 && m_damageValue != nullptr ? L"ready" : L"missing", m_inventoryField,
               m_selectedField, m_containerIdField);
    if (m_inventoryField == 0) {
        log().warn(L"ArmorHUD: the player inventory cannot be located; the main hand is not shown");
    }
    if (m_equipStride == 0 || m_itemsOffset == 0) {
        log().warn(L"ArmorHUD: the armor container cannot be located; only the hands are shown");
    }

    publish();
    cui::addHudObserver(&ArmorHUD::onHudCreated);
    uiprobe::registerDefExtension("hud", "hud_content", "", ah::layoutJson());
    m_definitionRegistered = true;
    log().info(L"ArmorHUD: ready (the items appear in worlds entered after injection)");
}

bool ArmorHUD::wanted() const
{
    return enabled() && !m_shuttingDown.load(std::memory_order_relaxed);
}

void ArmorHUD::publish()
{
    const invhud::Placement place = invhud::placementOf(anchor());
    const int form = ah::formOf(orientation(), textMode());
    writeBool(kSlotVisible, wanted());
    writeBool(kSlotVMid, place.vMid);
    writeBool(kSlotVBottom, place.vBottom);
    writeBool(kSlotHMid, place.hMid);
    writeBool(kSlotHRight, place.hRight);
    writeBool(kSlotBar, showBar());
    for (int f = 0; f < ah::kForms; ++f) {
        const ah::FormSize size = ah::formSize(f);
        writeBool(kSlotFirstForm + f, f == form);
        writeFloat(kSlotFirstFloat + f * 2, static_cast<float>(offsetX()) / static_cast<float>(size.width));
        writeFloat(kSlotFirstFloat + f * 2 + 1, static_cast<float>(offsetY()) / static_cast<float>(size.height));
    }
}

void* ArmorHUD::hudManager()
{
    return g_hudMc.load(std::memory_order_relaxed);
}

const void* ArmorHUD::stackOf(int index) const
{
    if (index < 0 || index >= ah::kItems) {
        return nullptr;
    }
    void* const player = GameData::instance().player();
    if (!memory::plausiblePointer(player)) {
        return nullptr;
    }
    const auto* const base = static_cast<const std::byte*>(player);
    void* hands[2] = {};
    if (!memory::copyGuarded(base + kOffhandOffset, &hands[0], sizeof(void*))
        || !memory::copyGuarded(base + kMainhandOffset, &hands[1], sizeof(void*))
        || hands[0] == nullptr || hands[0] != hands[1]) {
        if (!g_handsMismatchLogged.exchange(true, std::memory_order_relaxed)) {
            log().warn(L"ArmorHUD: cannot read the hands of the player; nothing is shown");
        }
        return nullptr;
    }
    void* const stackVtable = hands[0];
    if (index == ah::kMainhand) {
        return selectedStack(base, stackVtable);
    }
    if (index == ah::kOffhand) {
        return base + kOffhandOffset;
    }
    void* const container = m_armorContainer.load(std::memory_order_acquire);
    if (container == nullptr || m_armorOwner.load(std::memory_order_acquire) != player || m_itemsOffset == 0
        || m_stackStride == 0) {
        return nullptr;
    }
    std::uintptr_t range[2] = {};
    if (!memory::copyGuarded(static_cast<const std::byte*>(container) + m_itemsOffset, range, sizeof(range))
        || range[1] <= range[0] || (range[1] - range[0]) % m_stackStride != 0
        || (range[1] - range[0]) / m_stackStride < kArmorSlots) {
        return nullptr;
    }
    const auto* const stack = reinterpret_cast<const std::byte*>(range[0]) + static_cast<std::size_t>(index) * m_stackStride;
    void* vtable = nullptr;
    if (!memory::copyGuarded(stack, &vtable, sizeof(vtable)) || vtable != stackVtable) {
        return nullptr;
    }
    return stack;
}

const void* ArmorHUD::selectedStack(const std::byte* player, void* stackVtable) const
{
    if (m_inventoryField == 0 || m_stackStride == 0) {
        return nullptr;
    }
    const std::byte* inventory = nullptr;
    if (!memory::copyGuarded(player + m_inventoryField, &inventory, sizeof(inventory))
        || !memory::plausiblePointer(inventory)) {
        return nullptr;
    }
    std::uint8_t containerId = 0xFF;
    std::int32_t selected = -1;
    const std::byte* container = nullptr;
    if (!memory::copyGuarded(inventory + m_containerIdField, &containerId, sizeof(containerId)) || containerId != 0
        || !memory::copyGuarded(inventory + m_selectedField, &selected, sizeof(selected)) || selected < 0
        || selected >= kHotbarSlots
        || !memory::copyGuarded(inventory + kInvContainerOffset, &container, sizeof(container))
        || !memory::plausiblePointer(container)) {
        return nullptr;
    }
    std::uintptr_t range[2] = {};
    if (!memory::copyGuarded(container + kInvSlotsOffset, range, sizeof(range)) || range[1] <= range[0]
        || (range[1] - range[0]) % m_stackStride != 0 || (range[1] - range[0]) / m_stackStride < kInventorySlots) {
        return nullptr;
    }
    const auto* const stack =
        reinterpret_cast<const std::byte*>(range[0]) + static_cast<std::size_t>(selected) * m_stackStride;
    void* vtable = nullptr;
    if (!memory::copyGuarded(stack, &vtable, sizeof(vtable)) || vtable != stackVtable) {
        return nullptr;
    }
    return stack;
}

const void* ArmorHUD::stackFor(const void* collectionName, int index)
{
    if (collectionName == nullptr || index < 0 || index >= ah::kItems || !isArmorCollection(collectionName)) {
        return nullptr;
    }
    if (g_asks.fetch_add(1, std::memory_order_relaxed) == 0) {
        log().info(L"ArmorHUD: the HUD asked for {} (answering with the armor and the hands)", L"tk_armor_items");
    }
    ArmorHUD& self = instance();
    if (!self.wanted()) {
        return nullptr;
    }
    return self.stackOf(index);
}

void ArmorHUD::onPlayerViewUpdate()
{
    if (!m_definitionRegistered.load(std::memory_order_relaxed)) {
        return;
    }
    void* const player = GameData::instance().player();
    void* container = nullptr;
    if (wanted() && memory::plausiblePointer(player) && m_equipStride != 0) {
        alignas(8) std::byte component[kMaxEquipStride] = {};
        if (GameData::copyComponent(player, kEquipmentTypeId, m_equipStride, component)) {
            std::memcpy(&container, component + m_armorField, sizeof(container));
            if (!memory::plausiblePointer(container)) {
                container = nullptr;
            }
        }
        if (container == nullptr && !g_armorMissingLogged.exchange(true, std::memory_order_relaxed)) {
            log().warn(L"ArmorHUD: the player has no readable armor container; the armor slots stay empty");
        }
    }
    m_armorOwner.store(player, std::memory_order_release);
    m_armorContainer.store(container, std::memory_order_release);

    const int mode = textMode();
    const bool textForm = wanted() && mode != ah::kNoText && m_maxDamageSlot != 0 && m_damageValue != nullptr;
    for (int i = 0; i < ah::kItems; ++i) {
        std::string text;
        if (textForm) {
            if (const void* const stack = stackOf(i)) {
                int max = 0;
                int damage = 0;
                if (durabilityOf(stack, m_maxDamageSlot, m_damageValue, max, damage)) {
                    text = ah::durabilityText(mode, max, damage);
                }
            }
        }
        if (text != m_lastText[i]) {
            m_lastText[i] = text;
            cui::writePersistentText(kSlotFirstText + i, text.data(), text.size());
        }
    }
}

void ArmorHUD::onUpdate()
{
    publish();
}

void ArmorHUD::onEnabledChanged(bool)
{
    publish();
}

void ArmorHUD::shutdown()
{
    m_shuttingDown.store(true, std::memory_order_relaxed);
    publish();
}

void ArmorHUD::onHudCreated(void* ctrl)
{
    ArmorHUD& self = instance();
    if (!self.m_definitionRegistered) {
        return;
    }
    void* const mc = cui::hudContainerManager(ctrl);
    g_hudMc.store(mc, std::memory_order_relaxed);
    self.m_armorContainer.store(nullptr, std::memory_order_release);
    self.publish();
    int ok = 0;
    ok += cui::bindPersistentBool(ctrl, ah::kBindVisible, kSlotVisible) ? 1 : 0;
    ok += cui::bindPersistentBool(ctrl, ah::kBindVMid, kSlotVMid) ? 1 : 0;
    ok += cui::bindPersistentBool(ctrl, ah::kBindVBottom, kSlotVBottom) ? 1 : 0;
    ok += cui::bindPersistentBool(ctrl, ah::kBindHMid, kSlotHMid) ? 1 : 0;
    ok += cui::bindPersistentBool(ctrl, ah::kBindHRight, kSlotHRight) ? 1 : 0;
    ok += cui::bindPersistentBool(ctrl, ah::kBindBar, kSlotBar) ? 1 : 0;
    for (int f = 0; f < ah::kForms; ++f) {
        ok += cui::bindPersistentBool(ctrl, ah::formVisibleBinding(f).c_str(), kSlotFirstForm + f) ? 1 : 0;
        ok += cui::bindPersistentFloat(ctrl, ah::formXBinding(f).c_str(), kSlotFirstFloat + f * 2) ? 1 : 0;
        ok += cui::bindPersistentFloat(ctrl, ah::formYBinding(f).c_str(), kSlotFirstFloat + f * 2 + 1) ? 1 : 0;
    }
    for (int i = 0; i < ah::kItems; ++i) {
        ok += cui::bindPersistentText(ctrl, ah::textBinding(i).c_str(), kSlotFirstText + i) ? 1 : 0;
    }
    constexpr int kWanted = 6 + ah::kForms * 3 + ah::kItems;
    if (g_bindLogs.fetch_add(1, std::memory_order_relaxed) < kMaxBindLogs) {
        if (ok == kWanted) {
            log().info(L"ArmorHUD: bound {} values to the HUD controller", ok);
        } else {
            log().warn(L"ArmorHUD: bound only {} of {} values to the HUD controller", ok, kWanted);
        }
        if (mc == nullptr) {
            log().warn(L"ArmorHUD: the HUD container manager is unknown; the items stay empty");
        }
    }
}

MenuItem ArmorHUD::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());

    {
        std::vector<std::wstring> labels;
        for (int i = 0; i < ah::kOrientationCount; ++i) {
            labels.emplace_back(ah::orientationLabel(i));
        }
        children.push_back(menu::choice(
            L"Layout", std::move(labels), [this] { return orientation(); },
            [this](int value) { m_orientation.store(ah::clampOrientation(value), std::memory_order_relaxed); }));
    }
    children.push_back(menu::toggle(
        L"Durability bar", [this] { return showBar(); },
        [this] { m_bar.store(!showBar(), std::memory_order_relaxed); }));
    {
        std::vector<std::wstring> labels;
        for (int i = 0; i < ah::kTextModeCount; ++i) {
            labels.emplace_back(ah::textModeLabel(i));
        }
        children.push_back(menu::choice(
            L"Durability text", std::move(labels), [this] { return textMode(); },
            [this](int value) { m_text.store(ah::clampTextMode(value), std::memory_order_relaxed); }));
    }
    {
        std::vector<std::wstring> anchors;
        for (int i = 0; i < invhud::kAnchorCount; ++i) {
            anchors.emplace_back(invhud::anchorLabel(i));
        }
        children.push_back(menu::choice(
            L"Anchor", std::move(anchors), [this] { return anchor(); },
            [this](int value) { m_anchor.store(ah::clampAnchor(value), std::memory_order_relaxed); }));
    }
    children.push_back(menu::number(
        L"Offset X", [this] { return static_cast<float>(offsetX()); },
        [this](float value) {
            m_offsetX.store(ah::clampOffset(static_cast<int>(std::lround(value))), std::memory_order_relaxed);
        },
        true, static_cast<float>(ah::kOffsetMin), static_cast<float>(ah::kOffsetMax)));
    children.push_back(menu::number(
        L"Offset Y", [this] { return static_cast<float>(offsetY()); },
        [this](float value) {
            m_offsetY.store(ah::clampOffset(static_cast<int>(std::lround(value))), std::memory_order_relaxed);
        },
        true, static_cast<float>(ah::kOffsetMin), static_cast<float>(ah::kOffsetMax)));
    {
        MenuItem reset = menu::action(L"Reset offset", [this] {
            m_offsetX.store(0, std::memory_order_relaxed);
            m_offsetY.store(0, std::memory_order_relaxed);
        });
        reset.value = [] { return std::wstring(L"Reset"); };
        children.push_back(std::move(reset));
    }

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void ArmorHUD::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    m_orientation.store(ah::clampOrientation(Config::getInt(section, "layout", ah::kDefaultOrientation)),
                        std::memory_order_relaxed);
    m_anchor.store(ah::clampAnchor(Config::getInt(section, "anchor", ah::kDefaultAnchor)),
                   std::memory_order_relaxed);
    m_bar.store(Config::getBool(section, "bar", ah::kDefaultBar), std::memory_order_relaxed);
    m_text.store(ah::clampTextMode(Config::getInt(section, "text", ah::kDefaultText)),
                 std::memory_order_relaxed);
    m_offsetX.store(ah::clampOffset(Config::getInt(section, "offsetX", ah::kDefaultOffsetX)),
                    std::memory_order_relaxed);
    m_offsetY.store(ah::clampOffset(Config::getInt(section, "offsetY", ah::kDefaultOffsetY)),
                    std::memory_order_relaxed);
}

void ArmorHUD::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["layout"] = orientation();
    section["bar"] = showBar();
    section["text"] = textMode();
    section["anchor"] = anchor();
    section["offsetX"] = offsetX();
    section["offsetY"] = offsetY();
}

}
