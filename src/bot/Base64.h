#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi::bot::base64 {
std::string encode(std::span<const std::uint8_t> bytes);
std::string encode(std::string_view bytes);
std::string encodeUrl(std::span<const std::uint8_t> bytes);
std::string encodeUrl(std::string_view bytes);
std::optional<std::vector<std::uint8_t>> decode(std::string_view text);
std::optional<std::vector<std::uint8_t>> decodeUrl(std::string_view text);
}
