#include "modules/FastBlockPlacementLogic.h"

#include <algorithm>
#include <cstdlib>

namespace tsukuyomi::fastplace {
namespace {

int axisOf(int side)
{
    return side < 2 ? 1 : side < 4 ? 2 : 0;
}

std::int64_t delta(const std::int32_t a[3], const std::int32_t b[3], int axis)
{
    return static_cast<std::int64_t>(a[axis]) - b[axis];
}

}

bool allowed(Restriction mode, int layerHeight, const std::int32_t pos[3], int side,
             const std::int32_t first[3], int sideFirst)
{
    if (sideFirst < 0 || sideFirst > 5) return false;
    const int axis = axisOf(sideFirst);
    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
    const std::int64_t du = delta(pos, first, u), dv = delta(pos, first, v);
    const bool onPlane = pos[axis] == first[axis];
    switch (mode) {
    case Restriction::Plane:
        return onPlane;
    case Restriction::Face:
        return side == sideFirst;
    case Restriction::Column:
        return du == 0 && dv == 0;
    case Restriction::Line:
        return onPlane && (du == 0 || dv == 0);
    case Restriction::Diagonal:
        return onPlane && std::llabs(du) == std::llabs(dv);
    case Restriction::Layer: {
        const std::int64_t dy = delta(pos, first, 1);
        if (layerHeight > 0) return dy + 1 >= 1 && dy + 1 <= layerHeight;
        if (layerHeight < 0) return -dy + 1 >= 1 && -dy + 1 <= -static_cast<std::int64_t>(layerHeight);
        return true;
    }
    }
    return false;
}

void supportClick(const float inPlaced[3], int sideFirst, float out[3])
{
    for (int i = 0; i < 3; ++i) out[i] = inPlaced[i];
    if (sideFirst >= 0 && sideFirst <= 5) out[axisOf(sideFirst)] += (sideFirst & 1) ? 1.0F : -1.0F;
    for (int i = 0; i < 3; ++i) out[i] = std::clamp(out[i], 0.0F, 1.0F);
}

}
