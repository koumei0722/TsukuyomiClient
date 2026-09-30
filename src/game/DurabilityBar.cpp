#include "game/DurabilityBar.h"

#include <algorithm>
#include <cmath>

namespace tsukuyomi {

DurabilityBarColor durabilityBarColor(float ratio)
{
    const float value = std::clamp(ratio, 0.0f, 1.0f);
    return {std::min(1.0f, 2.0f - 2.0f * value), std::min(1.0f, 2.0f * value), 0.0f};
}

float durabilityBarWidth(float ratio, float fullWidth)
{
    return std::round(fullWidth * std::clamp(ratio, 0.0f, 1.0f));
}

}
