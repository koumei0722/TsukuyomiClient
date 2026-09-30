#include "input/Foreground.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace tsukuyomi::input {
namespace {

bool belongsToThisProcess(HWND window)
{
    if (window == nullptr) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId();
}

struct RecentResult {
    std::atomic<std::uint64_t> atNs{0};
    std::atomic<bool> value{false};
};

std::uint64_t nowNs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool cached(RecentResult& result, bool (*measure)())
{
    const std::uint64_t now = nowNs();
    const std::uint64_t at = result.atNs.load(std::memory_order_acquire);
    if (at != 0 && now >= at && now - at < 5'000'000) {
        return result.value.load(std::memory_order_relaxed);
    }
    const bool value = measure();
    result.value.store(value, std::memory_order_relaxed);
    result.atNs.store(now, std::memory_order_release);
    return value;
}

bool measureForeground()
{
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) return false;
    if (belongsToThisProcess(foreground)) return true;
    const HWND core = FindWindowExA(foreground, nullptr, "Windows.UI.Core.CoreWindow", nullptr);
    return belongsToThisProcess(core);
}

bool measureGameplay()
{
    if (!isGameForeground()) return false;
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    if (GetCursorInfo(&info) == 0) return false;
    return (info.flags & CURSOR_SHOWING) == 0;
}

}

bool isGameForeground()
{
    static RecentResult result;
    return cached(result, &measureForeground);
}

bool isInGameplay()
{
    static RecentResult result;
    return cached(result, &measureGameplay);
}

}
