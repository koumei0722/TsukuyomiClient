#include "game/CustomGuiScaleLogic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace tsukuyomi::customguiscale {

int ceilHalf(int mx)
{
    return mx / 2 + (mx > 0 && mx % 2 != 0 ? 1 : 0);
}

int defaultIndex(int d, int mx)
{
    int index = (std::max)(d, ceilHalf(mx));
    if (static_cast<std::uint32_t>(d) > static_cast<std::uint32_t>(mx)) index = mx;
    return std::clamp(index, 0, 7);
}

namespace {
bool valid(float base, float maximum, float minimum)
{
    return std::isfinite(base) && base > 0 && std::isfinite(minimum) && minimum > 0
        && std::isfinite(maximum) && maximum >= minimum;
}
}

float customScale(float base, float maximum, float minimum, int percent)
{
    if (!valid(base, maximum, minimum)) return std::isfinite(minimum) && minimum > 0 ? minimum : 1.0f;
    return static_cast<float>(std::clamp(static_cast<double>(base) * percent / 100.0,
                                         static_cast<double>(minimum), static_cast<double>(maximum)));
}

void percentRange(float base, float maximum, float minimum, int& lo, int& hi)
{
    lo = 50;
    hi = 200;
    if (!valid(base, maximum, minimum)) return;
    const auto bounded = [](double value) {
        return static_cast<int>(std::clamp(value, static_cast<double>((std::numeric_limits<int>::min)()),
                                           static_cast<double>((std::numeric_limits<int>::max)())));
    };
    lo = bounded(std::ceil(100.0 * minimum / base));
    hi = (std::max)(lo, bounded(std::floor(100.0 * maximum / base)));
}

int clampPercent(int percent, int lo, int hi)
{
    return std::clamp(percent, lo, (std::max)(lo, hi));
}

}
