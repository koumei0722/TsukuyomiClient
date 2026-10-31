#include "BotCompletion.h"
#include <algorithm>

namespace tsukuyomi::bot {
std::string guestKey(const std::string& host, std::uint8_t sub)
{
    return (host.empty() ? std::string(kUserLabel) : host) + " (" + std::to_string(sub) + ")";
}
bool isGuest(const std::string& name)
{
    const auto n = name.size();
    return n >= 5 && name[n - 1] == ')' && name[n - 3] == '(' && name[n - 4] == ' ' && name[n - 2] >= '1' && name[n - 2] <= '3';
}
std::vector<std::string> accountEnumValues(const std::vector<std::string>& names)
{
    std::vector<std::string> result;
    for (auto name : names) {
        std::transform(name.begin(), name.end(), name.begin(), [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        });
        if (chatcommand::isEnumWord(name)) result.push_back(std::move(name));
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
void addCompletion(chatcommand::TkCommandSpec& spec, std::uint32_t firstEnumIndex, const std::vector<std::string>& accounts,
                   std::optional<std::uint32_t> booleanIndex)
{
    using chatcommand::CompletionParam;
    using chatcommand::ParamType;
    const auto addEnum = [&](const char* name, std::vector<std::string> values) {
        const auto index = firstEnumIndex + static_cast<std::uint32_t>(spec.enums.size());
        spec.enums.push_back({name, std::move(values)});
        return index;
    };
    const auto word = [&](const char* paramName, const char* enumName, const char* value) {
        return CompletionParam{paramName, addEnum(enumName, {value}), false};
    };
    const auto bot = word("bot", "TsukuyomiBot", "bot");
    const CompletionParam plain{"action", addEnum("TsukuyomiBotAction", {"login", "status", "accounts", "leaveall"}), false};
    const auto logout = word("action", "TsukuyomiBotLogout", "logout");
    const auto leave = word("action", "TsukuyomiBotLeave", "leave");
    const auto switching = word("action", "TsukuyomiBotSwitch", "switch");
    const auto join = word("action", "TsukuyomiBotJoin", "join");
    const auto walk = word("action", "TsukuyomiBotWalk", "walk");
    const auto follow = word("mode", "TsukuyomiBotWalkFollow", "follow");
    const CompletionParam walkMode{"mode", addEnum("TsukuyomiBotWalkMode", {"path", "straight"}), true};
    const CompletionParam simple{"action", addEnum("TsukuyomiBotSimpleAction", {"stop", "eat", "use", "inventory"}), false};
    const auto build = word("action", "TsukuyomiBotBuild", "build");
    const CompletionParam face{"face", addEnum("TsukuyomiBotFace", {"down", "up", "north", "south", "west", "east"}), false};
    const auto hotbar = word("action", "TsukuyomiBotHotbar", "hotbar");
    const auto nameIndex = static_cast<std::uint32_t>(spec.softEnums.size());
    spec.softEnums.push_back({kAccountSoftEnum, accountEnumValues(accounts)});
    const CompletionParam name{"name", nameIndex, false, ParamType::SoftEnum};
    const CompletionParam number{"number", 0, false, ParamType::Int};
    const CompletionParam position{"position", 0, false, ParamType::Position};
    const CompletionParam blockPosition{"position", 0, false, ParamType::BlockPosition};
    const CompletionParam slot{"slot", 0, false, ParamType::Int};
    if (!booleanIndex)
        for (std::size_t i = 0; i < spec.enums.size() && !booleanIndex; ++i)
            if (spec.enums[i].name == "Boolean") booleanIndex = firstEnumIndex + static_cast<std::uint32_t>(i);
    if (!booleanIndex) booleanIndex = addEnum("Boolean", {"true", "false"});
    const CompletionParam classic{"classic", *booleanIndex, true};

    auto& o = spec.overloads;
    o.push_back({bot, plain});
    o.push_back({bot, logout, name});
    o.push_back({bot, leave, name, number});
    o.push_back({bot, switching});
    o.push_back({bot, switching, name, number});
    o.push_back({bot, join, name, number, classic});
    o.push_back({bot, walk, name, number, position, walkMode});
    o.push_back({bot, walk, name, number, follow});
    o.push_back({bot, build, name, number, blockPosition, face});
    o.push_back({bot, hotbar, name, number, slot});
    o.push_back({bot, simple, name, number});
    const auto skin = word("skin", "TsukuyomiSkin", "skin");
    o.push_back({skin, word("action", "TsukuyomiSkinReload", "reload")});
    o.push_back({skin, word("action", "TsukuyomiSkinSteal", "steal"), {"gamertag", 0, false, ParamType::Player},
                 {"name", 0, true, ParamType::String}});
}
}
