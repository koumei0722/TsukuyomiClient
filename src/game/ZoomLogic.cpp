#include "game/ZoomLogic.h"

#include <cmath>

namespace tsukuyomi::zoomlogic {
float scaleProjection(float tangent, float factor)
{
    if (!std::isfinite(tangent) || tangent <= 0.0f || !std::isfinite(factor) || factor < 1.0f) {
        return tangent;
    }
    return tangent / factor;
}
}
