#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tsukuyomi::appleskin {
inline constexpr int kHungerSlot = 20, kSaturationSlot = 30, kGainSlot = 40, kHealthSlot = 50;
inline constexpr int kFlashSlot = 60, kExhaustionSlot = 61, kExhaustionAlphaSlot = 62, kOnSlot = 220;
inline constexpr int kExhaustionWidth = 81;
struct Textures { std::array<std::string, 4> saturation; std::string exhaustion; };
struct Layout { std::string front, back; };
Layout layoutJson(const Textures& textures);
std::vector<std::uint8_t> saturationPixels(int stage);
std::vector<std::uint8_t> exhaustionPixels();
float exhaustionShown(float ratio);
std::string textureFileName(const std::vector<std::uint8_t>& rgba);
struct TooltipIcon { int x, image; bool faded; };
inline constexpr int kHungerFull = 0, kHungerHalf = 1, kHungerNone = -1;
std::vector<TooltipIcon> tooltipHungerIcons(int nutrition, int bars);
std::vector<TooltipIcon> tooltipSaturationIcons(float gain, int bars);
struct PixelRun { int x, y, width; std::uint32_t rgb; };
std::vector<PixelRun> tooltipSaturationRuns(int image, bool negative);
}
