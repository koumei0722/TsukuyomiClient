#pragma once
#include "BotProtocol.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace tsukuyomi::bot::gamepacket {

class Packet {
public:
    Packet() = default;
    Packet(Packet&& other) noexcept { *this = std::move(other); }
    Packet& operator=(Packet&& other) noexcept;
    Packet(const Packet&) = delete;
    Packet& operator=(const Packet&) = delete;
    ~Packet();
    explicit operator bool() const { return object_ != nullptr; }
    std::uint16_t id() const { return id_; }
    std::byte* object() const { return static_cast<std::byte*>(object_); }
    void* control() const { return control_; }
    void abandon() { object_ = nullptr; control_ = nullptr; }
private:
    friend Packet create(std::uint16_t id);
    void* object_ = nullptr;
    void* control_ = nullptr;
    std::uint16_t id_ = 0;
};

bool initialize();
bool available();
Packet create(std::uint16_t id);
std::optional<std::string> write(const Packet& packet);
bool read(Packet& packet, protocol::Bytes body, std::size_t* consumed = nullptr);

}
