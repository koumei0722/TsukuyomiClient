#include "replay/PacketTap.h"
#include "replay/BedrockPackets.h"
#include <iterator>

namespace tsukuyomi::replay {
PacketTap& PacketTap::instance() { static PacketTap tap; return tap; }
void PacketTap::push(bool sent, std::uint16_t id, const std::uint8_t* body, std::size_t length) {
    if (!bedrock::wanted(id, sent) || (length && !body)) return;
    std::lock_guard lock(mutex_);
    constexpr std::size_t limit = 64 * 1024 * 1024;
    while (!queue_.empty() && (queue_.size() >= 200000 || length > limit || bytes_ > limit - length)) {
        bytes_ -= queue_.front().body.size(); queue_.pop_front(); overflowed_ = true;
    }
    if (length > limit) { overflowed_ = true; return; }
    TappedPacket p{sent, id, {}}; if (length) p.body.assign(body, body + length);
    queue_.push_back(std::move(p)); bytes_ += length;
}
std::vector<TappedPacket> PacketTap::take() {
    std::lock_guard lock(mutex_);
    std::vector<TappedPacket> out(std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
    queue_.clear(); bytes_ = 0; return out;
}
bool PacketTap::takeOverflowed() { std::lock_guard lock(mutex_); const bool result = overflowed_; overflowed_ = false; return result; }
}
