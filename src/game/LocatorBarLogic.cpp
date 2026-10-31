#include "game/LocatorBarLogic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tsukuyomi::locatorbar {
namespace {
constexpr std::uint32_t kMarkMask = 0xFF005AA5;

std::uint32_t channel(float v)
{
    if (!(v > 0.0f)) return 0;
    return static_cast<std::uint32_t>(std::min(v, 1.0f) * 255.0f);
}
}

std::uint32_t packColor(const float* rgba)
{
    return (channel(rgba[3]) << 24) | (channel(rgba[0]) << 16) | (channel(rgba[1]) << 8) | channel(rgba[2]);
}

void markColor(std::uint32_t index, float* rgba)
{
    const auto word=index^kMarkMask;
    rgba[0]=(static_cast<float>((word>>16)&255)+0.5f)/255.0f;
    rgba[1]=(static_cast<float>((word>>8)&255)+0.5f)/255.0f;
    rgba[2]=(static_cast<float>(word&255)+0.5f)/255.0f;
    rgba[3]=(static_cast<float>(word>>24)+0.5f)/255.0f;
}
std::uint32_t decodeMark(std::uint32_t argb) { return argb^kMarkMask; }

std::uint32_t scaleAlpha(std::uint32_t argb, float factor)
{
    const auto alpha = static_cast<std::uint32_t>(static_cast<float>(argb >> 24) * std::clamp(factor, 0.0f, 1.0f));
    return (argb & 0x00FFFFFF) | (alpha << 24);
}

std::string distanceText(float blocks)
{
    if (!(blocks >= 0.0f)) return {};
    char text[24]{};
    const int written = std::snprintf(text, sizeof(text), "%lldm", std::llround(std::min(blocks, 9.9e9f)));
    return written > 0 ? std::string(text, static_cast<std::size_t>(written)) : std::string();
}

}
