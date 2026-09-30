#pragma once

#include <cstdint>

namespace tsukuyomi::input {

void noteRawMoveBits(std::uint32_t bits);

bool sneakHeldWithin(unsigned long long ms);

inline constexpr std::uint32_t kRawSneakBit = 1u << 0;
inline constexpr unsigned long long kSneakHoldGraceMs = 150;

}
