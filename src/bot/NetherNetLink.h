#pragma once
#include "GameConnection.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace tsukuyomi::bot::crypto { class EcKey; }

namespace tsukuyomi::bot::nethernet {

bool initialize();
bool available();
bool isNetherNet(void* mainPeer);
struct ServerId { std::uint64_t id = 0, extra = 0; std::uint8_t type = 0; };
std::optional<ServerId> serverId(void* mainPeer);

#pragma warning(push)
#pragma warning(disable: 4324)
class Link final : public gameconn::Transport {
public:
    enum class State { Idle, Creating, Opening, Open, Closed };
    Link();
    ~Link() override;
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;
    bool open(void* mainPeer, std::string token, std::shared_ptr<crypto::EcKey> key, std::string& error);
    State state() const { return state_.load(std::memory_order_acquire); }
    void tick();
    std::string closeReason() const;
    bool send(const std::string& bytes);
    std::optional<std::string> receive();
    bool sendBatch(const std::string& bytes, int) override { return send(bytes); }
    std::optional<std::string> receiveBatch() override { return receive(); }
    bool encryptsItself() const override { return true; }
    void close();

    void onOpened(std::uint64_t connectionId);
    void onEvent(int slot, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4);
    std::optional<std::string> makeAssertion(const std::string& fingerprints) const;
    void* gameCallbacks() const { return gameCallbacks_; }

    struct NetworkId { std::uint64_t id = 0, extra = 0; std::uint8_t type = 0; std::uint8_t pad[7]{}; };
    static_assert(sizeof(NetworkId) == 24);
    struct Identity;
private:
    std::atomic<State> state_{State::Idle};
    mutable std::mutex mutex_;
    std::string closeReason_;
    void* transport_ = nullptr;
    alignas(16) unsigned char transportHolder_[0x48]{};
    bool holderValid_ = false;
    NetworkId localId_{}, remoteId_{};
    std::uint64_t createdAt_ = 0;
    bool openSession();
    bool ready() const;
    std::uint64_t connectionId_ = 0;
    void* gameCallbacks_ = nullptr;
    std::string token_;
    std::shared_ptr<crypto::EcKey> key_;
    bool gameIdentity_ = false;
    struct Callbacks { void* const* vtable; Link* owner; } callbacks_{};
    std::unique_ptr<Identity> identity_;
};
#pragma warning(pop)

}
