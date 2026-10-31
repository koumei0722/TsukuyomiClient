#include "game/SkyFogColor.h"

namespace tsukuyomi::skyfog {

bool overworldColor(bool active, int dimension, std::uint32_t kind, bool present, float (&rgb)[3])
{
    if (!active || (dimension != 1 && dimension != 2) || kind != 0 || !present) return false;
    rgb[0] = 171.0f / 255.0f;
    rgb[1] = 210.0f / 255.0f;
    rgb[2] = 1.0f;
    return true;
}

}
