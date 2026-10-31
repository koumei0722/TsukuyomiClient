#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tsukuyomi::buildover {

struct PlaceFields {
    std::uint32_t dimensionOffset = 0;
    std::uint8_t blockSourceSlot = 0;
    std::uint8_t getBlockSlot = 0;
    std::uint8_t blockTypeOffset = 0;
    std::uint32_t buildOverSlot = 0;
};

inline constexpr std::size_t kPlaceSiteBytes = 183;

inline bool readPlaceFields(const std::byte* site, std::size_t size, PlaceFields& out)
{
    if (site == nullptr || size < kPlaceSiteBytes || site[0] != std::byte{0x48}
        || site[35] != std::byte{0x4c} || site[36] != std::byte{0x8b} || site[37] != std::byte{0xa9}
        || site[101] != std::byte{0x48} || site[102] != std::byte{0x8b} || site[103] != std::byte{0x40}
        || site[121] != std::byte{0x48} || site[122] != std::byte{0x8b} || site[123] != std::byte{0x40}
        || site[160] != std::byte{0x48} || site[161] != std::byte{0x8b} || site[162] != std::byte{0x48}
        || site[167] != std::byte{0x4c} || site[168] != std::byte{0x8b} || site[169] != std::byte{0x92}) return false;
    PlaceFields fields;
    std::memcpy(&fields.dimensionOffset, site + 38, 4);
    fields.blockSourceSlot = std::to_integer<std::uint8_t>(site[104]);
    fields.getBlockSlot = std::to_integer<std::uint8_t>(site[124]);
    fields.blockTypeOffset = std::to_integer<std::uint8_t>(site[163]);
    std::memcpy(&fields.buildOverSlot, site + 170, 4);
    const auto aligned = [](std::uint32_t value, std::uint32_t limit) { return value > 0 && value < limit && value % 8 == 0; };
    if (!aligned(fields.dimensionOffset, 0x4000) || !aligned(fields.blockSourceSlot, 0x100)
        || fields.getBlockSlot % 8 != 0 || !aligned(fields.blockTypeOffset, 0x100)
        || !aligned(fields.buildOverSlot, 0x2000)) return false;
    out = fields;
    return true;
}

}
