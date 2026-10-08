#include "game/ChatCommandComplete.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace tsukuyomi::chatcommand {

bool isEnumWord(std::string_view value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

std::vector<std::string> normalizeItemCandidates(const std::vector<std::string>& names)
{
    constexpr std::string_view prefix = "minecraft:";
    std::vector<std::string> result;
    result.reserve(names.size());
    for (const auto& name : names) {
        std::string_view value = name;
        if (value.starts_with(prefix)) value.remove_prefix(prefix.size());
        if (!value.empty()) result.emplace_back(value);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<std::string> itemEnumValues(const std::vector<std::string>& names)
{
    const auto shortNames = normalizeItemCandidates(names);
    std::vector<std::string> values;
    values.reserve(shortNames.size() * 2);
    for (const auto& name : shortNames) {
        values.push_back(name);
        if (name.find(':') == std::string::npos) values.push_back("minecraft:" + name);
    }
    return values;
}

TkCommandSpec buildTkCommandSpec(std::uint32_t firstEnumIndex, std::optional<std::uint32_t> existingItem,
                                 const std::vector<std::string>& items, const SettingsCompletion& settings,
                                 std::optional<std::uint32_t> existingBoolean)
{
    TkCommandSpec spec;
    spec.enums = {{"TsukuyomiHelp", {"help"}},
                  {"TsukuyomiModule", {"restock"}},
                  {"TsukuyomiRestockItem", {"add", "remove"}},
                  {"TsukuyomiRestockPlain", {"list", "clear"}}};
    std::optional<std::uint32_t> itemIndex = existingItem;
    if (!itemIndex && !items.empty()) {
        itemIndex = firstEnumIndex + static_cast<std::uint32_t>(spec.enums.size());
        spec.enums.push_back({"Item", items});
    }
    spec.overloads.push_back({{"help", firstEnumIndex, true}});
    auto& item = spec.overloads.emplace_back();
    item.push_back({"module", firstEnumIndex + 1, false});
    item.push_back({"action", firstEnumIndex + 2, false});
    if (itemIndex) item.push_back({"item", *itemIndex, true});
    spec.overloads.push_back({{"module", firstEnumIndex + 1, false},
                              {"action", firstEnumIndex + 3, false}});
    if (!settings.modules.empty()) {
        auto addEnum = [&](std::string name, const std::vector<std::string>& values) {
            const auto index = firstEnumIndex + static_cast<std::uint32_t>(spec.enums.size());
            spec.enums.push_back({std::move(name), values});
            return index;
        };
        std::vector<CompletionParam> set;
        set.push_back({"set", addEnum("TsukuyomiSet", {"set"}), false});
        set.push_back({"module", addEnum("TsukuyomiSetModule", settings.modules), false});
        if (!settings.settings.empty()) set.push_back({"setting", addEnum("TsukuyomiSetting", settings.settings), true});
        spec.overloads.push_back(set);
        auto add = [&](const std::vector<std::string>& names, const char* enumName,
                       const char* valueName, ParamType type, const char* valueEnum,
                       const std::vector<std::string>& values, bool stringChoice = false) {
            if (names.empty()) return;
            const auto settingIndex = addEnum(enumName, names);
            const auto valueIndex = type == ParamType::Enum ? addEnum(valueEnum, values) : 0;
            std::vector<CompletionParam> overload{set[0], set[1], {"setting", settingIndex, false},
                                                  {valueName, valueIndex, false, type}};
            spec.overloads.push_back(std::move(overload));
            if (stringChoice) spec.overloads.push_back({set[0], set[1], {"setting", settingIndex, false},
                                                        {"choice", 0, false, ParamType::String}});
        };
        if (!settings.toggle.empty()) {
            const auto settingIndex = addEnum("TsukuyomiToggleSetting", settings.toggle);
            const auto booleanIndex = existingBoolean ? *existingBoolean : addEnum("Boolean", {"true", "false"});
            spec.overloads.push_back({set[0], set[1], {"setting", settingIndex, false}, {"value", booleanIndex, false}});
            spec.overloads.push_back({set[0], set[1], {"setting", settingIndex, false},
                                      {"state", addEnum("TsukuyomiToggleValue", {"toggle"}), false}});
        }
        add(settings.number, "TsukuyomiNumberSetting", "value", ParamType::Float, nullptr, {});
        add(settings.integer, "TsukuyomiIntSetting", "value", ParamType::Int, nullptr, {});
        add(settings.choice, "TsukuyomiChoiceSetting", "choice", ParamType::Enum,
            "TsukuyomiChoiceValue", settings.choiceValues, true);
        if (!settings.key.empty()) {
            add(settings.key, "TsukuyomiKeySetting", "key", ParamType::Enum,
                "TsukuyomiKeyValue", settings.keyValues);
            const auto settingIndex = spec.overloads.back()[2].enumIndex;
            spec.overloads.push_back({set[0], set[1], {"setting", settingIndex, false},
                                      {"binding", 0, false, ParamType::Json}});
        }
        add(settings.action, "TsukuyomiActionSetting", "action", ParamType::Enum,
            "TsukuyomiRunValue", {"run"});
    }
    return spec;
}

}
