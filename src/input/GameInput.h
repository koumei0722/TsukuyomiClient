#pragma once

#include <cstdint>

namespace tsukuyomi::input {

void noteRawMoveBits(std::uint32_t bits);

bool sneakHeldWithin(unsigned long long ms);

inline constexpr std::uint32_t kRawSneakBit = 1u << 0;
inline constexpr unsigned long long kSneakHoldGraceMs = 150;

void holdSneakUntilInput(std::uint64_t inputs);
bool sneakHoldWanted(std::uint64_t inputsSent);
void noteSneakForced(bool forced, std::uint64_t inputsSent);
bool sneakForcedSince(std::uint64_t& inputsSent);

}
