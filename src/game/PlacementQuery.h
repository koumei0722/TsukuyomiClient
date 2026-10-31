#pragma once
#include <cstdint>

namespace tsukuyomi::placementquery {
struct HeldBlock { const void* type = nullptr; int itemValue = 0; };
bool initialize();
bool readHeld(const void* stack, HeldBlock& out);
bool query(const HeldBlock& held, const void* actor, const std::int32_t pos[3],
           std::uint8_t face, const float click[3], const void*& out);
}
