#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace tsukuyomi::sessiontick {

inline constexpr std::size_t kReturnOffset = 9, kFinishOffset = 0x42;

inline bool siteLooksRight(std::span<const std::uint8_t> code, bool catchup)
{
    constexpr std::uint8_t kBody[] = {
        0x49,0x8B,0x4E,0x40,0xE8,0,0,0,0,0x90,0x49,0x8B,0x4E,0x48,0x48,0x85,0xC9,0x74,0x13,
        0x48,0x8B,0x41,0x10,0x48,0x83,0xC1,0x10,0x48,0x8B,0x40,0x20,0xFF,0x15,0,0,0,0,0x90,
        0x49,0x8B,0x4E,0x50,0x48,0x85,0xC9,0x74,0x13,0x48,0x8B,0x01,0x48,0x8B,0x40,0x20,
        0xFF,0x15,0,0,0,0,0x90,0x0F,0x1F,0x44,0,0
    };
    static_assert(sizeof(kBody) == kFinishOffset);
    constexpr std::uint8_t kFirstEnd[] = {0x48,0x89,0xD9,0xE8};
    constexpr std::uint8_t kCatchupEnd[] = {0x4C,0x8B,0xBF,0x98,0,0,0,0x4D,0x85,0xFF};
    if (code.size() < kFinishOffset + (catchup ? sizeof(kCatchupEnd) : 8)) return false;
    for (std::size_t i = 0; i < sizeof(kBody); ++i) {
        if ((i >= 5 && i < 9) || (i >= 0x21 && i < 0x25) || (i >= 0x38 && i < 0x3C)) continue;
        const auto expected = catchup && (i == 2 || i == 12 || i == 40) ? 0x4F : kBody[i];
        if (code[i] != expected) return false;
    }
    const auto end = catchup ? std::span<const std::uint8_t>(kCatchupEnd) : std::span<const std::uint8_t>(kFirstEnd);
    for (std::size_t i = 0; i < end.size(); ++i)
        if (code[kFinishOffset + i] != end[i]) return false;
    return true;
}

}
