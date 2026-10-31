#pragma once
#include "GameConnection.h"
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace tsukuyomi::bot::raknet {

bool initialize();
bool available();
bool isRakNet(void* mainPeer);
struct Address { std::string host; std::uint16_t port = 0; };
std::optional<Address> remoteAddress(void* mainPeer);

class Link final : public gameconn::Transport {
public:
    enum class State { Idle, Connecting, Open, Closed };
    static constexpr std::uint64_t kConnectTimeoutMs = 10000;
    Link() = default;
    ~Link() override;
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;
    bool open(const Address& server, std::string& error);
    void tick();
    State state() const { return state_; }
    const std::string& closeReason() const { return reason_; }
    void close();
    bool sendBatch(const std::string& bytes, int reliability) override;
    std::optional<std::string> receiveBatch() override;
    bool encryptsItself() const override { return false; }
private:
    void fail(std::string reason);
    void* peer_ = nullptr;
    void* destroy_ = nullptr;
    State state_ = State::Idle;
    std::string reason_;
    std::uint64_t since_ = 0;
    alignas(8) unsigned char guid_[16]{};
    std::deque<std::string> incoming_;
};

}
