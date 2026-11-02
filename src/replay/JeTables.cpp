#include "replay/JeTables.h"
#include "replay/JeTables.inc"
#include <algorithm>
#include <cstring>

namespace tsukuyomi::replay::jetables {
namespace {
template<std::size_t N> int pairValue(const unsigned short (&table)[N], int key)
{
    std::size_t lo = 0, hi = N / 2;
    while (lo < hi) {
        const auto mid = (lo + hi) / 2;
        if (table[mid * 2] < key) lo = mid + 1;
        else hi = mid;
    }
    return lo < N / 2 && table[lo * 2] == key ? table[lo * 2 + 1] : -1;
}
struct Dictionary {
    std::vector<std::string_view> keys;
    template<std::size_t N> explicit Dictionary(const unsigned char (&data)[N])
    {
        for (std::size_t i = 0; i < N && data[i] != 0;) {
            const auto* p = reinterpret_cast<const char*>(data + i);
            const auto len = std::strlen(p);
            keys.emplace_back(p, len);
            i += len + 1;
        }
    }
    std::size_t lower(std::string_view key) const
    { return static_cast<std::size_t>(std::lower_bound(keys.begin(), keys.end(), key) - keys.begin()); }
    template<std::size_t N> int exact(std::string_view key, const unsigned short (&values)[N]) const
    {
        const auto i = lower(key);
        return i < keys.size() && i < N && keys[i] == key ? values[i] : -1;
    }
};
const Dictionary& blocks() { static const Dictionary d(jedata::kBedrockKeys); return d; }
}
std::uint8_t stateFlags(int state)
{ return state >= 0 && state < jedata::kStateCount ? jedata::kStateFlags[state] : 0; }
int blockEntityType(int state) { return pairValue(jedata::kBlockEntityPairs, state); }
int waterlogged(int state) { return pairValue(jedata::kWaterloggedPairs, state); }
int bedrockToJava(std::string_view name, const std::vector<std::pair<std::string, std::string>>& states)
{
    auto sorted = states;
    std::sort(sorted.begin(), sorted.end());
    std::string suffix = "|";
    for (const auto& [key, value] : sorted) {
        if (suffix.size() > 1) suffix += ',';
        suffix += key + "=" + value;
    }
    const auto& d = blocks();
    int value = d.exact(std::string(name) + suffix, jedata::kBedrockKeyStates);
    if (value >= 0) return value;
    std::string_view alias;
    if (name == "minecraft:water") alias = "minecraft:flowing_water";
    else if (name == "minecraft:flowing_water") alias = "minecraft:water";
    else if (name == "minecraft:lava") alias = "minecraft:flowing_lava";
    else if (name == "minecraft:flowing_lava") alias = "minecraft:lava";
    if (!alias.empty()) {
        value = d.exact(std::string(alias) + suffix, jedata::kBedrockKeyStates);
        if (value >= 0) return value;
    }
    const auto prefix = std::string(name) + "|";
    std::vector<std::string> tokens;
    for (const auto& [key, val] : sorted) tokens.push_back(key + "=" + val);
    int bestScore = -1;
    for (auto i = d.lower(prefix); i < d.keys.size() && d.keys[i].starts_with(prefix); ++i) {
        int score = 0;
        auto tail = d.keys[i].substr(prefix.size());
        while (!tail.empty()) {
            const auto comma = tail.find(',');
            const auto token = tail.substr(0, comma);
            if (std::find(tokens.begin(), tokens.end(), token) != tokens.end()) ++score;
            if (comma == std::string_view::npos) break;
            tail.remove_prefix(comma + 1);
        }
        if (score > bestScore) { bestScore = score; value = jedata::kBedrockKeyStates[i]; }
    }
    if (bestScore >= 0) return value;
    static const Dictionary names(jedata::kBedrockNames);
    value = names.exact(name, jedata::kBedrockNameStates);
    return value >= 0 ? value : jedata::kStoneState;
}
int biomeOf(std::string_view name)
{
    static const Dictionary d(jedata::kBedrockBiomes);
    return d.exact(name, jedata::kBedrockBiomeIds);
}
std::span<const unsigned char> configPackets() { return jedata::kConfigPackets; }
}
