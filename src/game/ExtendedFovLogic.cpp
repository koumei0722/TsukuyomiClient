#include "game/ExtendedFovLogic.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace tsukuyomi::extendedfov {

namespace {

float floatAt(std::span<const std::byte> code, std::size_t at)
{
    float value = 0.0f;
    std::memcpy(&value, code.data() + at, sizeof(value));
    return value;
}

}

bool readRegistration(std::span<const std::byte> code, std::span<const std::byte> values, Registered& out)
{
    out = {};
    if (code.size() < kRegistrationBytes || values.size() != 4 * sizeof(float)) {
        return false;
    }
    const auto at = [&](std::size_t i) { return std::to_integer<unsigned char>(code[i]); };
    if (at(0) != 0xBA || at(1) != 0x2F || at(2) != 0 || at(3) != 0 || at(4) != 0
        || at(0x14) != 0x48 || at(0x15) != 0x8D || at(0x16) != 0x05
        || at(0x1B) != 0x48 || at(0x1C) != 0x89 || at(0x1D) != 0x03
        || at(0x1E) != 0xC7 || at(0x1F) != 0x43 || at(0x20) != 0x20
        || at(0x25) != 0x0F || at(0x26) != 0x28 || at(0x27) != 0x05
        || at(0x2C) != 0x0F || at(0x2D) != 0x11 || at(0x2E) != 0x43 || at(0x2F) != 0x10) {
        return false;
    }
    out.min = floatAt(values, 0);
    out.max = floatAt(values, 4);
    out.delta = floatAt(code, 0x21);
    out.defaultValue = floatAt(values, 12);
    const float initial = floatAt(values, 8);
    return std::isfinite(out.min) && std::isfinite(out.max) && out.min > 0.0f && out.min < out.max
        && out.max < kExtendedMax && out.defaultValue >= out.min && out.defaultValue <= out.max
        && initial == out.defaultValue && out.delta > 0.0f && out.delta < 1.0f;
}

bool isClampSite(std::span<const std::byte> code)
{
    if (code.size() < kClampNextOffset) {
        return false;
    }
    const auto at = [&](std::size_t i) { return std::to_integer<unsigned char>(code[i]); };
    return at(0x0F) == 0xF3 && at(0x10) == 0x0F && at(0x11) == 0x10 && at(0x12) == 0x0D;
}

bool looksLikeFovOption(const float fields[5], const Registered& reg)
{
    const float min = fields[0];
    const float max = fields[1];
    const float value = fields[2];
    const float defaultValue = fields[3];
    const float delta = fields[4];
    return min == reg.min && (max == reg.max || max == kExtendedMax) && defaultValue == reg.defaultValue
        && delta == reg.delta && std::isfinite(value) && value >= reg.min && value <= kExtendedMax;
}

bool shouldRemember(float fov, const Registered& reg)
{
    return std::isfinite(fov) && fov > reg.max && fov <= kExtendedMax;
}

bool restorable(float saved, const Registered& reg)
{
    return shouldRemember(saved, reg);
}

}
