#include "game/AdvancedTooltipText.h"

#include <cstdio>

namespace tsukuyomi::advancedtooltip {

std::string suffix(std::string_view id, int maxDamage, int damage, std::int32_t dyeColor, int nbtTags)
{
    if (id.empty()) return {};
    std::string text;
    if (dyeColor >= 0) {
        char color[16]{};
        std::snprintf(color, sizeof(color), "#%06X", static_cast<unsigned>(dyeColor) & 0xFFFFFFu);
        text += "\n\xC2\xA7" "7Color: ";
        text += color;
    }
    if (maxDamage > 0 && damage > 0) {
        text += "\n\xC2\xA7" "fDurability: ";
        text += std::to_string(maxDamage - damage);
        text += " / ";
        text += std::to_string(maxDamage);
    }
    text += "\n\xC2\xA7" "8";
    text += id;
    if (nbtTags > 0) {
        text += "\n\xC2\xA7" "8NBT: ";
        text += std::to_string(nbtTags);
        text += " tag(s)";
    }
    return text;
}

}
