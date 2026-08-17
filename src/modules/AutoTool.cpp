#include "modules/AutoTool.h"

#include "core/Logger.h"
#include "hooks/Detours.h"
#include "input/Foreground.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <algorithm>

namespace tsukuyomi {

namespace {

int accessViolationFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

bool readIntGuarded(const void* address, int& value)
{
    __try {
        value = *static_cast<const int*>(address);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool writeIntGuarded(void* address, int value)
{
    __try {
        *static_cast<int*>(address) = value;
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool probeSlotSpeeds(void** itemSlot, std::byte* slotZero, void* savedItem, ptrdiff_t stride,
                     int slotCount, void* rcx, void* rdx, void* r8, void* r9, float* speeds)
{
    __try {
        for (int slot = 0; slot < slotCount; ++slot) {
            *itemSlot = slotZero + stride * slot;
            speeds[slot] = hooks::callGetDestroySpeed(rcx, rdx, r8, r9);
        }

        *itemSlot = savedItem;
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        *itemSlot = savedItem;
        return false;
    }
}

}

static bool readSlotValue(void* holder, ptrdiff_t offset, int slotCount, int& slot)
{
    if (holder == nullptr) {
        return false;
    }
    const void* const field = static_cast<const std::byte*>(holder) + offset;
    int value = -1;
    if (!readIntGuarded(field, value)) {
        return false;
    }
    if (value < 0 || value >= slotCount) {
        return false;
    }
    slot = value;
    return true;
}

AutoTool& AutoTool::instance()
{
    static AutoTool module;
    return module;
}

bool AutoTool::available() const
{
    const Scanner& scanner = Scanner::instance();
    return scanner.found(Target::GetDestroySpeed) && scanner.found(Target::SetSelectedSlot);
}

void AutoTool::onScansReady()
{
    if (!available() && enabled()) {
        log().warn(L"AutoTool: required functions not found, disabling");
        setEnabled(false);
    }
}

void AutoTool::forgetSwitch()
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    m_switched = false;
    m_switchedA = nullptr;
    m_switchedB = nullptr;
    m_originalSlot = -1;
    m_manualSlot = -1;
    m_guardUntil = Clock::time_point{};
}

void AutoTool::dropHolder(void* expected, const wchar_t* reason)
{
    if (expected == nullptr) {
        return;
    }

    void* current = expected;
    bool dropped = m_holder.compare_exchange_strong(current, nullptr, std::memory_order_acq_rel);
    current = expected;
    dropped = m_holderAlt.compare_exchange_strong(current, nullptr, std::memory_order_acq_rel)
              || dropped;
    if (!dropped) {

        return;
    }

    forgetSwitch();
    log().warn(L"AutoTool: dropped the slot holder ({}). Switch hotbar slots once to recover",
               reason);
}

int AutoTool::readSelectedSlot()
{

    void* const holder = m_holder.load(std::memory_order_acquire);
    if (holder == nullptr) {
        return -1;
    }

    int slot = -1;
    if (!readSlotValue(holder, kSelectedSlotOffset, kSlotCount, slot)) {
        dropHolder(holder, L"selected slot is not readable or out of range");
        return -1;
    }
    return slot;
}

bool AutoTool::applySlot(int slot)
{
    if (slot < 0 || slot >= kSlotCount) {
        return false;
    }

    void* const targets[] = {m_holder.load(std::memory_order_acquire),
                             m_holderAlt.load(std::memory_order_acquire)};

    constexpr size_t kTargetCount = 2;
    bool wrote = false;
    for (size_t i = 0; i < kTargetCount; ++i) {
        void* const holder = targets[i];
        if (holder == nullptr || (i > 0 && holder == targets[0])) {
            continue;
        }

        int current = -1;
        if (!readSlotValue(holder, kSelectedSlotOffset, kSlotCount, current)) {
            dropHolder(holder, L"selected slot is not readable or out of range");
            continue;
        }

        void* const field = static_cast<std::byte*>(holder) + kSelectedSlotOffset;
        if (writeIntGuarded(field, slot)) {
            wrote = true;
        } else {
            dropHolder(holder, L"memory is not writable");
        }
    }
    return wrote;
}

void AutoTool::restoreSlot()
{
    void* switchedA = nullptr;
    void* switchedB = nullptr;
    int original = -1;
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!m_switched) {
            return;
        }
        switchedA = m_switchedA;
        switchedB = m_switchedB;
        original = m_originalSlot;

        m_switched = false;
        m_switchedA = nullptr;
        m_switchedB = nullptr;
        m_originalSlot = -1;

        m_guardUntil = Clock::now() + std::chrono::milliseconds(kGuardMs);
    }

    if (original < 0) {
        return;
    }

    void* const holder = m_holder.load(std::memory_order_acquire);
    void* const alt = m_holderAlt.load(std::memory_order_acquire);
    const bool sameWorld = (switchedA != nullptr && (switchedA == holder || switchedA == alt))
                           || (switchedB != nullptr && (switchedB == holder || switchedB == alt));
    if (!sameWorld) {
        return;
    }

    if (applySlot(original)) {
        log().info(L"AutoTool: restored slot {}", original + 1);
    }
}

void AutoTool::noteHolderChange(void* previous, void* current)
{
    const int count = m_holderChanges.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > kHolderLogLimit) {
        return;
    }

