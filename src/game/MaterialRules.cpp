#include "game/MaterialRules.h"

#include <charconv>

namespace tsukuyomi::materialrules {
namespace {
std::string_view bare(std::string_view name)
{
    return name.starts_with("minecraft:") ? name.substr(10) : name;
}
std::string_view trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == text.npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
std::string_view valueOf(const States& states, const char* key)
{
    const auto at = states.find(key);
    return at == states.end() ? std::string_view{} : std::string_view(at->second);
}
int numberOf(const States& states, const char* key)
{
    const auto value = valueOf(states, key);
    int number = 0;
    if (value.empty()) return number;
    std::from_chars(value.data(), value.data() + value.size(), number);
    return number;
}
bool bitOf(const States& states, const char* key)
{
    return valueOf(states, key) == "true" || numberOf(states, key) != 0;
}
std::vector<Item> primary(std::string_view block, const States& states, const Item& picked)
{
    const auto name = bare(block);
    if (name == "air" || name == "structure_void" || name.starts_with("light_block")) return {};
    if (isLiquid(block)) {
        if (valueOf(states, "liquid_depth") != "0") return {};
        return {{name == "water" || name == "flowing_water" ? "minecraft:water_bucket" : "minecraft:lava_bucket", 1}};
    }
    if (bitOf(states, "upper_block_bit") || bitOf(states, "head_piece_bit")) return {};
    std::string item = picked.name.empty() ? std::string(block) : picked.name;
    if (const auto at = item.find("double_slab"); at != item.npos) {
        item.replace(at, 11, "slab");
        return {{std::move(item), 2}};
    }
    int count = 0;
    if (name == "snow_layer") count = numberOf(states, "height") + 1;
    else if (name == "sea_pickle") count = numberOf(states, "cluster_count") + 1;
    else if (name.ends_with("candle")) count = numberOf(states, "candles") + 1;
    else if (name == "turtle_egg") {
        const auto eggs = valueOf(states, "turtle_egg_count");
        count = eggs == "four_egg" ? 4 : eggs == "three_egg" ? 3 : eggs == "two_egg" ? 2 : 1;
    }
    if (count > 0) return {{std::move(item), static_cast<std::size_t>(count)}};
    if (!picked.name.empty()) return {{picked.name, picked.count == 0 ? 1 : picked.count}};
    return {{std::move(item), 1, true}};
}
}

States parseStates(const std::vector<std::string>& lines)
{
    States out;
    for (const auto& line : lines) {
        const auto at = line.find(": ");
        if (at == line.npos) continue;
        const auto key = trim(std::string_view(line).substr(0, at));
        if (!key.empty()) out[std::string(key)] = trim(std::string_view(line).substr(at + 2));
    }
    return out;
}
bool isLiquid(std::string_view name)
{
    name = bare(name);
    return name == "water" || name == "flowing_water" || name == "lava" || name == "flowing_lava";
}
std::vector<Item> itemsFor(std::string_view block, const States& states, const Item& picked,
                          std::string_view layer1, const States& layer1States)
{
    auto out = primary(block, states, picked);
    if (bare(layer1) == "water" && valueOf(layer1States, "liquid_depth") == "0")
        out.push_back({"minecraft:water_bucket", 1});
    return out;
}
}
