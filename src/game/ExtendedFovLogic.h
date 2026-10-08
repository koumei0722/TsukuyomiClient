#pragma once

#include <cstddef>
#include <span>

namespace tsukuyomi::extendedfov {

inline constexpr float kExtendedMax = 150.0f;

struct Registered {
    float min = 0.0f;
    float max = 0.0f;
    float defaultValue = 0.0f;
    float delta = 0.0f;
};

bool readRegistration(std::span<const std::byte> code, Registered& out);

inline constexpr std::size_t kClampDispOffset = 0x13;
inline constexpr std::size_t kClampNextOffset = 0x17;
bool isClampSite(std::span<const std::byte> code);

bool looksLikeFovOption(const float fields[5], const Registered& reg);

bool shouldRemember(float fov, const Registered& reg);

bool restorable(float saved, const Registered& reg);

}
