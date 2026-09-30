#include "core/DiagFlags.h"

#include "core/Paths.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <system_error>

namespace tsukuyomi::diagflags {
namespace {

constexpr std::array<const wchar_t*, static_cast<size_t>(Flag::Count)> kNames = {
    L"diag-hooks-all.txt", L"diag-frametrace.txt", L"diag-hotcount.txt", L"diag-perf.txt"};
std::array<std::atomic<bool>, kNames.size()> g_flags{};
unsigned long long g_lastPoll = 0;
bool g_polled = false;

}

void poll(const std::filesystem::path& dir)
{
    const unsigned long long at = GetTickCount64();
    if (g_polled && at - g_lastPoll < 500) {
        return;
    }
    g_lastPoll = at;
    g_polled = true;
    for (size_t i = 0; i < kNames.size(); ++i) {
        std::error_code ec;
        const bool exists = std::filesystem::exists(dir / kNames[i], ec);
        g_flags[i].store(exists, std::memory_order_release);
    }
}

void poll()
{
    poll(paths::dataDir());
}

bool get(Flag flag)
{
    const auto index = static_cast<size_t>(flag);
    return index < g_flags.size() && g_flags[index].load(std::memory_order_acquire);
}

}
