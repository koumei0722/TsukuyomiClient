#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace tsukuyomi::abilities {

inline constexpr ptrdiff_t kArrayOffset = 0x18;
inline constexpr ptrdiff_t kStride = 0x0C;
inline constexpr int kCount = 20;
inline constexpr ptrdiff_t kLayerStride = kStride * kCount;

inline constexpr int kLayerCount = 6;

inline constexpr int kPlayerLayer = 1;

inline constexpr ptrdiff_t kTypeOffset = 0x0;
inline constexpr ptrdiff_t kValueOffset = 0x4;

inline constexpr std::uint8_t kTypeUnset = 1;
inline constexpr std::uint8_t kTypeBool = 2;
inline constexpr std::uint8_t kTypeFloat = 3;

inline constexpr int kFlying = 9;
inline constexpr int kMayFly = 10;
inline constexpr int kFlySpeed = 13;
inline constexpr int kWalkSpeed = 14;
inline constexpr int kNoClip = 17;
inline constexpr int kVerticalFlySpeed = 19;

bool looksValid(std::byte* layered);

std::byte* at(std::byte* layered, int layer, int index);

std::byte* slotOf(std::byte* layered, int index);

bool readBool(std::byte* layered, int index);
bool setBool(std::byte* layered, int index, bool wanted);

bool readByte(const void* address, std::uint8_t& value);
bool writeByte(void* address, std::uint8_t value);
bool readFloat(const void* address, float& value);
bool writeFloat(void* address, float value);

class RestoreLedger {
public:
    void observeWorld(std::uint64_t world);

    void note(const void* who);

    void markAllDirty();

    void clearDirty();

    bool needsRestore(const void* who);

    void markClean(const void* who);

    void markDirty(const void* who);

    bool allClean() const;

private:
    static constexpr size_t kMax = 8;

    int find(const void* who);

    std::atomic<std::uint64_t> m_world{0};
    std::atomic<const void*> m_who[kMax]{};
    std::atomic<bool> m_dirty[kMax]{};
    std::atomic<std::uint64_t> m_seen[kMax]{};
};

}
