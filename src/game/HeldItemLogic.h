#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tsukuyomi::helditem {

inline bool readSelectedItemSlot(const std::byte* site, std::size_t size, std::uint32_t& out)
{
    if (site == nullptr || size < 22 || site[15] != std::byte{0x48}
        || site[16] != std::byte{0x8b} || site[17] != std::byte{0x80}) return false;
    std::memcpy(&out, site + 18, 4);
    return true;
}

}
