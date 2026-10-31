#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace tsukuyomi::bot::networklayout {
inline constexpr std::size_t kCreateBytes = 0x102, kOptionsBytes = 0x54, kDestroyBytes = 0x1e;
struct ConnectionFields {
    std::uint32_t size, arg5Pointer, arg5Control, arg5Extra, arg6, arg7;
    std::uint32_t options, optionsText, optionsTag;
};
std::optional<ConnectionFields> readConnection(std::span<const std::byte> create, std::span<const std::byte> options, std::span<const std::byte> destroy);
struct FriendFields { std::uint32_t head, key; };
std::optional<FriendFields> readFriendLookup(std::span<const std::byte> code);
}
