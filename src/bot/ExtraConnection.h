#pragma once
#include "BotProtocol.h"
#include "GameConnection.h"
#include "NetherNetLink.h"
#include "RakNetLink.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tsukuyomi::bot {

class ExtraConnection {
public:
    enum class Phase { Idle, Opening, Settings, LoggingIn, Ready, Closed };
    enum class Kind { NetherNet, RakNet };
    struct Target {
        Kind kind = Kind::NetherNet;
        void* mainPeer = nullptr;
        raknet::Address address;
    };
    static constexpr std::uint64_t kOpenTimeoutMs = 20000, kSettingsTimeoutMs = 15000;
    bool start(const Target& target, std::int32_t protocol, std::string request, std::string token, std::shared_ptr<crypto::EcKey> key,
               std::uint64_t nowMs, std::string& error);
    std::vector<std::string> pump(std::uint64_t nowMs);
    bool send(const std::vector<std::string>& packets);
    void flush();
    Phase phase() const { return phase_; }
    Kind kind() const { return kind_; }
    const std::string& reason() const { return reason_; }
    void close(std::string reason = {});
private:
    void fail(std::string reason);
    gameconn::Transport* transport();
    bool transportOpen();
    bool transportClosed(std::string& why);
    void onServerHandshake(protocol::Bytes body);
    nethernet::Link nether_;
    raknet::Link rak_;
    gameconn::Connection game_;
    Kind kind_ = Kind::NetherNet;
    Phase phase_ = Phase::Idle;
    std::int32_t protocol_ = 0;
    std::string request_, reason_;
    std::shared_ptr<crypto::EcKey> key_;
    std::uint64_t since_ = 0;
    std::vector<bool> seenIds_ = std::vector<bool>(1024);
    int loggedIds_ = 0;
};

}
