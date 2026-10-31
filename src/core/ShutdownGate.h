#pragma once

#include <atomic>
#include <cstdint>

namespace tsukuyomi {

class ShutdownGate {
public:
    class Guard {
    public:
        explicit Guard(ShutdownGate& gate) : m_gate(gate.enter() ? &gate : nullptr) {}
        ~Guard() { if (m_gate != nullptr) m_gate->leave(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        explicit operator bool() const { return m_gate != nullptr; }

    private:
        ShutdownGate* m_gate;
    };

    void close()
    {
        std::uint64_t state = m_state.fetch_or(kClosed, std::memory_order_acq_rel) | kClosed;
        while (state != kClosed) {
            m_state.wait(state, std::memory_order_acquire);
            state = m_state.load(std::memory_order_acquire);
        }
    }

private:
    bool enter()
    {
        std::uint64_t state = m_state.load(std::memory_order_relaxed);
        while ((state & kClosed) == 0) {
            if (m_state.compare_exchange_weak(state, state + 1, std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void leave()
    {
        if (m_state.fetch_sub(1, std::memory_order_release) == kClosed + 1) {
            m_state.notify_all();
        }
    }

    static constexpr std::uint64_t kClosed = std::uint64_t{1} << 63;
    std::atomic<std::uint64_t> m_state{0};
};

}
