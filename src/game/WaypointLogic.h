#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace tsukuyomi::waypoints {
struct Waypoint {
    std::string name;
    std::uint32_t rgb = 0xFFFFFF;
    int x = 0, y = 0, z = 0;
    std::string dimension;
    bool visible = true;
};
struct PaletteEntry { const char* name; const char* code; std::uint32_t rgb; };
const PaletteEntry* palette(std::size_t& count);
bool parseColor(std::string_view text, std::uint32_t& rgb);
std::string formatColor(std::uint32_t rgb);
std::string dimensionName(int minY, int maxYExclusive);
nlohmann::json toJson(const std::vector<Waypoint>& list);
bool fromJson(const nlohmann::json& json, std::vector<Waypoint>& out);
std::string defaultName(const std::vector<Waypoint>& list);
bool parseCoordinate(std::string_view text, int& out);
}
