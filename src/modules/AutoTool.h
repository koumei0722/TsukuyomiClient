#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>

#include "modules/Module.h"

namespace tsukuyomi {

class AutoTool : public Module {
public:
    static AutoTool& instance();

    const wchar_t* name() const override { return L"AutoTool"; }
    bool available() const override;

    void onScansReady() override;
    void shutdown() override;

    float onGetDestroySpeed(void* rcx, void* rdx, void* r8, void* r9);
    void onSetSelectedSlot(void* rcx, void* rdx, void* r8, void* r9);

protected:
    void onEnabledChanged(bool enabled) override;
    void onUpdate() override;

private:
    AutoTool() = default;

    using Clock = std::chrono::steady_clock;

    void restoreSlot();

    bool applySlot(int slot);

    int readSelectedSlot();

    void dropHolder(void* expected, const wchar_t* reason);

    void forgetSwitch();
    void noteHolderChange(void* previous, void* current);

    static constexpr int kSlotCount = 9;
    static constexpr ptrdiff_t kSelectedSlotOffset = 0x10;

    static constexpr ptrdiff_t kItemPointerOffset = 0x10;
    static constexpr ptrdiff_t kSlotStride = 0x98;

    static constexpr int kIdleRestoreMs = 250;

    static constexpr int kGuardMs = 250;

    static constexpr int kHolderLogLimit = 64;

    std::atomic<void*> m_holder{nullptr};
    std::atomic<void*> m_holderAlt{nullptr};
    std::atomic<int> m_holderChanges{0};
    std::atomic<Clock::rep> m_lastSpeedQuery{0};

    std::mutex m_stateMutex;
    int m_manualSlot = -1;
    int m_originalSlot = -1;

    void* m_switchedA = nullptr;
    void* m_switchedB = nullptr;
    bool m_switched = false;
    Clock::time_point m_guardUntil{};
};

}
