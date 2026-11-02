#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace tsukuyomi::replay {
struct TappedPacket { bool sent; std::uint16_t id; std::vector<std::uint8_t> body; };
class PacketTap {
public:
    static PacketTap& instance();
    void push(bool sent, std::uint16_t id, const std::uint8_t* body, std::size_t length);
    std::vector<TappedPacket> take();
    bool takeOverflowed();
private:
    std::mutex mutex_;
    std::deque<TappedPacket> queue_;
    std::size_t bytes_ = 0;
    bool overflowed_ = false;
};
}
