#pragma once

#include <array>
#include <cstdint>

namespace tsukuyomi::appleskin {

inline constexpr float kSaturationScale = 2.0f;
inline constexpr float kHealExhaustion = 6.0f;
inline constexpr float kExhaustionThreshold = 4.0f;
inline constexpr int kIcons = 10;
struct Effect { int id = 0; int duration = 0; int amplifier = 0; float chance = 0; };
struct Food {
    int nutrition = 0;
    float saturationModifier = 0;
    bool alwaysEat = false;
    bool rotten = false;
    std::array<Effect, 16> effects{};
    int effectCount = 0;
};
struct Input {
    float hunger = 0, saturation = 0, exhaustion = 0, health = 0, maxHealth = 20;
    Food food;
    bool hasFood = false, peaceful = false, naturalRegen = true;
    bool regeneration = false, poison = false, wither = false;
    int heartCount = 0;
};
struct States {
    std::array<std::uint8_t, kIcons> hunger{}, saturation{}, gain{}, health{};
    float exhaustionRatio = 0;
};
float saturationGain(const Food& food);
bool harmfulEffect(int id);
std::uint8_t saturationState(float fraction);
float estimatedHealth(const Input& input);
States calculate(const Input& input);
struct Flash { float u = 0; int direction = 1; };
float advanceFlash(Flash& flash, std::uint64_t ticks, float maxAlpha);

std::uint32_t packFood(int nutrition, float gain, bool rotten);
bool unpackFood(std::uint32_t id, int& nutrition, float& gain, bool& rotten);
}
