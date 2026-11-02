#include "game/PlaceAmount.h"

#include "core/Logger.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>

namespace tsukuyomi::placeamount {
namespace {

struct Amount {
    std::int32_t kind;
    std::int32_t value;
    std::uint8_t clampToStack;
};

std::atomic<int> g_next{0};
std::atomic<bool> g_available{false};
std::atomic<int> g_logs{0};

bool rewriteRaw(Amount* a, int items)
{
    __try {
        if (a->kind != 1 || a->value != 2) {
            return false;
        }
        a->kind = 0;
        a->value = items;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}

void setNext(int items)
{
    g_next.store(items, std::memory_order_relaxed);
}

void clear()
{
    g_next.store(0, std::memory_order_relaxed);
}

bool available()
{
    return g_available.load(std::memory_order_relaxed);
}

void markAvailable()
{
    g_available.store(true, std::memory_order_relaxed);
}

void apply(void* amount)
{
    const int items = g_next.exchange(0, std::memory_order_relaxed);
    if (items <= 0 || amount == nullptr) {
        return;
    }
    const bool done = rewriteRaw(static_cast<Amount*>(amount), items);
    if (g_logs.fetch_add(1, std::memory_order_relaxed) < 3) {
        log().info(L"PlaceAmount: {} items in one click{}", items, done ? L"" : L" (not the place-one shape; left as is)");
    }
}

}
