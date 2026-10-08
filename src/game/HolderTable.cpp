#include "game/HolderTable.h"

#include <algorithm>

namespace tsukuyomi {

void HolderTable::record(void* holder)
{
    if (holder == nullptr) {
        return;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    const unsigned long long seq = ++m_seq;
    for (Entry& one : m_entries) {
        if (one.holder == holder) {
            one.seenAt = seq;
            return;
        }
    }
    Entry* victim = &m_entries[0];
    for (Entry& one : m_entries) {
        if (one.holder == nullptr) {
            victim = &one;
            break;
        }
        if (one.seenAt < victim->seenAt) {
            victim = &one;
        }
    }
    victim->holder = holder;
    victim->seenAt = seq;
}

void HolderTable::forget(void* holder)
{
    if (holder == nullptr) {
        return;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (Entry& one : m_entries) {
        if (one.holder == holder) {
            one = Entry{};
        }
    }
}

std::size_t HolderTable::snapshot(void** out, std::size_t capacity) const
{
    std::array<Entry, kCapacity> copy{};
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        copy = m_entries;
    }
    std::sort(copy.begin(), copy.end(), [](const Entry& a, const Entry& b) { return a.seenAt > b.seenAt; });
    std::size_t count = 0;
    for (const Entry& one : copy) {
        if (one.holder != nullptr && count < capacity) {
            out[count++] = one.holder;
        }
    }
    return count;
}

NetIdMatch compareNetIds(const std::int32_t* a, const std::int32_t* b, std::size_t count)
{
    NetIdMatch match;
    if (a == nullptr || b == nullptr) {
        return match;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (a[i] == 0 || b[i] == 0) {
            continue;
        }
        if (a[i] == b[i]) {
            ++match.same;
        } else {
            ++match.different;
        }
    }
    return match;
}

}
