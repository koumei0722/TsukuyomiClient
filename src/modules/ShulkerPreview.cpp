#include "modules/ShulkerPreview.h"

#include "config/WriteSwitches.h"
#include "core/Logger.h"
#include "core/Strings.h"
#include "game/BlockRegistry.h"
#include "game/ShulkerFill.h"
#include "game/ShulkerPreviewLayout.h"
#include "game/UiProbe.h"
#include "input/GameButtons.h"

#include <Windows.h>

#include <atomic>
#include <string>

namespace tsukuyomi {

namespace {

namespace cui = containerui;
using json = nlohmann::json;

std::string miniDefinition()
{
    const json icon = {{"tk_sp_mini",
                        {{"type", "custom"},
                         {"renderer", "inventory_item_renderer"},
                         {"size", {"50%", "50%"}},
                         {"anchor_from", "bottom_right"},
                         {"anchor_to", "bottom_right"},
                         {"layer", 1},
                         {"bindings",
                          json::array({{{"binding_name", "#tk_sp_mini"},
                                        {"binding_name_override", "#item_id_aux"},
                                        {"binding_type", "collection"},
                                        {"binding_collection_name", "$item_collection_name"}}})}}}};
    return json::array({icon}).dump();
}

}

ShulkerPreview& ShulkerPreview::instance()
{
    static ShulkerPreview module;
    return module;
}

bool ShulkerPreview::miniAvailable() const
{
    return cui::available() && cui::collectionBindingsAvailable() && !writes::blocked("ShulkerPreview:icons");
}

bool ShulkerPreview::writeBlocked() const
{
    return Module::writeBlocked()
           || (writes::blocked("ShulkerPreview:tooltip") && writes::blocked("ShulkerPreview:icons"));
}

bool ShulkerPreview::available() const
{
    return tooltipAvailable() || miniAvailable();
}

bool ShulkerPreview::wantPreview() const
{
    return enabled() && (m_previewKey.empty() || m_previewKey.isDown());
}

void ShulkerPreview::onScansReady()
{
    cui::onScansReady();
    cui::addObserver(this);
    resolveTooltip();
    m_wheelLeftButton = GameButtons::instance().watchButton(gamebuttonlogic::button::inventoryLeft);
    m_wheelRightButton = GameButtons::instance().watchButton(gamebuttonlogic::button::inventoryRight);
    if (cui::available()) {
        registerUiDefinitions();
    }
    if (miniAvailable()) {
        cui::addHudObserver(&ShulkerPreview::onHudCreated);
    }
    log().info(L"ShulkerPreview: tooltip {} / full-box icons {}", tooltipAvailable() ? L"ready" : L"NOT usable",
               miniAvailable() ? L"ready" : L"NOT usable");
}

void ShulkerPreview::shutdown()
{
    cui::removeObserver(this);
    cui::detachPersistentCollectionInt();
    const std::lock_guard<std::mutex> lock(m_stacksLock);
    m_currentSet = 0;
    m_slotSets.clear();
    for (ContentSet& set : m_sets) {
        set.id = 0;
        set.key.clear();
        set.live.fill(false);
    }
}

void ShulkerPreview::onEnabledChanged(bool)
{
}

void ShulkerPreview::registerUiDefinitions()
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    uiprobe::registerDefExtension("common", "item_renderer", "", miniDefinition());
    uiprobe::registerDefExtension("hud", "hotbar_hud_item_icon", "", miniDefinition());
    if (tooltipAvailable()) {
        uiprobe::registerDefExtension("common", "stack_splitting_overlay", "", spreview::bundleJson());
        uiprobe::registerDefReplaceControls("common", "highlight_slot_panel", spreview::hoverPanelControlsJson());
    }
}

MenuItem ShulkerPreview::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    MenuItem key = menu::keybind(
        L"Preview key", [this] { return m_previewKey.combo(); },
        [this](std::vector<int> combo) {
            m_previewKey.set(std::move(combo));
            log().info(L"ShulkerPreview: preview key set to {}", m_previewKey.name());
        },
        {});
    bindPad(key, m_previewKey);
    key.available = [this] { return tooltipAvailable(); };
    key.hidden = writes::blocked("ShulkerPreview:tooltip");
    children.push_back(std::move(key));
    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void ShulkerPreview::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    std::vector<int> combo;
    if (const auto it = section.find("previewKeys"); it != section.end() && it->is_array()) {
        for (const auto& value : *it) {
            if (value.is_number_integer()) {
                combo.push_back(value.get<int>());
            }
        }
    }
    m_previewKey.set(std::move(combo));
}

