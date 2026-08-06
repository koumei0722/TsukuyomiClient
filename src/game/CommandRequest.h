#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace tsukuyomi {

class CommandRequest {
public:
    static CommandRequest& instance();

    void onScansReady();

    bool available() const;

    void onEntityContext(void* entityContext);

    bool run(const char* command);

    static constexpr std::size_t kMaxLength = 120;

private:
    CommandRequest() = default;

    struct StdString {
        union {
            char inline_[16];
            const char* pointer;
        } data{};
        std::uint64_t length = 0;
        std::uint64_t capacity = 15;
    };
    static_assert(sizeof(StdString) == 0x20, "std::string のレイアウトが実測と合っていません");

    struct Args {
        StdString text;
        const void* origin = nullptr;
        std::int32_t version = 0;
        std::int32_t pad = 0;
    };
    static_assert(sizeof(Args) == 0x30, "コマンド引数のレイアウトが実測と合っていません");

    static constexpr std::int32_t kCommandVersion = 49;

    static constexpr int kMaxSenderCandidates = 32;

    static constexpr std::uintptr_t kUserAddressLimit = 0x0000800000000000ULL;

    static constexpr unsigned long long kWarnIntervalMs = 10000;

    static constexpr std::size_t kOriginSize = 0x28;

    static constexpr std::size_t kOriginGuardSize = 0x18;
    static constexpr std::byte kOriginGuardByte{0xA5};

    using SendCommandFn = void(__fastcall*)(void* sender, std::int32_t* out, const Args* command,
                                            std::int32_t flags);

    using MakeCommandOriginFn = void*(__fastcall*)(void* out, void* player);

    void* findSender(int& survivors);

    static int scoreSender(const std::byte* candidate);

    void* sender();

    const void* makeOrigin();

    SendCommandFn m_send = nullptr;
    MakeCommandOriginFn m_makeOrigin = nullptr;

    std::atomic<void*> m_player{nullptr};

    std::atomic<void*> m_sender{nullptr};

    std::byte m_origin[kOriginSize + kOriginGuardSize]{};

    char m_text[kMaxLength + 1]{};

    std::atomic<bool> m_warnedNoPlayer{false};
    std::atomic<bool> m_warnedBadPlayer{false};

    std::atomic<unsigned long long> m_nextSenderWarnMs{0};
    std::atomic<unsigned long long> m_nextPlayerWarnMs{0};
    std::atomic<bool> m_loggedOrigin{false};

    std::atomic<bool> m_originBroken{false};
};

}
