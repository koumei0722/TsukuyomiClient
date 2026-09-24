#include "modules/ShulkerPreview.h"

#include "config/WriteSwitches.h"
#include "core/Logger.h"
#include "core/Strings.h"
#include "game/BlockRegistry.h"
#include "game/ShulkerFill.h"
#include "game/UiProbe.h"

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
    if (cui::available()) {
        registerUiDefinitions();
    }
    log().info(L"ShulkerPreview: tooltip {} / full-box icons {}", tooltipAvailable() ? L"ready" : L"NOT usable",
               miniAvailable() ? L"ready" : L"NOT usable");
}

void ShulkerPreview::shutdown()
{
    cui::removeObserver(this);
    const std::lock_guard<std::mutex> lock(m_stacksLock);
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
}

MenuItem ShulkerPreview::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(menu::back());
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    MenuItem key = menu::keybind(
        L"Preview key", [this] { return m_previewKey.combo(); },
        [this](std::vector<int> combo) {
            m_previewKey.set(std::move(combo));
            log().info(L"ShulkerPreview: preview key set to {}", m_previewKey.name());
        },
        {});
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
    if (m_addressKeyed.load()) {
        forgetSetKeys();
    }
    return false;
}

void ShulkerPreview::onScreenTick()
{
    m_screenOpen.store(true, std::memory_order_release);
    const bool want = wantPreview();
    if (want != m_lastWant) {
        m_lastWant = want;
        cui::requestRefresh();
    }
}

void ShulkerPreview::onScreenLost()
{
    m_screenOpen.store(false, std::memory_order_release);
    m_miniCache.clear();
    freeStacks();
}

void ShulkerPreview::onScreenCreated(void* ctrl)
{
    m_miniCache.clear();
    m_idAuxCache.clear();
    m_maxStackCache.clear();
    if (miniAvailable() && !cui::bindCollectionInt(ctrl, "#tk_sp_mini", &ShulkerPreview::miniIcon, 0)) {
        static bool told = false;
        if (!told) {
            told = true;
            log().warn(L"ShulkerPreview: the full-box icon binding could not be registered");
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

int ShulkerPreview::miniIconOf(const void* stack)
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
    if (const auto it = m_miniCache.find(key); it != m_miniCache.end()) {
        return it->second;
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
    if (m_miniCache.size() >= 256) {
        m_miniCache.clear();
    }
    m_miniCache.emplace(key, value);
    return value;
}

}
