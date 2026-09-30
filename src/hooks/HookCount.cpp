#include "hooks/HookCount.h"

#include "core/Logger.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <vector>

namespace tsukuyomi::hooks {
namespace {

constexpr std::size_t kMaxCounters = 128;
std::array<HookCounter*, kMaxCounters> g_counters{};
std::atomic<std::size_t> g_counterCount{0};

std::atomic<bool> g_counting{false};
std::atomic<unsigned long long> g_reportedAt{0};
constexpr unsigned long long kReportMs = 30000;

std::atomic<unsigned long long> g_since{0};

}

HookCounter::HookCounter(const wchar_t* name) : m_name(name)
{
    const std::size_t at = g_counterCount.fetch_add(1, std::memory_order_acq_rel);
    if (at < kMaxCounters) {
        g_counters[at] = this;
    }
}

bool HookCounter::counting()
{
    return g_counting.load(std::memory_order_relaxed);
}

void setCountingHooks(bool on)
{
    if (g_counting.exchange(on, std::memory_order_relaxed) == on) {
        return;
    }
    const unsigned long long at = GetTickCount64();
    g_since.store(at, std::memory_order_relaxed);
    g_reportedAt.store(at, std::memory_order_relaxed);
    if (on) {
        const std::size_t taken = std::min(g_counterCount.load(std::memory_order_acquire),
                                           kMaxCounters);
        for (std::size_t i = 0; i < taken; ++i) {
            if (g_counters[i] != nullptr) {
                (void)g_counters[i]->take();
            }
        }
    }
}

void reportHookCounts()
{
    if (!g_counting.load(std::memory_order_relaxed)) {
        return;
    }
    const unsigned long long at = GetTickCount64();
    const unsigned long long was = g_reportedAt.load(std::memory_order_relaxed);
    if (was != 0 && at - was < kReportMs) {
        return;
    }
    g_reportedAt.store(at, std::memory_order_relaxed);
    const unsigned long long since = g_since.exchange(at, std::memory_order_relaxed);
    const double secs = (since != 0 && at > since) ? (at - since) / 1000.0 : 0.0;

    const std::size_t taken = std::min(g_counterCount.load(std::memory_order_acquire),
                                       kMaxCounters);
    std::vector<std::pair<unsigned long long, const wchar_t*>> rows;
    rows.reserve(taken);
    unsigned long long total = 0;
    for (std::size_t i = 0; i < taken; ++i) {
        HookCounter* const counter = g_counters[i];
        if (counter == nullptr) {
            continue;
        }
        const unsigned long long count = counter->take();
        total += count;
        if (count != 0) {
            rows.emplace_back(count, counter->name());
        }
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::wstring line = std::format(L"Hooks: how often each one was called - {:.0f} s, "
                                    L"{} of {} came (total {})",
                                    secs, rows.size(), taken, total);
    for (const auto& [count, name] : rows) {
        const double perSec = secs > 0.0 ? static_cast<double>(count) / secs : 0.0;
        line += std::format(L" | {} {} ({:.0f}/s)", name, count, perSec);
    }
    log().info(L"{}", line);
    if (g_counterCount.load(std::memory_order_relaxed) > kMaxCounters) {
        log().warn(L"Hooks: ran out of counter slots ({} wanted, {} kept)",
                   g_counterCount.load(std::memory_order_relaxed), kMaxCounters);
    }
}

}
