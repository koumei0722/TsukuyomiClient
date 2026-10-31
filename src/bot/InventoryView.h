#pragma once
#include "BotSession.h"
#include "InventoryLayout.h"
#include "game/ChatCommand.h"
#include <array>
#include <vector>
#include <map>

namespace tsukuyomi::bot {
class InventoryView {
public:
    static constexpr std::uint8_t kWindow = inventorylayout::kWindow;
    static constexpr std::uint8_t kChestContainer = inventorylayout::kChestContainer;
    chatcommand::Reply open(const std::string& botName, BotSession& session, std::uint64_t now);
    chatcommand::Reply openSnapshot(const std::string& name, BotSession::InventorySnapshot snapshot, std::uint64_t now);
    void close(bool serverInitiated, BotSession* session);
    void tick(BotSession* session, std::uint64_t now);
    void onResponse(protocol::Bytes body);
    void abandon();
    bool isOpen() const { return state_ != State::Closed; }
    bool isBot() const { return isOpen() && !snapshot_; }
    const std::string& botName() const { return botName_; }
    static bool filterSend(std::uint16_t id, protocol::Bytes body);
private:
    enum class State { Closed, Opening, Assigning, Open, Closing };
    struct Shown {
        bool empty = true;
        std::int32_t netId = 0;
        std::uint16_t count = 0;
        bool operator==(const Shown&) const = default;
    };
    static Shown shown(const Item& item);
    void updateShown(const BotSession::InventorySnapshot& snapshot, bool initial);
    void forgetShown();
    std::optional<std::string> prepare(const std::string& name);
    void begin(const std::string& name, std::uint64_t now);
    std::optional<BotSession::InventorySnapshot> snapshot_;
    State state_ = State::Closed;
    std::map<std::int32_t, std::uint64_t> pendingRequests_;
    bool closeServerInitiated_ = false, cursorReturnSent_ = false;
    std::string botName_;
    std::array<std::int32_t, 3> position_{};
    std::array<Shown, 54> shown_{};
    Shown cursor_;
    std::uint64_t spawnSerial_ = 0, openAt_ = 0;
};
}
