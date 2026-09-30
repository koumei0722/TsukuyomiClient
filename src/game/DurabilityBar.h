#pragma once

namespace tsukuyomi {

struct DurabilityBarColor {
    float r;
    float g;
    float b;
};

DurabilityBarColor durabilityBarColor(float ratio);
float durabilityBarWidth(float ratio, float fullWidth);

}
