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

}

bool readByte(const void* address, std::uint8_t& value)
{
    __try {
        value = *static_cast<const std::uint8_t*>(address);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool writeByte(void* address, std::uint8_t value)
{
    __try {
        *static_cast<std::uint8_t*>(address) = value;
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

std::byte* at(std::byte* layered, int layer, int index)
{
    if (layered == nullptr || layer < 0 || layer >= kLayerCount || index < 0 || index >= kCount) {
        return nullptr;
    }
    return layered + kArrayOffset + kLayerStride * layer + kStride * index;
}

bool looksValid(std::byte* layered)
{
    if (layered == nullptr) {
        return false;
    }

    std::uint8_t type = 0;
    if (!readByte(at(layered, kPlayerLayer, kFlySpeed) + kTypeOffset, type)
        || type != kTypeFloat) {
        return false;
    }
    if (!readByte(at(layered, kPlayerLayer, kWalkSpeed) + kTypeOffset, type)
        || type != kTypeFloat) {
        return false;
    }
    return true;
}

std::byte* slotOf(std::byte* layered, int index)
{
    if (layered == nullptr || index < 0 || index >= kCount) {
        return nullptr;
    }
    for (int layer = kLayerCount - 1; layer >= 0; --layer) {
        std::byte* const slot = at(layered, layer, index);
        std::uint8_t type = 0;
        if (!readByte(slot + kTypeOffset, type)) {
            return nullptr;
        }
        if (type != kTypeUnset) {
            return type == kTypeBool || type == kTypeFloat ? slot : nullptr;
        }
    }

    return at(layered, kPlayerLayer, index);
}

bool readBool(std::byte* layered, int index)
{
    std::byte* const slot = slotOf(layered, index);
    std::uint8_t type = 0;
    std::uint8_t value = 0;
    return slot != nullptr && readByte(slot + kTypeOffset, type) && type == kTypeBool
        && readByte(slot + kValueOffset, value) && value == 1;
}

bool setBool(std::byte* layered, int index, bool wanted)
{
    std::byte* const slot = slotOf(layered, index);
    std::uint8_t type = 0;
    if (slot == nullptr || !readByte(slot + kTypeOffset, type)
        || (type != kTypeUnset && type != kTypeBool)) {
        return false;
    }
    if (type == kTypeBool) {
        std::uint8_t value = 0;
        if (!readByte(slot + kValueOffset, value)) {
            return false;
        }
        if (value == static_cast<std::uint8_t>(wanted)) {
            return true;
        }
    }

    if (writeByte(slot + kValueOffset, static_cast<std::uint8_t>(wanted)) && type == kTypeUnset) {
        writeByte(slot + kTypeOffset, kTypeBool);
    }
    return false;
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
