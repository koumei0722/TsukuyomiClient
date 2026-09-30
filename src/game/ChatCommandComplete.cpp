#include "game/ChatCommandComplete.h"

#include <algorithm>
#include <string_view>

namespace tsukuyomi::chatcommand {

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
                                 const std::vector<std::string>& items)
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
    return spec;
}

}
