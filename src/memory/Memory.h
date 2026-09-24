#pragma once

#include <cstddef>
#include <cstdint>

namespace tsukuyomi::memory {

constexpr bool plausiblePointer(std::uintptr_t value)
{
    constexpr std::uintptr_t kLowest = 0x10000;
    constexpr std::uintptr_t kUserLimit = 0x0000800000000000ULL;
    return value >= kLowest && value < kUserLimit && (value & 0x7) == 0;
}

inline bool plausiblePointer(const void* value)
{
    return plausiblePointer(reinterpret_cast<std::uintptr_t>(value));
}

bool isReadable(const void* address, size_t size);

bool isWritable(const void* address, size_t size);

bool isExecutable(const void* address, size_t size);

bool inGameModule(const void* address);

void* ripTarget(const std::byte* at, std::size_t dispOffset);

std::size_t functionSize(const void* address);

}
