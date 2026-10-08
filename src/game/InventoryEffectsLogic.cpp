#include "game/InventoryEffectsLogic.h"

#include <algorithm>
#include <cmath>

namespace tsukuyomi::invfx {

namespace {

constexpr int kLongDuration = 32147;

int compareInt(int a, int b)
{
    return a < b ? -1 : (a > b ? 1 : 0);
}

int compareFalseFirst(bool a, bool b)
{
    return a == b ? 0 : (a ? 1 : -1);
}

int compareColor(std::uint32_t a, std::uint32_t b)
{
    return a < b ? -1 : (a > b ? 1 : 0);
}

}

int compareLikeJava(const EffectKey& a, const EffectKey& b)
{
    const bool bothLong = a.duration > kLongDuration && b.duration > kLongDuration;
    const bool bothAmbient = a.ambient && b.ambient;
    if (bothLong || bothAmbient) {
        if (const int c = compareFalseFirst(a.ambient, b.ambient); c != 0) return c;
        return compareColor(a.color, b.color);
    }
    if (const int c = compareFalseFirst(a.ambient, b.ambient); c != 0) return c;
    if (const int c = compareFalseFirst(a.duration == -1, b.duration == -1); c != 0) return c;
    if (const int c = compareInt(a.duration, b.duration); c != 0) return c;
    return compareColor(a.color, b.color);
}

int rowStep(int count)
{
    return count > 5 ? 132 / (count - 1) : 33;
}

int rowTop(int index, int count)
{
    return index * rowStep(count);
}

std::uint32_t colorFromFloats(float r, float g, float b)
{
    auto channel = [](float v) -> std::uint32_t {
        if (!std::isfinite(v)) return 0;
        const float scaled = std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f);
        return static_cast<std::uint32_t>(scaled);
    };
    return (channel(r) << 16) | (channel(g) << 8) | channel(b);
}

std::string iconPath(const std::string& name)
{
    return "textures/ui/" + name + "_effect";
}

}
