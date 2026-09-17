#include "core/Perf.h"

#include "core/Logger.h"
#include "core/Paths.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>

namespace tsukuyomi::perf {
namespace {

struct Bucket {
    std::atomic<long long> ticks{0};
    std::atomic<long long> calls{0};
    std::atomic<long long> worst{0};
};

std::array<Bucket, static_cast<std::size_t>(Slot::Count)> g_buckets;

std::atomic<long long> g_frameTicks{0};
std::atomic<long long> g_frameWorst{0};
std::atomic<long long> g_frames{0};
long long g_lastFrameAt = 0;

std::atomic<unsigned long long> g_reportedAt{0};

const wchar_t* const kNames[] = {
    L"Schematica",  L"load",       L"cells",      L"draw",      L"clear",
    L"prune",       L"diff",       L"review",     L"restore",   L"fix-taken",
    L"place-actors", L"dirty",     L"publish",    L"lookup",    L"tessellate",
    L"chunk-build", L"ask-builds",
    L"HandRestock", L"hr-resolve", L"hr-count",   L"hr-watch",  L"hr-client",
};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<std::size_t>(Slot::Count));

long long frequency()
{
    static const long long value = [] {
        LARGE_INTEGER li{};
        QueryPerformanceFrequency(&li);
        return li.QuadPart != 0 ? li.QuadPart : 1;
    }();
    return value;
}

double toMs(long long ticks)
{
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(frequency());
}

constexpr unsigned long long kReportMs = 5000;

}

bool on()
{

    static std::atomic<int> armed{0};
    static std::atomic<unsigned long long> readAt{0};
    const unsigned long long at = GetTickCount64();
    const unsigned long long last = readAt.load(std::memory_order_acquire);
    if (last != 0 && at - last < 500) {
        return armed.load(std::memory_order_relaxed) == 1;
    }
    readAt.store(at, std::memory_order_release);
    std::error_code ec;
    const bool want = std::filesystem::exists(paths::dataDir() / L"diag-perf.txt", ec);
    const int was = armed.exchange(want ? 1 : 2, std::memory_order_release);
    if (was != 0 && was != (want ? 1 : 2)) {
        log().info(L"Perf: diag-perf.txt = {}", want ? L"on" : L"off");
        if (want) {
            reset();
        }
    }
    return want;
}

long long now()
{
    if (!on()) {
        return 0;
    }
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);

    return li.QuadPart != 0 ? li.QuadPart : 1;
}

void add(Slot slot, long long began)
{
    if (began == 0) {
        return;
    }
    const auto index = static_cast<std::size_t>(slot);
    if (index >= g_buckets.size()) {
        return;
    }
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    const long long spent = li.QuadPart - began;
    if (spent < 0) {
        return;
    }
    Bucket& bucket = g_buckets[index];
    bucket.ticks.fetch_add(spent, std::memory_order_relaxed);
    bucket.calls.fetch_add(1, std::memory_order_relaxed);

    long long worst = bucket.worst.load(std::memory_order_relaxed);
    while (spent > worst
           && !bucket.worst.compare_exchange_weak(worst, spent, std::memory_order_relaxed)) {
    }
}

void endFrame()
{
    if (!on()) {
        g_lastFrameAt = 0;
        return;
    }
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    const long long at = li.QuadPart;
    if (g_lastFrameAt != 0) {
        const long long spent = at - g_lastFrameAt;

        if (spent > 0 && spent < frequency()) {
            g_frameTicks.fetch_add(spent, std::memory_order_relaxed);
            g_frames.fetch_add(1, std::memory_order_relaxed);
            long long worst = g_frameWorst.load(std::memory_order_relaxed);
            while (spent > worst
                   && !g_frameWorst.compare_exchange_weak(worst, spent,
                                                          std::memory_order_relaxed)) {
            }
        }
    }
    g_lastFrameAt = at;
}

void reset()
{
    for (Bucket& bucket : g_buckets) {
        bucket.ticks.store(0, std::memory_order_relaxed);
        bucket.calls.store(0, std::memory_order_relaxed);
        bucket.worst.store(0, std::memory_order_relaxed);
    }
    g_frameTicks.store(0, std::memory_order_relaxed);
    g_frameWorst.store(0, std::memory_order_relaxed);
    g_frames.store(0, std::memory_order_relaxed);
    g_lastFrameAt = 0;
}

void maybeReport()
{
    if (!on()) {
        return;
    }
    const unsigned long long at = GetTickCount64();
    const unsigned long long was = g_reportedAt.load(std::memory_order_relaxed);
    if (was != 0 && at - was < kReportMs) {
        return;
    }
    g_reportedAt.store(at, std::memory_order_relaxed);
    const long long frames = g_frames.exchange(0, std::memory_order_relaxed);
    const long long frameTicks = g_frameTicks.exchange(0, std::memory_order_relaxed);
    const long long frameWorst = g_frameWorst.exchange(0, std::memory_order_relaxed);
    if (frames == 0) {
        return;
    }
    const double avg = toMs(frameTicks) / static_cast<double>(frames);
    const double fps = avg > 0.0 ? 1000.0 / avg : 0.0;

    std::wstring line = std::format(L"Perf: {} frames / {:.2f} ms avg ({:.0f} fps) "
                                    L"/ {:.2f} ms worst |",
                                    frames, avg, fps, toMs(frameWorst));
    for (std::size_t i = 0; i < g_buckets.size(); ++i) {
        Bucket& bucket = g_buckets[i];
        const long long calls = bucket.calls.exchange(0, std::memory_order_relaxed);
        const long long ticks = bucket.ticks.exchange(0, std::memory_order_relaxed);
        const long long worst = bucket.worst.exchange(0, std::memory_order_relaxed);
        if (calls == 0) {
            continue;
        }

        line += std::format(L" {} {}x/{:.3f}ms(worst {:.2f})", kNames[i], calls,
                            toMs(ticks) / static_cast<double>(frames), toMs(worst));
    }
    log().info(L"{}", line);
}

}
