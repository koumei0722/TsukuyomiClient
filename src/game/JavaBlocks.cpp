#include "game/JavaBlocks.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <iterator>

#include "game/JavaBlockTable.inc"

namespace tsukuyomi::javablocks {

namespace {

std::string_view stripNamespace(std::string_view name)
{
    constexpr std::string_view prefix = "minecraft:";
    return name.starts_with(prefix) ? name.substr(prefix.size()) : name;
}

const table::JavaBlockRow* findRow(std::string_view name)
{
    const auto* const begin = std::begin(table::kJavaBlocks);
    const auto* const end = std::end(table::kJavaBlocks);
    const auto* const at = std::lower_bound(begin, end, name, [](const table::JavaBlockRow& row, std::string_view want) {
        return std::string_view{row.name} < want;
    });
    return at != end && std::string_view{at->name} == name ? at : nullptr;
}

std::string_view renamed(std::string_view name)
{
    static constexpr std::pair<std::string_view, std::string_view> kRenames[] = {
        {"grass", "short_grass"},
        {"grass_path", "dirt_path"},
        {"sign", "oak_sign"},
        {"wall_sign", "oak_wall_sign"},
        {"chain", "iron_chain"},
    };
    for (const auto& [from, to] : kRenames)
        if (name == from) return to;
    return {};
}

struct Property {
    std::string_view name;
    int stride = 0;
    std::vector<std::string_view> values;
};

std::vector<Property> parseProperties(std::string_view text)
{
    std::vector<Property> out;
    while (!text.empty()) {
        const auto bar = text.find('|');
        const std::string_view one = text.substr(0, bar);
        text = bar == std::string_view::npos ? std::string_view{} : text.substr(bar + 1);
        const auto colon = one.find(':');
        const auto equal = one.find('=');
        if (colon == std::string_view::npos || equal == std::string_view::npos || equal < colon) continue;
        Property p;
        p.name = one.substr(0, colon);
        const std::string_view stride = one.substr(colon + 1, equal - colon - 1);
        std::from_chars(stride.data(), stride.data() + stride.size(), p.stride);
        std::string_view values = one.substr(equal + 1);
        while (!values.empty()) {
            const auto comma = values.find(',');
            p.values.push_back(values.substr(0, comma));
            values = comma == std::string_view::npos ? std::string_view{} : values.substr(comma + 1);
        }
        out.push_back(std::move(p));
    }
    return out;
}

int indexOf(const Property& p, std::string_view value)
{
    const auto at = std::find(p.values.begin(), p.values.end(), value);
    return at == p.values.end() ? -1 : static_cast<int>(at - p.values.begin());
}

structure::PaletteEntry parseBedrock(std::string_view text)
{
    structure::PaletteEntry entry;
    const auto bar = text.find('|');
    entry.name = "minecraft:" + std::string{text.substr(0, bar)};
    text = bar == std::string_view::npos ? std::string_view{} : text.substr(bar + 1);
    while (!text.empty()) {
        const auto next = text.find('|');
        const std::string_view one = text.substr(0, next);
        text = next == std::string_view::npos ? std::string_view{} : text.substr(next + 1);
        const auto equal = one.find('=');
        if (equal == std::string_view::npos || equal + 1 >= one.size()) continue;
        structure::StateValue state;
        state.name = std::string{one.substr(0, equal)};
        const std::string_view value = one.substr(equal + 2);
        if (one[equal + 1] == 's') {
            state.tag = nbt::Tag::String;
            state.text = std::string{value};
        } else {
            state.tag = nbt::Tag::Int;
            std::from_chars(value.data(), value.data() + value.size(), state.number);
        }
        entry.states.push_back(std::move(state));
    }
    std::sort(entry.states.begin(), entry.states.end(),
              [](const structure::StateValue& a, const structure::StateValue& b) { return a.name < b.name; });
    return entry;
}

bool alwaysInWater(std::string_view name)
{
    return name == "kelp" || name == "kelp_plant" || name == "seagrass" || name == "tall_seagrass"
           || name == "bubble_column";
}

}

Converted toBedrock(std::string_view name, const std::vector<std::pair<std::string, std::string>>& properties)
{
    Converted out;
    std::string_view bare = stripNamespace(name);
    const table::JavaBlockRow* row = findRow(bare);
    if (row == nullptr) {
        const std::string_view now = renamed(bare);
        if (!now.empty()) {
            bare = now;
            row = findRow(bare);
        }
    }
    if (row == nullptr) return out;

    const std::vector<Property> props = parseProperties(row->properties);
    std::vector<int> chosen(props.size(), 0);
    int rest = static_cast<int>(row->defaultState) - static_cast<int>(row->base);
    std::vector<std::size_t> order(props.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return props[a].stride > props[b].stride; });
    for (const std::size_t i : order) {
        if (props[i].stride <= 0) continue;
        chosen[i] = rest / props[i].stride;
        rest %= props[i].stride;
    }

    for (const auto& [key, value] : properties) {
        const auto p = std::find_if(props.begin(), props.end(), [&](const Property& one) { return one.name == key; });
        if (p == props.end()) {
            ++out.droppedProperties;
            continue;
        }
        int at = indexOf(*p, value);
        if (at < 0 && (value == "true" || value == "false") && indexOf(*p, "low") >= 0)
            at = indexOf(*p, value == "true" ? "low" : "none");
        if (at < 0) {
            ++out.droppedProperties;
            continue;
        }
        chosen[static_cast<std::size_t>(p - props.begin())] = at;
    }

    int state = row->base;
    for (std::size_t i = 0; i < props.size(); ++i) {
        state += chosen[i] * props[i].stride;
        if (props[i].name == "waterlogged" && props[i].values[static_cast<std::size_t>(chosen[i])] == "true")
            out.waterlogged = true;
    }
    if (alwaysInWater(bare)) out.waterlogged = true;
    if (state < 0 || static_cast<std::size_t>(state) >= std::size(table::kJavaToBedrock)) return out;
    const auto bedrock = table::kJavaToBedrock[static_cast<std::size_t>(state)];
    if (bedrock >= std::size(table::kBedrockStates)) return out;
    out.entry = parseBedrock(table::kBedrockStates[bedrock]);
    out.known = true;
    return out;
}

structure::PaletteEntry bedrockWater()
{
    Converted water = toBedrock("water", {});
    return water.entry;
}

}
