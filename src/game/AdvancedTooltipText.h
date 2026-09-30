#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tsukuyomi::advancedtooltip {

std::string suffix(std::string_view id, int maxDamage, int damage, std::int32_t dyeColor = -1, int nbtTags = 0);

}