    log().info(L"AutoTool: slot holder {:#x} -> {:#x}", reinterpret_cast<uintptr_t>(previous),
               reinterpret_cast<uintptr_t>(current));
    if (count == kHolderLogLimit) {
        log().warn(L"AutoTool: the slot holder keeps changing, further changes are not logged");
    }
}

void AutoTool::onSetSelectedSlot(void* rcx, void* rdx, void* r8, void* r9)
{

    if (rcx != nullptr) {
        void* const previous = m_holder.exchange(rcx, std::memory_order_acq_rel);
        if (previous != rcx) {

            void* const alt = m_holderAlt.exchange(previous, std::memory_order_acq_rel);

            if (rcx != alt) {
                noteHolderChange(previous, rcx);
            }
        }
    }

    const auto slot = static_cast<int>(reinterpret_cast<uintptr_t>(rdx));

    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);

        const bool guarded = m_switched || Clock::now() < m_guardUntil;
        if (!guarded && slot >= 0 && slot < kSlotCount) {
            m_manualSlot = slot;
        }
    }

    hooks::callSetSelectedSlot(rcx, rdx, r8, r9);
}

float AutoTool::onGetDestroySpeed(void* rcx, void* rdx, void* r8, void* r9)
{

    const auto mode = reinterpret_cast<uintptr_t>(rdx);
    const bool mining = (mode == 0xB || mode == 0x1);

    if (!enabled() || !mining || rcx == nullptr) {
        return hooks::callGetDestroySpeed(rcx, rdx, r8, r9);
    }

    const int currentSlot = readSelectedSlot();
    if (currentSlot < 0) {
        return hooks::callGetDestroySpeed(rcx, rdx, r8, r9);
    }

    m_lastSpeedQuery.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);

    auto** const itemSlot =
        reinterpret_cast<void**>(static_cast<std::byte*>(rcx) + kItemPointerOffset);
    void* const savedItem = *itemSlot;
    if (savedItem == nullptr) {
        return hooks::callGetDestroySpeed(rcx, rdx, r8, r9);
    }

    auto* const slotZero = static_cast<std::byte*>(savedItem) - kSlotStride * currentSlot;

    if (!memory::isReadable(slotZero, static_cast<size_t>(kSlotStride) * kSlotCount)) {
        dropHolder(m_holder.load(std::memory_order_acquire), L"slot array is out of range");
        return hooks::callGetDestroySpeed(rcx, rdx, r8, r9);
    }

    float speeds[kSlotCount] = {};
    if (!probeSlotSpeeds(itemSlot, slotZero, savedItem, kSlotStride, kSlotCount, rcx, rdx, r8, r9,
                         speeds)) {

        dropHolder(m_holder.load(std::memory_order_acquire), L"probing faulted");
        log().error(L"AutoTool: probing the hotbar faulted inside the game, disabling");
        setEnabled(false);
        return hooks::callGetDestroySpeed(rcx, rdx, r8, r9);
    }

    float best = speeds[0];
    for (const float speed : speeds) {
        best = (std::max)(best, speed);
    }

    constexpr float kEpsilon = 0.0001f;
    int bestSlot = currentSlot;
    if (speeds[currentSlot] < best - kEpsilon) {
        for (int slot = 0; slot < kSlotCount; ++slot) {
            if (speeds[slot] >= best - kEpsilon) {
                bestSlot = slot;
                break;
            }
        }
    }

    if (bestSlot != currentSlot) {
        int fromSlot = -1;
        {
            const std::lock_guard<std::mutex> lock(m_stateMutex);
            if (!m_switched) {
                m_originalSlot = (m_manualSlot >= 0) ? m_manualSlot : currentSlot;
                m_switchedA = m_holder.load(std::memory_order_acquire);
                m_switchedB = m_holderAlt.load(std::memory_order_acquire);
                m_switched = true;
                fromSlot = m_originalSlot;
            }
        }
        if (fromSlot >= 0) {
            log().info(L"AutoTool: slot {} -> {} (speed {:.1f} -> {:.1f})", fromSlot + 1,
                       bestSlot + 1, speeds[currentSlot], best);
        }
        applySlot(bestSlot);
    }

    return speeds[currentSlot];
}

void AutoTool::onUpdate()
{
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!m_switched) {
            return;
        }
    }

    const auto lastQuery =
        Clock::time_point(Clock::duration(m_lastSpeedQuery.load(std::memory_order_relaxed)));

    const bool idle = (Clock::now() - lastQuery) > std::chrono::milliseconds(kIdleRestoreMs);
    const bool released = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0;

    const bool unfocused = !input::isInGameplay();

    if (idle || released || unfocused) {
        restoreSlot();
    }
}

void AutoTool::onEnabledChanged(bool enabled)
{
    if (enabled) {
        if (m_holder.load(std::memory_order_acquire) == nullptr) {

            log().info(L"AutoTool: switch hotbar slots once to activate");
        }
    } else {
        restoreSlot();
    }
}

void AutoTool::shutdown()
{
    restoreSlot();
}

}
