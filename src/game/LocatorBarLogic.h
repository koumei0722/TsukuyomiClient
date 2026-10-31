#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tsukuyomi::locatorbar {

std::uint32_t packColor(const float* rgba);

void markColor(std::uint32_t index, float* rgba);
std::uint32_t decodeMark(std::uint32_t argb);
std::uint32_t scaleAlpha(std::uint32_t argb, float factor);

std::string distanceText(float blocks);

}
