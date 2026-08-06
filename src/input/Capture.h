#pragma once

#include <atomic>

namespace tsukuyomi::input {

namespace detail {
inline std::atomic<bool> g_consoleCapturing{false};
}

inline void setConsoleCapturing(bool capturing)
{
    detail::g_consoleCapturing.store(capturing, std::memory_order_release);
}

inline bool consoleCapturing()
{
    return detail::g_consoleCapturing.load(std::memory_order_acquire);
}

}
