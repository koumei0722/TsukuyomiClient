#pragma once

#include <atomic>
#include <cstddef>

namespace tsukuyomi {

class PlayerContext {
public:
    static PlayerContext& instance();

    void onEntityContext(void* entityContext);

private:
    PlayerContext() = default;

    void noteOtherTarget(const void* vtable, bool serverType, bool kept);

    std::atomic<void*> m_player{nullptr};

    static constexpr std::size_t kOtherTargetLogs = 4;
    std::atomic<const void*> m_otherTargetVtables[kOtherTargetLogs] = {};
    std::atomic<std::size_t> m_otherTargetCount{0};
};

}
