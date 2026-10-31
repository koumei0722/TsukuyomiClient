#pragma once
#include <cstdint>

namespace tsukuyomi::bot::inventorylayout {
inline constexpr std::uint8_t kWindow = 99, kChestContainer = 7;
bool toBot(std::uint8_t& container, std::uint8_t& slot);
bool toChest(std::uint8_t& container, std::uint8_t& slot);
}
