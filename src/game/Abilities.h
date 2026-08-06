#pragma once

#include <cstddef>

namespace tsukuyomi::abilities {

inline constexpr ptrdiff_t kArrayOffset = 0x18;
inline constexpr ptrdiff_t kStride = 0x0C;
inline constexpr int kCount = 20;
inline constexpr ptrdiff_t kLayerStride = kStride * kCount;

inline constexpr int kLayerCount = 2;

inline constexpr int kPlayerLayer = 1;

inline constexpr ptrdiff_t kTypeOffset = 0x0;
inline constexpr ptrdiff_t kValueOffset = 0x4;

inline constexpr int kTypeUnset = 1;
inline constexpr int kTypeBool = 2;
inline constexpr int kTypeFloat = 3;

inline constexpr int kFlying = 9;
inline constexpr int kMayFly = 10;
inline constexpr int kFlySpeed = 13;
inline constexpr int kWalkSpeed = 14;
inline constexpr int kNoClip = 17;
inline constexpr int kVerticalFlySpeed = 19;

inline constexpr float kDefaultFlySpeed = 0.05f;
inline constexpr float kDefaultVerticalFlySpeed = 1.0f;

std::byte* fromContext(void* context);

bool looksValid(std::byte* layered);

std::byte* at(std::byte* layered, int layer, int index);

std::byte* slotOf(std::byte* layered, int index);

bool readInt(const void* address, int& value);
bool writeInt(void* address, int value);
bool readFloat(const void* address, float& value);
bool writeFloat(void* address, float value);

}
