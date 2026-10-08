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

    std::atomic<void*> m_player{nullptr};

};

}
