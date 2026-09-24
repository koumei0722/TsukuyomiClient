#pragma once

#include <atomic>
#include <cstddef>

namespace tsukuyomi::hooks {

class HookCounter {
public:
    explicit HookCounter(const wchar_t* name);

    void tick()
    {
        if (!counting()) {
            return;
        }
        m_count.fetch_add(1, std::memory_order_relaxed);
    }

    const wchar_t* name() const { return m_name; }
    unsigned long long take() { return m_count.exchange(0, std::memory_order_relaxed); }

    static bool counting();

private:
    const wchar_t* m_name = nullptr;
    std::atomic<unsigned long long> m_count{0};
};

void setCountingHooks(bool on);
bool countingHooks();

void reportHookCounts();

}

#define TSUKUYOMI_HOOK_COUNT(name)                                      \
    do {                                                                \
        static ::tsukuyomi::hooks::HookCounter s_tsukuyomiHookCounter{L"" name}; \
        s_tsukuyomiHookCounter.tick();                                  \
    } while (false)
