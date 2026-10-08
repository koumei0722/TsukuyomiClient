#pragma once

#include <cstdint>
#include <string>

namespace tsukuyomi::invfx {

struct EffectKey {
    int id = 0;
    int duration = 0;
    bool ambient = false;
    std::uint32_t color = 0;
};

int compareLikeJava(const EffectKey& a, const EffectKey& b);

int rowStep(int count);
int rowTop(int index, int count);

inline constexpr int kBoxHeight = 32;
inline constexpr int kIconSize = 18;
inline constexpr int kIconInset = 7;
inline constexpr int kTextX = 32;
inline constexpr int kSpacing = 7;
inline constexpr int kMinSpace = 32;
inline constexpr int kFullSpace = 120;
inline constexpr int kGap = 2;

std::uint32_t colorFromFloats(float r, float g, float b);

std::string iconPath(const std::string& name);

}
