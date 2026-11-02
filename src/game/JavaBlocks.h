#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game/Structure.h"

namespace tsukuyomi::javablocks {

struct Converted {
    structure::PaletteEntry entry;
    bool waterlogged = false;
    bool known = false;
    int droppedProperties = 0;
};

Converted toBedrock(std::string_view name, const std::vector<std::pair<std::string, std::string>>& properties);

structure::PaletteEntry bedrockWater();

}
