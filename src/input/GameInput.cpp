#include "input/GameInput.h"

#include <Windows.h>

#include <atomic>

namespace tsukuyomi::input {

namespace {
std::atomic<unsigned long long> g_sneakSeenMs{0};
}

void noteRawMoveBits(std::uint32_t bits)
{
    if ((bits & kRawSneakBit) != 0) {
        g_sneakSeenMs.store(GetTickCount64(), std::memory_order_release);
    }
}

bool sneakHeldWithin(unsigned long long ms)
{
    const unsigned long long at = g_sneakSeenMs.load(std::memory_order_acquire);
    return at != 0 && GetTickCount64() - at <= ms;
}

}
