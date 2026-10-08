#include "game/Abilities.h"

#include <Windows.h>

namespace tsukuyomi::abilities {

namespace {

int accessViolationFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

constexpr ptrdiff_t kContextLayeredOffset = 0x08;

bool readPointerGuarded(const void* address, std::byte*& value)
{
    __try {
        value = *static_cast<std::byte* const*>(address);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

}

bool readInt(const void* address, int& value)
{
    __try {
        value = *static_cast<const int*>(address);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool writeInt(void* address, int value)
{
    __try {
        *static_cast<int*>(address) = value;
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool readFloat(const void* address, float& value)
{
    __try {
        value = *static_cast<const float*>(address);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool writeFloat(void* address, float value)
{
    __try {
        *static_cast<float*>(address) = value;
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

std::byte* fromContext(void* context)
{
    if (context == nullptr) {
        return nullptr;
    }
    std::byte* layered = nullptr;
    if (!readPointerGuarded(static_cast<std::byte*>(context) + kContextLayeredOffset, layered)) {
        return nullptr;
    }
    return layered;
}

std::byte* at(std::byte* layered, int layer, int index)
{
    return layered + kArrayOffset + kLayerStride * layer + kStride * index;
}

bool looksValid(std::byte* layered)
{
    if (layered == nullptr) {
        return false;
    }

    int type = 0;
    if (!readInt(at(layered, kPlayerLayer, kFlySpeed) + kTypeOffset, type)
        || type != kTypeFloat) {
        return false;
    }
    if (!readInt(at(layered, kPlayerLayer, kWalkSpeed) + kTypeOffset, type)
        || type != kTypeFloat) {
        return false;
    }
    return true;
}

std::byte* slotOf(std::byte* layered, int index)
{
    for (int layer = kLayerCount - 1; layer >= 0; --layer) {
        std::byte* const slot = at(layered, layer, index);
        int type = 0;
        if (!readInt(slot + kTypeOffset, type)) {
            return nullptr;
        }
        if (type == kTypeBool || type == kTypeFloat) {
            return slot;
        }
    }

    return at(layered, kPlayerLayer, index);
}

void RestoreLedger::observeWorld(std::uint64_t world)
{
    if (m_world.load(std::memory_order_relaxed) != world) {
        m_world.store(world, std::memory_order_relaxed);
    }
}

int RestoreLedger::find(const void* who)
{
    const std::uint64_t world = m_world.load(std::memory_order_relaxed);
    for (size_t i = 0; i < kMax; ++i) {
        if (m_who[i].load(std::memory_order_relaxed) == who) {
            if (m_seen[i].load(std::memory_order_relaxed) != world) {
                m_seen[i].store(world, std::memory_order_relaxed);
            }
            return static_cast<int>(i);
        }
    }
    return -1;
}

void RestoreLedger::note(const void* who)
{
    if (who == nullptr) {
        return;
    }
    if (find(who) >= 0) {
        return;
    }
    const std::uint64_t world = m_world.load(std::memory_order_relaxed);
    for (size_t i = 0; i < kMax; ++i) {
        if (m_who[i].load(std::memory_order_relaxed) != nullptr) {
            continue;
        }
        m_seen[i].store(world, std::memory_order_seq_cst);
        const void* empty = nullptr;
        if (m_who[i].compare_exchange_strong(empty, who, std::memory_order_seq_cst)) {
            return;
        }
        if (empty == who) {
            return;
        }
    }
    for (size_t i = 0; i < kMax; ++i) {
        std::uint64_t seen = m_seen[i].load(std::memory_order_relaxed);
        if (seen == world) {
            continue;
        }
        if (!m_seen[i].compare_exchange_strong(seen, world, std::memory_order_seq_cst)) {
            continue;
        }
        m_dirty[i].store(false, std::memory_order_seq_cst);
        m_who[i].store(who, std::memory_order_seq_cst);
        return;
    }
}

void RestoreLedger::markAllDirty()
{
    for (size_t i = 0; i < kMax; ++i) {
        if (m_who[i].load(std::memory_order_relaxed) != nullptr) {
            m_dirty[i].store(true, std::memory_order_relaxed);
        }
    }
}

void RestoreLedger::clearDirty()
{
    for (size_t i = 0; i < kMax; ++i) {
        m_dirty[i].store(false, std::memory_order_relaxed);
    }
}

bool RestoreLedger::needsRestore(const void* who)
{
    const int i = find(who);
    if (i < 0) {
        return false;
    }
    return m_dirty[i].load(std::memory_order_relaxed);
}

void RestoreLedger::markDirty(const void* who)
{
    const int i = find(who);
    if (i >= 0) {
        m_dirty[i].store(true, std::memory_order_relaxed);
    }
}

void RestoreLedger::markClean(const void* who)
{
    const int i = find(who);
    if (i >= 0) {
        m_dirty[i].store(false, std::memory_order_relaxed);
    }
}

bool RestoreLedger::allClean() const
{
    const std::uint64_t world = m_world.load(std::memory_order_relaxed);
    for (size_t i = 0; i < kMax; ++i) {
        if (m_dirty[i].load(std::memory_order_relaxed) && m_who[i].load(std::memory_order_relaxed) != nullptr
            && m_seen[i].load(std::memory_order_relaxed) == world) {
            return false;
        }
    }
    return true;
}

}
