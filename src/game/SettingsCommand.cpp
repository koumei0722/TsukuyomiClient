#include "game/SettingsCommand.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <mutex>
#include <utility>

#include "game/ChatCommand.h"
#include "game/SettingsCommandLogic.h"
#include "game/UiProbe.h"
#include "modules/ModuleManager.h"
#include "config/Config.h"
#include "core/Logger.h"
#include "core/Strings.h"
#include "input/PadKeys.h"

namespace tsukuyomi::settingscommand {
namespace {
struct Extra {
    std::wstring label;
    std::function<std::vector<MenuItem>()> factory;
};
std::vector<Extra> g_extras;
std::mutex g_indexMutex;
std::vector<ModuleInfo> g_index;
std::atomic<bool> g_padDirty{false};

std::string sectionName(const MenuItem& item, std::size_t index)
{
    const auto& modules = ModuleManager::instance().modules();
    const auto label = item.labelText();
    return toUtf8(index < modules.size() && label != modules[index]->name() ? modules[index]->name() : label);
}

std::vector<MenuItem> buildTree()
{
    auto tree = ModuleManager::instance().buildMenuItems();
    for (auto& module : tree) {
        for (const auto& extra : g_extras) {
            if (module.labelText() != extra.label) continue;
            auto children = extra.factory();
            module.children.insert(module.children.end(), std::make_move_iterator(children.begin()),
                                   std::make_move_iterator(children.end()));
        }
    }
    return tree;
}

void cacheIndex(std::vector<ModuleInfo> index)
{
    std::lock_guard lock(g_indexMutex);
    g_index = std::move(index);
}

}

void addExtra(std::wstring moduleLabel, std::function<std::vector<MenuItem>()> factory)
{
    g_extras.push_back({std::move(moduleLabel), std::move(factory)});
}

chatcommand::SettingsCompletion completion()
{
    auto index = buildIndex(buildTree());
    auto result = completionOf(index);
    cacheIndex(std::move(index));
    return result;
}

void registerCommand()
{
    cacheIndex(buildIndex(buildTree()));
    chatcommand::registerRawCommand({"set"}, "/tk set <module> <setting> <value>",
        [](const auto&, const auto& rawArgs) {
            const auto tree = buildTree();
            auto result = run(tree, rawArgs);
            if (result.changed || result.keysChanged || result.padChanged) uiprobe::markSettingsDirty();
            if (result.padChanged && result.changed) g_padDirty.store(true, std::memory_order_release);
            if (result.keysChanged) uiprobe::syncOwnKeyRows();
            cacheIndex(buildIndex(buildTree()));
            return chatcommand::Reply{std::move(result.lines), result.error};
        },
        [](const auto& words, std::size_t argument, std::string_view suggestion) {
            std::lock_guard lock(g_indexMutex);
            return allowSuggestion(g_index, words, argument, suggestion);
        }, true);
}

void loadPadKeys()
{
    const auto tree = buildTree();
    unsigned warnings = 0;
    auto warn = [&warnings](const std::string& section, const std::string& name) {
        g_padDirty.store(true, std::memory_order_release);
        if (warnings++ < 8) log().warn(L"Controller binding ignored: {}.padKeys.{}", toUtf16(section), toUtf16(name));
    };
    for (std::size_t i = 0; i < tree.size(); ++i) {
        const auto key = sectionName(tree[i], i);
        const auto& section = Config::instance().section(key);
        const auto saved = section.find("padKeys");
        if (saved == section.end()) continue;
        if (!saved->is_object()) { warn(key, "(invalid object)"); continue; }
        const auto settings = settingsOf(tree[i], true);
        for (const auto& entry : saved->items()) {
            const auto ref = std::find_if(settings.begin(), settings.end(), [&entry](const SettingRef& s) { return s.name == entry.key(); });
            std::vector<int> combo;
            if (ref == settings.end() || ref->item->kind != MenuItemKind::Keybind || !ref->item->setPadKeys
                || !entry.value().is_string() || !padkeys::parseCombo(entry.value().get<std::string>(), combo)) {
                warn(key, entry.key());
                continue;
            }
            ref->item->setPadKeys(std::move(combo));
        }
    }
}

void savePadKeys()
{
    if (!g_padDirty.exchange(false, std::memory_order_acq_rel)) return;
    const auto tree = buildTree();
    for (std::size_t i = 0; i < tree.size(); ++i) {
        auto saved = nlohmann::json::object();
        for (const auto& ref : settingsOf(tree[i], true)) {
            if (ref.item->kind != MenuItemKind::Keybind || !ref.item->getPadKeys) continue;
            const auto combo = ref.item->getPadKeys();
            if (!combo.empty()) saved[ref.name] = padkeys::comboName(combo);
        }
        auto& section = Config::instance().section(sectionName(tree[i], i));
        if (saved.empty()) section.erase("padKeys");
        else section["padKeys"] = std::move(saved);
    }
}

}
