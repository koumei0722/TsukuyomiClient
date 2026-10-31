#include "BotProtocol.h"
#include <bit>
#include <limits>

namespace tsukuyomi::bot::protocol {
Bytes bytes(std::string_view value) { return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}; }
void writeVarUInt64(std::string& out, std::uint64_t value)
{
    do {
        auto next = static_cast<std::uint8_t>(value & 0x7f);
        value >>= 7;
        if (value) next |= 0x80;
        out.push_back(static_cast<char>(next));
    } while (value);
}
void writeVarUInt32(std::string& out, std::uint32_t value) { writeVarUInt64(out, value); }
void writeVarInt32(std::string& out, std::int32_t value)
{
    const auto bits = std::bit_cast<std::uint32_t>(value);
    writeVarUInt32(out, (bits << 1) ^ (0u - (bits >> 31)));
}
void writeVarInt64(std::string& out, std::int64_t value)
{
    const auto bits = std::bit_cast<std::uint64_t>(value);
    writeVarUInt64(out, (bits << 1) ^ (std::uint64_t{0} - (bits >> 63)));
}
bool writeString(std::string& out, std::string_view value)
{
    if (value.size() > (std::numeric_limits<std::uint32_t>::max)()) return false;
    writeVarUInt32(out, static_cast<std::uint32_t>(value.size()));
    out.append(value);
    return true;
}
std::optional<std::uint8_t> Reader::byte()
{
    if (position_ == data_.size()) return std::nullopt;
    return data_[position_++];
}
std::optional<bool> Reader::boolean()
{
    const auto value = byte();
    if (!value || *value > 1) return std::nullopt;
    return *value != 0;
}
std::optional<std::uint64_t> Reader::varUInt(unsigned bits)
{
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < bits; shift += 7) {
        const auto next = byte();
        if (!next) return std::nullopt;
        const auto low = static_cast<std::uint64_t>(*next & 0x7f);
        if (bits - shift < 7 && low >= (std::uint64_t{1} << (bits - shift))) return std::nullopt;
        value |= low << shift;
        if ((*next & 0x80) == 0) return value;
    }
    return std::nullopt;
}
std::optional<std::uint32_t> Reader::varUInt32()
{
    const auto value = varUInt(32);
    if (!value) return std::nullopt;
    return static_cast<std::uint32_t>(*value);
}
std::optional<std::uint64_t> Reader::varUInt64() { return varUInt(64); }
std::optional<std::int32_t> Reader::varInt32()
{
    const auto value = varUInt32();
    if (!value) return std::nullopt;
    return std::bit_cast<std::int32_t>((*value >> 1) ^ (0u - (*value & 1)));
}
std::optional<std::int64_t> Reader::varInt64()
{
    const auto value = varUInt64();
    if (!value) return std::nullopt;
    return std::bit_cast<std::int64_t>((*value >> 1) ^ (std::uint64_t{0} - (*value & 1)));
}
std::optional<std::string> Reader::string()
{
    const auto length = varUInt32();
    if (!length || *length > data_.size() - position_) return std::nullopt;
    std::string result;
    if (*length) result.assign(reinterpret_cast<const char*>(data_.data() + position_), *length);
    position_ += *length;
    return result;
}
std::optional<std::uint64_t> Reader::fixed64()
{
    if (remaining() < 8) return std::nullopt;
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t{data_[position_ + i]} << (i * 8);
    position_ += 8;
    return value;
}
std::optional<std::uint64_t> Reader::fixedBytes(unsigned count)
{
    if (count > 8 || remaining() < count) return std::nullopt;
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= std::uint64_t{data_[position_ + i]} << (i * 8);
    position_ += count;
    return value;
}
std::optional<float> Reader::float32()
{
    const auto value = fixedBytes(4);
    if (!value) return std::nullopt;
    return std::bit_cast<float>(static_cast<std::uint32_t>(*value));
}
std::uint32_t header(std::uint16_t id, std::uint8_t recipient, std::uint8_t sender)
{
    return (id & 0x3ffu) | ((recipient & 3u) << 10) | ((sender & 3u) << 12);
}
std::optional<Header> readHeader(Bytes data)
{
    Reader reader(data);
    const auto value = reader.varUInt32();
    if (!value || *value > 0x3fff) return std::nullopt;
    return Header{static_cast<std::uint16_t>(*value & 0x3ff), static_cast<std::uint8_t>((*value >> 10) & 3),
        static_cast<std::uint8_t>((*value >> 12) & 3), reader.position()};
}
bool isReadPacket(std::uint16_t id)
{
    return id == 2 || id == 5 || id == 0x0b || id == 0x13 || id == 0x2d || id == 0x3a || id == 0x3d
        || id == 0x46 || id == 0x73 || id == 0xa1 || id == 0x11 || id == 0x1f || id == 0x30 || id == 0x31 || id == 0x32 || id == 0x94;
}
}
