#include "game/AppleSkinMath.h"

#include <algorithm>
#include <cmath>

namespace tsukuyomi::appleskin {
float saturationGain(const Food& food)
{
    return static_cast<float>(food.nutrition) * food.saturationModifier * kSaturationScale;
}
bool harmfulEffect(int id)
{
    switch (id) {
    case 2: case 4: case 7: case 9: case 15: case 17: case 18: case 19: case 20:
    case 24: case 25: case 30: case 32: case 33: case 34: case 35: return true;
    default: return false;
    }
}
std::uint8_t saturationState(float fraction)
{
    return fraction >= 1 ? 4 : fraction > 0.5f ? 3 : fraction > 0.25f ? 2 : fraction > 0 ? 1 : 0;
}
float estimatedHealth(const Input& in)
{
    if (!in.hasFood || in.peaceful || in.hunger >= 18 || in.regeneration || in.poison || in.wither
        || in.health <= 0 || in.health >= in.maxHealth || in.maxHealth > 20 || in.heartCount != 10) return in.health;
    float food = std::clamp(in.hunger + static_cast<float>(in.food.nutrition), 0.0f, 20.0f);
    float sat = std::clamp(in.saturation + saturationGain(in.food), 0.0f, food);
    float exh = in.exhaustion;
    float inc = 0;
    if (in.naturalRegen) {
        for (int i = 0; i < 200 && food >= 18 && in.health + inc < in.maxHealth; ++i) {
            inc += 1;
            exh += kHealExhaustion;
            while (exh >= kExhaustionThreshold) {
                exh -= kExhaustionThreshold;
                if (sat > 0) sat = std::max(0.0f, sat - 1);
                else food -= 1;
            }
        }
    }
    for (int i = 0; i < std::clamp(in.food.effectCount, 0, 16); ++i) {
        const Effect& effect = in.food.effects[static_cast<std::size_t>(i)];
        if (effect.id == 10 && effect.duration > 0 && effect.amplifier >= 0) {
            const int interval = effect.amplifier >= 6 ? 1 : std::max(50 >> effect.amplifier, 1);
            inc += static_cast<float>(effect.duration / interval);
        }
    }
    return std::min(in.health + inc, in.maxHealth);
}
States calculate(const Input& in)
{
    States out;
    if (!std::isfinite(in.hunger) || !std::isfinite(in.saturation) || !std::isfinite(in.exhaustion)
        || !std::isfinite(in.health) || !std::isfinite(in.maxHealth)) return out;
    const int hunger = static_cast<int>(std::clamp(in.hunger, 0.0f, 20.0f));
    const float sat = std::clamp(in.saturation, 0.0f, 20.0f);
    out.exhaustionRatio = std::clamp(in.exhaustion / kExhaustionThreshold, 0.0f, 1.0f);
    for (int i = 0; i < kIcons; ++i) out.saturation[i] = saturationState(sat / 2 - static_cast<float>(i));
    if (!in.hasFood || !std::isfinite(in.food.saturationModifier)
        || !(hunger < 20 || in.food.alwaysEat || in.peaceful)) return out;
    const int modH = std::clamp(hunger + in.food.nutrition, 0, 20);
    if (modH > hunger) {
        for (int i = hunger / 2; i < (modH + 1) / 2; ++i) {
            out.hunger[i] = static_cast<std::uint8_t>((i * 2 + 1 == modH ? 2 : 1) + (in.food.rotten ? 2 : 0));
        }
    }
    const float modS = std::clamp(sat + saturationGain(in.food), 0.0f, static_cast<float>(modH));
    if (modS > sat) {
        for (int i = static_cast<int>(sat / 2); i < kIcons; ++i)
            out.gain[i] = saturationState(modS / 2 - static_cast<float>(i));
    }
    const float modHP = estimatedHealth(in);
    if (modHP > in.health) {
        const int start = std::max(0, static_cast<int>(std::ceil(in.health)) / 2);
        const int end = std::min(kIcons, static_cast<int>(std::ceil(modHP / 2)));
        const int fixed = static_cast<int>(std::ceil(modHP));
        for (int i = start; i < end; ++i) out.health[i] = i * 2 + 1 == fixed ? 2 : 1;
    }
    return out;
}
float advanceFlash(Flash& flash, std::uint64_t ticks, float maxAlpha)
{
    ticks %= 32;
    for (std::uint64_t i = 0; i < ticks; ++i) {
        flash.u += static_cast<float>(flash.direction) * 0.125f;
        if (flash.u >= 1.5f) flash.direction = -1;
        if (flash.u <= -0.5f) flash.direction = 1;
    }
    return std::clamp(flash.u, 0.0f, 1.0f) * std::clamp(maxAlpha, 0.0f, 1.0f);
}
std::uint32_t packFood(int nutrition, float gain, bool rotten)
{
    const int scaled = std::isfinite(gain) ? static_cast<int>(std::round(std::clamp(gain, -3276.8f, 3276.7f) * 10)) : 0;
    return (static_cast<std::uint32_t>(std::clamp(nutrition, -128, 127)) & 0xffu)
        | ((static_cast<std::uint32_t>(scaled) & 0xffffu) << 8) | (rotten ? 1u << 24 : 0u);
}
bool unpackFood(std::uint32_t id, int& nutrition, float& gain, bool& rotten)
{
    if ((id & 0xfe000000u) != 0) return false;
    const int n = static_cast<int>(id & 0xffu);
    const int s = static_cast<int>((id >> 8) & 0xffffu);
    nutrition = n >= 128 ? n - 256 : n;
    gain = static_cast<float>(s >= 32768 ? s - 65536 : s) / 10;
    rotten = (id & (1u << 24)) != 0;
    return true;
}
}