void ShulkerPreview::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["previewKeys"] = m_previewKey.combo();
}

bool ShulkerPreview::onSlotButton(std::uint32_t, int, const std::string&, int)
{
    m_miniCache.clear();
    {
        const std::lock_guard<std::mutex> lock(m_stacksLock);
        m_slotSets.clear();
    }
    return false;
}

void ShulkerPreview::onScreenTick()
{
    ++m_tick;
    consumeWheel();
    const bool want = wantPreview();
    if (want != m_lastWant) {
        m_lastWant = want;
        cui::requestRefresh();
    }
}

void ShulkerPreview::onScreenLost()
{
    m_currentBox = {};
    m_publishedBox = {};
    m_selectedBox = {};
    m_publishedSet = 0;
    m_selectedSet = 0;
    select(-1);
    m_miniCache.clear();
    freeStacks();
}

void ShulkerPreview::onScreenCreated(void* ctrl)
{
    m_currentBox = {};
    m_publishedBox = {};
    m_selectedBox = {};
    m_publishedSet = 0;
    m_selectedSet = 0;
    select(-1);
    m_miniCache.clear();
    m_idAuxCache.clear();
    m_maxStackCache.clear();
    {
        const std::lock_guard<std::mutex> lock(m_stacksLock);
        m_slotSets.clear();
    }
    if (miniAvailable() && !cui::bindCollectionInt(ctrl, "#tk_sp_mini", &ShulkerPreview::miniIcon, 0)) {
        static bool told = false;
        if (!told) {
            told = true;
            log().warn(L"ShulkerPreview: the full-box icon binding could not be registered");
        }
    }
    if (tooltipAvailable()) {
        int bound = 0;
        bound += cui::bindCollectionInt(ctrl, spreview::kBindCurrent, &ShulkerPreview::currentSlot, 0) ? 1 : 0;
        bound += cui::bindBool(ctrl, spreview::kBindVisible, &ShulkerPreview::gridVisible, 0) ? 1 : 0;
        bound += cui::bindPersistentText(ctrl, spreview::kBindName, spreview::kTextSlotName) ? 1 : 0;
        bound += cui::bindBool(ctrl, spreview::kBindNormal, &ShulkerPreview::normalTooltip, 0) ? 1 : 0;
        for (int i = 0; i < kSlots; ++i) {
            bound += cui::bindBool(ctrl, spreview::selectedBinding(i).c_str(), &ShulkerPreview::cellSelected,
                                   static_cast<std::uintptr_t>(i)) ? 1 : 0;
        }
        bound += cui::bindBool(ctrl, spreview::kBindHasSelected, &ShulkerPreview::hasSelected, 0) ? 1 : 0;
        bound += cui::bindPersistentText(ctrl, spreview::kBindSelectedName, spreview::kTextSlotSelectedName) ? 1 : 0;
        constexpr int kExpected = 2 + 2 + kSlots + 2;
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1, std::memory_order_relaxed) < 3) {
            if (bound == kExpected) {
                log().info(L"ShulkerPreview: the tooltip grid bindings were registered on the screen");
            } else {
                log().warn(L"ShulkerPreview: only {} of {} tooltip grid bindings were registered",
                           bound, kExpected);
            }
        }
    }
}

int ShulkerPreview::idAuxOf(const std::string& name, int aux)
{
    const std::string key = name + '|' + std::to_string(aux);
    if (const auto it = m_idAuxCache.find(key); it != m_idAuxCache.end()) {
        return it->second;
    }
    const void* const item = blocks::itemByName(name);
    const int value = (item != nullptr) ? cui::idAuxOfItem(item, aux) : 0;
    m_idAuxCache.emplace(key, value);
    return value;
}

