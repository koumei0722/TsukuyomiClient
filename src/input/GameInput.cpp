#include "input/GameInput.h"

#include <Windows.h>

#include <atomic>

namespace tsukuyomi::input {

namespace {
std::atomic<unsigned long long> g_sneakSeenMs{0};
std::atomic<std::uint64_t> g_sneakHoldUntilInput{0};
std::atomic<std::uint64_t> g_sneakForcedSince{0};
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

void holdSneakUntilInput(std::uint64_t inputs)
{
    g_sneakHoldUntilInput.store(inputs, std::memory_order_release);
}

bool sneakHoldWanted(std::uint64_t inputsSent)
{
    return inputsSent < g_sneakHoldUntilInput.load(std::memory_order_acquire);
}

void noteSneakForced(bool forced, std::uint64_t inputsSent)
{
    if (!forced) g_sneakForcedSince.store(0, std::memory_order_release);
    else if (g_sneakForcedSince.load(std::memory_order_acquire) == 0) g_sneakForcedSince.store(inputsSent + 1, std::memory_order_release);
}

bool sneakForcedSince(std::uint64_t& inputsSent)
{
    const std::uint64_t since = g_sneakForcedSince.load(std::memory_order_acquire);
    if (since == 0) return false;
    inputsSent = since - 1;
    return true;
}

}
