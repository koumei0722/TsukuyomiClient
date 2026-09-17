#pragma once

#include <atomic>
#include <cstddef>

namespace tsukuyomi {

class UiSound {
public:
    static UiSound& instance();

    void onScansReady();

    void request();

    void pump();

    void onPlayed(void* self, const void* name, const void* position, float a, float b, bool c,
                  const void* extra);

private:
    UiSound() = default;

    static constexpr std::ptrdiff_t kControlBlock = 0x1190;
    static constexpr std::ptrdiff_t kSoundPlayer = 0x11a0;
    static constexpr std::ptrdiff_t kListener = 0x11b8;
    static constexpr std::size_t kSelfSize = 0x1310;

    static bool selfIsAlive(const void* self);

    std::atomic<bool> m_wanted{false};

    std::atomic<void*> m_self{nullptr};
    std::atomic<bool> m_ready{false};
    float m_a = 1.0f;
    float m_b = 1.0f;
    bool m_c = false;
    alignas(16) unsigned char m_position[16]{};
    alignas(16) unsigned char m_extra[16]{};
};

}
