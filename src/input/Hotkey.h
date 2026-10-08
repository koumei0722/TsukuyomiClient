#pragma once

#include <string>
#include <cstdint>
#include <atomic>
#include <thread>
#include <vector>
#include <utility>

#include "ui/Menu.h"

namespace tsukuyomi {

class Hotkey {
public:
    Hotkey() = default;
    explicit Hotkey(std::vector<int> combo);
    Hotkey(std::vector<int> combo, const char* output);

    void useLegacyOnly() { m_legacyOnly = true; }
    void set(std::vector<int> combo);
    void setPad(std::vector<int> combo);
    std::vector<int> padCombo() const;
    std::vector<int> combo() const;
    bool empty() const;

    std::wstring name() const;

    bool triggered();
    bool releasedAlone();

    bool isDown() const;

private:
    class Lock {
    public:
        Lock() = default;
        Lock(const Lock&) {}
        Lock& operator=(const Lock&) { return *this; }
        void lock() const
        {
            while (m_flag.test_and_set(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        void unlock() const { m_flag.clear(std::memory_order_release); }

    private:
        mutable std::atomic_flag m_flag = ATOMIC_FLAG_INIT;
    };
    struct Guard {
        explicit Guard(const Lock& lock) : m_lock(lock) { m_lock.lock(); }
        ~Guard() { m_lock.unlock(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        const Lock& m_lock;
    };
    bool isDownLocked() const;
    Lock m_lock;

    std::vector<int> m_combo;
    std::vector<int> m_padCombo;
    bool m_wasDown = false;
    bool m_legacyOnly = false;
    int m_slot = -1;
    const char* m_output = nullptr;
    std::uint64_t m_seenSeq = 0;
    std::uint64_t m_seenAloneSeq = 0;
};

inline void bindPad(MenuItem& item, Hotkey& hotkey)
{
    item.getPadKeys = [&hotkey] { return hotkey.padCombo(); };
    item.setPadKeys = [&hotkey](std::vector<int> combo) { hotkey.setPad(std::move(combo)); };
}

}
