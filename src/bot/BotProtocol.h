#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi::bot::protocol {
using Bytes = std::span<const std::uint8_t>;
Bytes bytes(std::string_view value);
void writeVarUInt32(std::string&, std::uint32_t);
void writeVarUInt64(std::string&, std::uint64_t);
void writeVarInt32(std::string&, std::int32_t);
void writeVarInt64(std::string&, std::int64_t);
bool writeString(std::string&, std::string_view);
class Reader {
public:
    explicit Reader(Bytes data) : data_(data) {}
    std::optional<std::uint32_t> varUInt32();
    std::optional<std::uint64_t> varUInt64();
    std::optional<std::int32_t> varInt32();
    std::optional<std::int64_t> varInt64();
    std::optional<std::uint8_t> byte();
    std::optional<bool> boolean();
    std::optional<std::string> string();
    std::optional<std::uint64_t> fixed64();
    std::optional<float> float32();
    std::optional<std::uint64_t> fixedBytes(unsigned count);
    std::size_t remaining() const { return data_.size() - position_; }
    std::size_t position() const { return position_; }
private:
    std::optional<std::uint64_t> varUInt(unsigned bits);
    Bytes data_;
    std::size_t position_ = 0;
};
struct Header { std::uint16_t id; std::uint8_t recipient, sender; std::size_t bodyOffset; };
std::uint32_t header(std::uint16_t id, std::uint8_t recipient, std::uint8_t sender);
std::optional<Header> readHeader(Bytes);
bool isReadPacket(std::uint16_t id);
inline constexpr std::size_t kMaxBlobStatusHashes = 0xfff;
struct Vec3 { float x = 0, y = 0, z = 0; };
struct AuthInput {
    float pitch = 0, yaw = 0;
    Vec3 position, delta;
    float moveX = 0, moveZ = 0;
    std::vector<std::int32_t> extraFlags;
    std::uint64_t tick = 0;
    bool handledTeleport = false, verticalCollision = false;
};
inline constexpr std::uint8_t kRespawnClientReady = 2;
struct Spawn { Vec3 position; float pitch = 0, yaw = 0; };
struct StartGame { std::int64_t uniqueId; std::uint64_t runtimeId; std::optional<Spawn> spawn; };
struct MovePlayer { std::uint64_t runtimeId; Vec3 position; float pitch, yaw; std::uint8_t mode; bool onGround; };
struct Correction { std::uint8_t type; Vec3 position, delta; bool onGround; std::uint64_t tick; };
struct Respawn { Vec3 position; std::uint8_t state; std::uint64_t runtimeId; };
struct ChangeDimension { std::int32_t dimension; Vec3 position; bool respawn; std::optional<std::uint32_t> screenId; };
struct Latency { std::int64_t timestamp; bool needsResponse; };
struct Disconnect { std::int32_t reason; bool skipMessage; std::string message; };
}
