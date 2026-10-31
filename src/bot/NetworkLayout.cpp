#include "NetworkLayout.h"
#include <cstring>
#include <initializer_list>

namespace tsukuyomi::bot::networklayout {
namespace {
bool matches(std::span<const std::byte> code, std::size_t at, std::initializer_list<int> bytes)
{
    if (at > code.size() || bytes.size() > code.size() - at) return false;
    for (const int value : bytes) {
        if (value >= 0 && std::to_integer<int>(code[at]) != value) return false;
        ++at;
    }
    return true;
}
std::uint32_t word(std::span<const std::byte> code, std::size_t at)
{
    std::uint32_t value = 0;
    std::memcpy(&value, code.data() + at, sizeof value);
    return value;
}
}
std::optional<ConnectionFields> readConnection(std::span<const std::byte> create, std::span<const std::byte> options, std::span<const std::byte> destroy)
{
    if (create.size() < kCreateBytes || options.size() < kOptionsBytes || destroy.size() < kDestroyBytes
        || !matches(destroy, 0, {0x48, 0x8b, 0x75, 0x58, 0x48, 0x85, 0xf6, 0x74, -1, 0x48, 0x89, 0xf1, 0xe8})
        || !matches(destroy, 17, {0xba}) || !matches(destroy, 22, {0x48, 0x89, 0xf1, 0xe8})
        || !matches(create, 0, {0xe8}) || !matches(create, 6, {0x4c, 0x8b, 0xbe})
        || !matches(create, 0x1b, {0xba}) || !matches(create, 0x6e, {0x48, 0x8d, 0x86})
        || !matches(create, 0x88, {0x48, 0x8b, 0x8e}) || !matches(create, 0x98, {0x48, 0x8b, 0x8e})
        || !matches(create, 0xa3, {0x48, 0x8b, 0x96}) || !matches(create, 0xb2, {0x48, 0x8b, 0x8e})
        || !matches(create, 0xcb, {0x48, 0x89, 0x4c, 0x24, 0x40})
        || !matches(create, 0xd4, {0x48, 0x89, 0x4c, 0x24, 0x38, 0x48, 0x89, 0x44, 0x24, 0x30, 0x4c, 0x89, 0x7c, 0x24, 0x28})
        || !matches(create, 0xf6, {0x45, 0x89, 0xf1}) || !matches(create, 0xfd, {0xe8})
        || !matches(options, 7, {0x80, 0x79, 0x28, 0x01})
        || !matches(options, 0xd, {0x8b, 0x01, 0x89, 0x87})
        || !matches(options, 0x23, {0x0f, 0x10, 0x41, 0x08, 0x0f, 0x10, 0x49, 0x18})
        || !matches(options, 0x2b, {0x0f, 0x11, 0x8f}) || !matches(options, 0x32, {0x0f, 0x11, 0x87})
        || !matches(options, 0x39, {0x48, 0xc7, 0x41, 0x18, 0, 0, 0, 0, 0x48, 0xc7, 0x41, 0x20, 0x0f, 0, 0, 0})
        || !matches(options, 0x4d, {0xc6, 0x87, -1, -1, -1, -1, 1})) return std::nullopt;
    const ConnectionFields f{word(create, 0x1c), word(create, 0xa6), word(create, 0x8b), word(create, 0xb5),
        word(create, 9), word(create, 0x71), word(options, 0x11), word(options, 0x35), word(options, 0x4f)};
    if (word(destroy, 18) != f.size || word(create, 0x9b) != f.arg5Control || f.arg5Control != f.arg5Pointer + 8 || f.arg5Extra != f.arg5Control + 8
        || f.size < 0x100 || f.size >= 0x1000 || f.arg6 >= 0x1000 || f.arg7 >= 0x1000
        || f.arg5Pointer >= 0x1000 || f.arg5Control >= 0x1000 || f.arg5Extra >= 0x1000
        || f.options >= f.size || f.optionsText >= f.size
        || f.optionsText != f.options + 8 || word(options, 0x2e) != f.optionsText + 0x10
        || f.optionsTag != f.options + 0x28 || f.optionsTag >= f.size) return std::nullopt;
    return f;
}
std::optional<FriendFields> readFriendLookup(std::span<const std::byte> code)
{
    std::optional<FriendFields> result;
    for (std::size_t at = 0; at < code.size(); ++at) {
        if (!matches(code, at, {0x48, 0x23, 0x81, -1, -1, -1, -1, 0x48, 0x8b, 0x99, -1, -1, -1, -1,
                0x48, 0x8b, 0x89, -1, -1, -1, -1, 0x48, 0xc1, 0xe0, 4, 0x4c, 0x8b, 0x74, 1, 8, 0x49, 0x39, 0xde})) continue;
        const auto head = word(code, at + 10);
        if (word(code, at + 3) != head + 0x28 || word(code, at + 17) != head + 0x10 || head >= 0x400) return std::nullopt;
        std::optional<std::uint32_t> key;
        for (std::size_t i = at + 33; i < code.size(); ++i) {
            if (!matches(code, i, {0x49, 0x3b, 0x7e, -1, 0x75, -1, 0x49, 0x83, 0x7e, -1, 0x10})) continue;
            const auto length = std::to_integer<std::uint32_t>(code[i + 3]);
            if (key || length < 0x10 || length >= 0x50 || std::to_integer<std::uint32_t>(code[i + 9]) != length + 8) return std::nullopt;
            key = length - 0x10;
        }
        if (!key || result) return std::nullopt;
        result = FriendFields{head, *key};
    }
    return result;
}
}
