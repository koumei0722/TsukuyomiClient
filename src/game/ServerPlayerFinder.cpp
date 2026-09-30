#include "game/ServerPlayerFinder.h"

#include "core/Logger.h"
#include "game/GameData.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>

namespace tsukuyomi::serverfind {

namespace {

std::mutex g_lock;
std::thread g_worker;
std::atomic<bool> g_running{false};
std::atomic<bool> g_stop{false};
std::atomic<unsigned long long> g_triedSerial{0};

LONG accessFilter(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER
                                                                                  : EXCEPTION_CONTINUE_SEARCH;
}

bool scanRegion(const std::uintptr_t* begin, std::size_t words, std::uintptr_t needle, void** out, int cap, int& found)
{
    __try {
        for (std::size_t i = 0; i < words && found < cap; ++i) {
            if (begin[i] == needle) {
                out[found++] = const_cast<std::uintptr_t*>(begin + i);
            }
        }
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

void work(const void* vtable)
{
    constexpr int kCap = 32;
    void* hits[kCap] = {};
    int found = 0;
    MEMORY_BASIC_INFORMATION info{};
    for (auto* at = reinterpret_cast<std::uint8_t*>(std::uintptr_t{0x10000});
         !g_stop.load(std::memory_order_relaxed) && found < kCap
         && VirtualQuery(at, &info, sizeof(info)) == sizeof(info);
         at = static_cast<std::uint8_t*>(info.BaseAddress) + info.RegionSize) {
        if (info.State != MEM_COMMIT || info.Type != MEM_PRIVATE || info.Protect != PAGE_READWRITE
            || info.RegionSize > (256ull << 20)) {
            continue;
        }
        scanRegion(static_cast<const std::uintptr_t*>(info.BaseAddress), info.RegionSize / 8,
                   reinterpret_cast<std::uintptr_t>(vtable), hits, kCap, found);
    }
    if (g_stop.load(std::memory_order_relaxed)) {
        return;
    }
    GameData& data = GameData::instance();
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    const bool haveSelf = data.rawPlayerPos(px, py, pz);
    void* best = nullptr;
    double bestDistance = 1e30;
    for (int i = 0; i < found; ++i) {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (!data.isServerPlayer(hits[i]) || !GameData::rawPosOf(hits[i], x, y, z)) {
            continue;
        }
        const double distance = haveSelf ? std::hypot(x - px, y - py, z - pz) : 0.0;
        if (best == nullptr || distance < bestDistance) {
            best = hits[i];
            bestDistance = distance;
        }
    }
    if (best != nullptr && data.setPlayerAlt(best)) {
        log().info(L"DebugScreen: found the server-side player on the heap ({} candidate(s))", found);
    } else {
        log().info(L"DebugScreen: the server-side player was not found on the heap ({} candidate(s))", found);
    }
}

}

void requestIfMissing(bool localServerRunning)
{
    GameData& data = GameData::instance();
    const void* const vtable = data.serverPlayerVtable();
    const unsigned long long serial = data.playerSerial();
    if (!localServerRunning || vtable == nullptr || data.playerAlt() != nullptr || serial == 0
        || g_triedSerial.load(std::memory_order_relaxed) == serial || g_running.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_running.load(std::memory_order_acquire) || g_stop.load(std::memory_order_relaxed)) {
        return;
    }
    if (g_worker.joinable()) {
        g_worker.join();
    }
    g_triedSerial.store(serial, std::memory_order_relaxed);
    g_running.store(true, std::memory_order_release);
    g_worker = std::thread([vtable] {
        work(vtable);
        g_running.store(false, std::memory_order_release);
    });
}

void shutdown()
{
    g_stop.store(true, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_worker.joinable()) {
        g_worker.join();
    }
}

}
