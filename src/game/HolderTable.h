#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace tsukuyomi {

class HolderTable {
public:
    static constexpr std::size_t kCapacity = 16;

    void record(void* holder);
    void forget(void* holder);
    void clear();
    std::size_t snapshot(void** out, std::size_t capacity) const;
    std::size_t size() const;

private:
    struct Entry {
        void* holder = nullptr;
        unsigned long long seenAt = 0;
    };
    mutable std::mutex m_mutex;
    std::array<Entry, kCapacity> m_entries{};
    unsigned long long m_seq = 0;
};

struct NetIdMatch {
    int same = 0;
    int different = 0;
    bool sameOwner() const { return same >= 1 && same > different; }
};
NetIdMatch compareNetIds(const std::int32_t* a, const std::int32_t* b, std::size_t count);

}
