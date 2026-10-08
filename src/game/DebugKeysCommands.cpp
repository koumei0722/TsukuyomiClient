#include "game/DebugKeysCommands.h"

#include <charconv>
#include <cmath>
#include <string_view>

namespace tsukuyomi::debugkeys {

namespace {

bool validPosition(double value)
{
    return std::isfinite(value);
}

std::string fixed2(double value)
{
    char text[512]{};
    const auto result = std::to_chars(text, text + sizeof(text), value, std::chars_format::fixed, 2);
    return result.ec == std::errc{} ? std::string(text, result.ptr) : std::string{};
}

bool integerValue(std::string_view value)
{
    if (value.empty()) {
        return false;
    }
    int parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size();
}

void appendQuoted(std::string& out, std::string_view value)
{
    out.push_back('"');
    for (char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
}

}

std::string locationCommand(int dimensionId, double x, double y, double z,
                            double yaw, double pitch)
{
    constexpr std::string_view dimensions[] = {"overworld", "nether", "the_end"};
    if (dimensionId < 0 || dimensionId >= 3 || !validPosition(x) || !validPosition(y)
        || !validPosition(z) || !validPosition(yaw) || !validPosition(pitch)) {
        return {};
    }
    return "/execute in " + std::string(dimensions[dimensionId]) + " run tp @s "
           + fixed2(x) + " " + fixed2(y) + " " + fixed2(z) + " "
           + fixed2(yaw) + " " + fixed2(pitch);
}

std::string setblockCommand(int x, int y, int z, std::string_view blockName,
                            std::span<const std::string_view> states)
{
    if (blockName.empty()) {
        return {};
    }
    std::string out = "/setblock " + std::to_string(x) + " " + std::to_string(y) + " "
                      + std::to_string(z) + " " + std::string(blockName);
    bool first = true;
    for (const std::string_view state : states) {
        const std::size_t split = state.find(": ");
        if (split == std::string_view::npos || split == 0 || split + 2 == state.size()) {
            continue;
        }
        if (first) {
            out += " [";
            first = false;
        } else {
            out.push_back(',');
        }
        appendQuoted(out, state.substr(0, split));
        out.push_back('=');
        const std::string_view value = state.substr(split + 2);
        if (value == "true" || value == "false" || integerValue(value)) {
            out.append(value);
        } else {
            appendQuoted(out, value);
        }
    }
    if (!first) {
        out.push_back(']');
    }
    return out;
}

std::string summonCommand(std::string_view entityName, double x, double y, double z)
{
    if (entityName.empty() || !validPosition(x) || !validPosition(y) || !validPosition(z)) {
        return {};
    }
    return "/summon " + std::string(entityName) + " " + fixed2(x) + " "
           + fixed2(y) + " " + fixed2(z);
}

}
