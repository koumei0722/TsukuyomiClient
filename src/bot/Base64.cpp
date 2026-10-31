#include "Base64.h"

namespace tsukuyomi::bot::base64 {
namespace {
constexpr std::string_view kStandard = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view kUrl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string encodeImpl(std::span<const std::uint8_t> bytes, bool url)
{
    const auto alphabet = url ? kUrl : kStandard;
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto left = bytes.size() - i;
        const std::uint32_t value = (static_cast<std::uint32_t>(bytes[i]) << 16)
            | (left > 1 ? static_cast<std::uint32_t>(bytes[i + 1]) << 8 : 0)
            | (left > 2 ? bytes[i + 2] : 0);
        result += alphabet[(value >> 18) & 63];
        result += alphabet[(value >> 12) & 63];
        if (left > 1) result += alphabet[(value >> 6) & 63];
        else if (!url) result += '=';
        if (left > 2) result += alphabet[value & 63];
        else if (!url) result += '=';
    }
    return result;
}

std::optional<std::vector<std::uint8_t>> decodeImpl(std::string_view text, bool url)
{
    const auto alphabet = url ? kUrl : kStandard;
    std::size_t count = text.size();
    if (url) {
        if (count % 4 == 1 || text.find('=') != std::string_view::npos) return std::nullopt;
    } else {
        if (count % 4 != 0) return std::nullopt;
        while (count > 0 && text[count - 1] == '=') --count;
        const auto padding = text.size() - count;
        if (padding > 2 || (padding == 1 && count % 4 != 3)
            || (padding == 2 && count % 4 != 2)) return std::nullopt;
    }
    std::vector<std::uint8_t> result;
    std::uint32_t value = 0;
    unsigned bits = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto at = alphabet.find(text[i]);
        if (at == std::string_view::npos) return std::nullopt;
        value = (value << 6) | static_cast<std::uint32_t>(at);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<std::uint8_t>(value >> bits));
        }
    }
    if (bits && (value & ((1u << bits) - 1)) != 0) return std::nullopt;
    return result;
}
std::span<const std::uint8_t> asBytes(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}
}
std::string encode(std::span<const std::uint8_t> bytes) { return encodeImpl(bytes, false); }
std::string encode(std::string_view bytes) { return encode(asBytes(bytes)); }
std::string encodeUrl(std::span<const std::uint8_t> bytes) { return encodeImpl(bytes, true); }
std::string encodeUrl(std::string_view bytes) { return encodeUrl(asBytes(bytes)); }
std::optional<std::vector<std::uint8_t>> decode(std::string_view text) { return decodeImpl(text, false); }
std::optional<std::vector<std::uint8_t>> decodeUrl(std::string_view text) { return decodeImpl(text, true); }
}
