#include "game/WaypointLogic.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <nlohmann/json.hpp>

namespace tsukuyomi::waypoints {
namespace {
constexpr PaletteEntry kPalette[]{
    {"Black", "§0", 0x000000}, {"Dark Blue", "§1", 0x0000AA},
    {"Dark Green", "§2", 0x00AA00}, {"Dark Aqua", "§3", 0x00AAAA},
    {"Dark Red", "§4", 0xAA0000}, {"Dark Purple", "§5", 0xAA00AA},
    {"Gold", "§6", 0xFFAA00}, {"Gray", "§7", 0xAAAAAA},
    {"Dark Gray", "§8", 0x555555}, {"Blue", "§9", 0x5555FF},
    {"Green", "§a", 0x55FF55}, {"Aqua", "§b", 0x55FFFF},
    {"Red", "§c", 0xFF5555}, {"Light Purple", "§d", 0xFF55FF},
    {"Yellow", "§e", 0xFFFF55}, {"White", "§f", 0xFFFFFF}
};
std::string normalized(std::string_view text)
{
    std::string out;
    for (unsigned char c : text) if (!std::isspace(c) && c != '_') out += static_cast<char>(std::tolower(c));
    return out;
}
}
const PaletteEntry* palette(std::size_t& count) { count = std::size(kPalette); return kPalette; }
bool parseColor(std::string_view text, std::uint32_t& rgb)
{
    const auto name = normalized(text);
    for (const auto& entry : kPalette) if (name == normalized(entry.name)) { rgb = entry.rgb; return true; }
    const bool hash = !text.empty() && text.front() == '#';
    if (hash) text.remove_prefix(1);
    if (text.size() != 6 && !(hash && text.size() == 3)) return false;
    std::uint32_t value = 0;
    for (char c : text) {
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return false;
        value = text.size() == 3 ? (value << 8) | static_cast<unsigned>(digit * 17) : (value << 4) | static_cast<unsigned>(digit);
    }
    rgb = value;
    return true;
}
std::string formatColor(std::uint32_t rgb)
{
    char text[8]{};
    std::snprintf(text, sizeof(text), "#%06X", static_cast<unsigned>(rgb & 0xFFFFFF));
    return text;
}
std::string dimensionName(int minY, int maxYExclusive)
{
    if (minY == -64 && maxYExclusive == 320) return "overworld";
    if (minY == 0 && maxYExclusive == 128) return "nether";
    if (minY == 0 && maxYExclusive == 256) return "the_end";
    return "h" + std::to_string(minY) + "_" + std::to_string(maxYExclusive);
}
nlohmann::json toJson(const std::vector<Waypoint>& list)
{
    auto rows = nlohmann::json::array();
    for (const auto& w : list) rows.push_back({{"name", w.name}, {"color", formatColor(w.rgb)}, {"x", w.x}, {"y", w.y}, {"z", w.z}, {"dimension", w.dimension}, {"visible", w.visible}});
    return {{"version", 1}, {"waypoints", std::move(rows)}};
}
bool fromJson(const nlohmann::json& json, std::vector<Waypoint>& out)
{
    if (!json.is_object() || !json.contains("version") || !json["version"].is_number_integer() || json["version"] != 1 ||
        !json.contains("waypoints") || !json["waypoints"].is_array()) return false;
    std::vector<Waypoint> list;
    for (const auto& row : json["waypoints"]) {
        if (!row.is_object()) continue;
        bool valid = true;
        for (const char* key : {"name", "color", "dimension"}) if (!row.contains(key) || !row[key].is_string()) valid = false;
        for (const char* key : {"x", "y", "z"}) if (!row.contains(key) || !row[key].is_number_integer()) valid = false;
        if (!row.contains("visible") || !row["visible"].is_boolean() || !valid) continue;
        Waypoint w;
        if (!parseColor(row["color"].get<std::string>(), w.rgb)) continue;
        if (!parseCoordinate(row["x"].dump(), w.x) || !parseCoordinate(row["y"].dump(), w.y) || !parseCoordinate(row["z"].dump(), w.z)) continue;
        w.name = row["name"].get<std::string>(); w.dimension = row["dimension"].get<std::string>(); w.visible = row["visible"].get<bool>();
        list.push_back(std::move(w));
    }
    out = std::move(list);
    return true;
}
std::string defaultName(const std::vector<Waypoint>& list)
{
    for (std::size_t n = 1;; ++n) {
        auto name = "Waypoint " + std::to_string(n);
        if (std::none_of(list.begin(), list.end(), [&](const auto& w) { return w.name == name; })) return name;
    }
}
bool parseCoordinate(std::string_view text, int& out)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    if (text.empty()) return false;
    if (text.front() == '+') { text.remove_prefix(1); if (text.empty() || text.front() == '-' || text.front() == '+') return false; }
    int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value < -30000000 || value > 30000000) return false;
    out = value;
    return true;
}
}