int ShulkerPreview::maxStackOfName(const std::string& name)
{
    if (const auto it = m_maxStackCache.find(name); it != m_maxStackCache.end()) {
        return it->second;
    }
    const void* const item = blocks::itemByName(name);
    const int max = (item != nullptr) ? cui::maxStackOfItem(item) : 0;
    m_maxStackCache.emplace(name, max);
    return max;
}

int ShulkerPreview::miniIcon(void* ctrl, const std::string& coll, int index, std::uintptr_t)
{
    ShulkerPreview& self = instance();
    if (!self.enabled()) {
        return 0;
    }
    return self.miniIconOf(cui::screenStackOf(ctrl, coll, index));
}

int ShulkerPreview::miniIconHud(void* ctrl, const std::string& coll, int index, std::uintptr_t)
{
    ShulkerPreview& self = instance();
    if (self.m_forgetNames.exchange(false, std::memory_order_acquire)) {
        self.m_idAuxCache.clear();
        self.m_maxStackCache.clear();
    }
    if (!self.enabled()) {
        return 0;
    }
    const int value = self.miniIconOf(cui::hudStackOf(ctrl, coll, index), false);
    if (value != 0 && !self.m_loggedHudMini) {
        self.m_loggedHudMini = true;
        log().info(L"ShulkerPreview: the HUD cell {} #{} gets the corner icon", toUtf16(coll), index);
    }
    return value;
}

void ShulkerPreview::onHudCreated(void* ctrl)
{
    ShulkerPreview& self = instance();
    if (!self.miniAvailable()) {
        return;
    }
    self.m_forgetNames.store(true, std::memory_order_release);
    static std::atomic<int> logs{0};
    const bool bound = cui::bindPersistentCollectionInt(ctrl, "#tk_sp_mini", 0, &ShulkerPreview::miniIconHud, 0);
    if (logs.fetch_add(1, std::memory_order_relaxed) < 4) {
        if (bound) {
            log().info(L"ShulkerPreview: the full-box icon binding was registered on the HUD");
        } else {
            log().warn(L"ShulkerPreview: the full-box icon binding could not be registered on the HUD");
        }
    }
}

int ShulkerPreview::miniIconOf(const void* stack, bool useCache)
{
    if (stack == nullptr || cui::isEmpty(stack)) {
        return 0;
    }
    const bool shulker = cui::itemName(stack).find("shulker_box") != std::string::npos;
    if (!shulker) {
        return 0;
    }
    const void* const root = cui::userDataOf(stack);
    if (root == nullptr) {
        return 0;
    }
    const void* first = nullptr;
    const void* last = nullptr;
    if (!cui::itemsListBounds(root, first, last)) {
        return 0;
    }
    const MiniKey key{reinterpret_cast<std::uintptr_t>(root), reinterpret_cast<std::uintptr_t>(first),
                      reinterpret_cast<std::uintptr_t>(last)};
    if (useCache) {
        if (const auto it = m_miniCache.find(key); it != m_miniCache.end()) {
            return it->second;
        }
    }
    std::vector<cui::NbtItem> items;
    const bool nbtOk = cui::nbtItemsOfTag(root, items);
    if (!nbtOk) {
        return 0;
    }
    std::vector<shulkerfill::Slot> slots;
    slots.reserve(items.size());
    bool allEnchanted = true;
    for (const cui::NbtItem& one : items) {
        slots.push_back(shulkerfill::Slot{one.slot, one.name, one.aux, one.count});
        allEnchanted = allEnchanted && one.enchanted;
    }
    int value = 0;
    const int at = shulkerfill::uniformFull(slots, kSlots, [this](const std::string& n) { return maxStackOfName(n); });
    if (at >= 0) {
        value = idAuxOf(slots[static_cast<std::size_t>(at)].name, slots[static_cast<std::size_t>(at)].aux);
        if (value != 0 && allEnchanted) {
            value |= cui::kGlintBit;
        }
        if (!m_loggedMini) {
            m_loggedMini = true;
            log().info(L"ShulkerPreview: a full shulker box of {} (27 x {}) gets the corner icon",
                       toUtf16(slots.front().name), maxStackOfName(slots.front().name));
        }
    }
    if (!useCache) {
        return value;
    }
    if (m_miniCache.size() >= 256) {
        m_miniCache.clear();
    }
    m_miniCache.emplace(key, value);
    return value;
}

}
