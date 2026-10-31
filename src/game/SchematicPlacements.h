#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "game/SchematicCells.h"

namespace tsukuyomi::schematic {

enum class Mirror : int { None = 0, X = 1, Z = 2 };
struct PlacementSpec {
    std::uint64_t id = 0;
    std::wstring name, schematic;
    int x = 0, y = 0, z = 0;
    int rotation = 0;
    Mirror mirror = Mirror::None;
    bool enabled = true, render = true, locked = false;
    int color = 0;
    GridSpec grid;
    bool operator==(const PlacementSpec&) const = default;
};
inline constexpr float kPlacementColors[8][4] = {
    {1.0F,0.25F,0.25F,1.0F}, {0.25F,1.0F,0.25F,1.0F},
    {0.3F,0.5F,1.0F,1.0F}, {1.0F,0.85F,0.2F,1.0F},
    {0.9F,0.25F,1.0F,1.0F}, {0.2F,1.0F,1.0F,1.0F},
    {1.0F,0.55F,0.15F,1.0F}, {0.7F,0.7F,0.7F,1.0F}
};
struct PlacementFile {
    std::string kind, levelId, name;
    std::uint64_t selected = 0, nextId = 1;
    std::vector<PlacementSpec> placements;
};
nlohmann::json toJson(const PlacementSpec& spec);
bool fromJson(const nlohmann::json& json, PlacementSpec& spec);
nlohmann::json toJson(const PlacementFile& file);
bool fromJson(const nlohmann::json& json, PlacementFile& file);
bool migrateLegacy(const nlohmann::json& blueprints, PlacementFile& file,
                   bool& placementsMigrated);
Box placementBox(const PlacementSpec& spec, int sizeX, int sizeY, int sizeZ);

}
