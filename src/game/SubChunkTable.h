#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace subchunks {

class Table {
public:
    static constexpr std::size_t kPageBits = 12;
    static constexpr std::size_t kPageSize = std::size_t{1} << kPageBits;
    static constexpr std::size_t kMaxPages = 1024;
    static constexpr std::size_t kMaxSlots = kPageSize * kMaxPages;
    static constexpr std::size_t kNone = ~std::size_t{0};
    static constexpr std::size_t kBuckets = std::size_t{1} << 18;

    struct Slot {
        std::atomic<int> baseX{0};
        std::atomic<int> baseY{0};
        std::atomic<int> baseZ{0};
        std::atomic<void*> ptr[2] = {};
        std::atomic<bool> used{false};
        std::atomic<std::uint32_t> next{0};
    };

    Table() = default;
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
    ~Table()
    {
        for (auto& page : slotPages_) {
            delete[] page.exchange(nullptr);
        }
        for (auto& page : notePages_) {
            delete[] page.exchange(nullptr);
        }
    }

    std::size_t size() const
    {
        return std::min(count_.load(std::memory_order_acquire), kMaxSlots);
    }

    Slot* slot(std::size_t i) const
    {
        if (i >= kMaxSlots) {
            return nullptr;
        }
        Slot* const page = slotPages_[i >> kPageBits].load(std::memory_order_acquire);
        return page != nullptr ? &page[i & (kPageSize - 1)] : nullptr;
    }

    std::size_t find(int baseX, int baseY, int baseZ) const
    {
        std::uint32_t v = baseHeads_[baseHash(baseX, baseY, baseZ)].load(std::memory_order_acquire);
        for (std::size_t steps = 0; v != 0 && steps <= kMaxSlots; ++steps) {
            const std::size_t i = v - 1;
            const Slot* const s = slot(i);
            if (s == nullptr) {
                return kNone;
            }
            if (s->used.load(std::memory_order_acquire)
                && s->baseX.load(std::memory_order_relaxed) == baseX
                && s->baseY.load(std::memory_order_relaxed) == baseY
                && s->baseZ.load(std::memory_order_relaxed) == baseZ) {
                return i;
            }
            v = s->next.load(std::memory_order_acquire);
        }
        return kNone;
    }

    std::size_t add(int baseX, int baseY, int baseZ)
    {
        if (count_.load(std::memory_order_acquire) >= kMaxSlots) {
            return kNone;
        }
        const std::size_t i = count_.fetch_add(1, std::memory_order_acq_rel);
        if (i >= kMaxSlots) {
            return kNone;
        }
        Slot* const s = ensureSlotPage(i);
        if (s == nullptr) {
            return kNone;
        }
        s->baseX.store(baseX, std::memory_order_relaxed);
        s->baseY.store(baseY, std::memory_order_relaxed);
        s->baseZ.store(baseZ, std::memory_order_relaxed);
        s->ptr[0].store(nullptr, std::memory_order_relaxed);
        s->ptr[1].store(nullptr, std::memory_order_relaxed);
        s->used.store(true, std::memory_order_release);
        auto& head = baseHeads_[baseHash(baseX, baseY, baseZ)];
        std::uint32_t was = head.load(std::memory_order_acquire);
        do {
            s->next.store(was, std::memory_order_release);
        } while (!head.compare_exchange_weak(was, static_cast<std::uint32_t>(i + 1),
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire));
        return i;
    }

    bool notePointer(void* p, std::size_t i)
    {
        if (p == nullptr || i >= kMaxSlots) {
            return true;
        }
        if (noteCount_.load(std::memory_order_acquire) >= kMaxSlots) {
            return false;
        }
        const std::size_t n = noteCount_.fetch_add(1, std::memory_order_acq_rel);
        if (n >= kMaxSlots) {
            return false;
        }
        Note* const note = ensureNotePage(n);
        if (note == nullptr) {
            return false;
        }
        note->ptr.store(p, std::memory_order_relaxed);
        note->slot.store(static_cast<std::uint32_t>(i + 1), std::memory_order_relaxed);
        auto& head = ptrHeads_[ptrHash(p)];
        std::uint32_t was = head.load(std::memory_order_acquire);
        do {
            note->next.store(was, std::memory_order_release);
        } while (!head.compare_exchange_weak(was, static_cast<std::uint32_t>(n + 1),
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire));
        return true;
    }

    std::size_t findByPointer(const void* p) const
    {
        if (p == nullptr) {
            return kNone;
        }
        std::uint32_t v = ptrHeads_[ptrHash(p)].load(std::memory_order_acquire);
        for (std::size_t steps = 0; v != 0 && steps <= kMaxSlots; ++steps) {
            const Note* const note = notePage(v - 1);
            if (note == nullptr) {
                return kNone;
            }
            if (note->ptr.load(std::memory_order_relaxed) == p) {
                const std::uint32_t si = note->slot.load(std::memory_order_relaxed);
                const Slot* const s = si != 0 ? slot(si - 1) : nullptr;
                if (s != nullptr && s->used.load(std::memory_order_acquire)
                    && (s->ptr[0].load(std::memory_order_relaxed) == p
                        || s->ptr[1].load(std::memory_order_relaxed) == p)) {
                    return si - 1;
                }
            }
            v = note->next.load(std::memory_order_acquire);
        }
        return kNone;
    }

    void clear()
    {
        const std::size_t used = size();
        count_.store(0, std::memory_order_release);
        for (std::size_t i = 0; i < used; ++i) {
            if (Slot* const s = slot(i)) {
                s->used.store(false, std::memory_order_release);
                s->ptr[0].store(nullptr, std::memory_order_relaxed);
                s->ptr[1].store(nullptr, std::memory_order_relaxed);
            }
        }
        for (auto& head : baseHeads_) {
            head.store(0, std::memory_order_release);
        }
        for (auto& head : ptrHeads_) {
            head.store(0, std::memory_order_release);
        }
        noteCount_.store(0, std::memory_order_release);
    }

private:
    struct Note {
        std::atomic<void*> ptr{nullptr};
        std::atomic<std::uint32_t> slot{0};
        std::atomic<std::uint32_t> next{0};
    };

    static std::size_t mix(std::uint64_t key)
    {
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdULL;
        key ^= key >> 33;
        return static_cast<std::size_t>(key) & (kBuckets - 1);
    }

    static std::size_t baseHash(int baseX, int baseY, int baseZ)
    {
        const std::uint64_t ux = static_cast<std::uint32_t>(baseX >> 4) & 0x1FFFFFu;
        const std::uint64_t uy = static_cast<std::uint32_t>(baseY >> 4) & 0x1FFFFFu;
        const std::uint64_t uz = static_cast<std::uint32_t>(baseZ >> 4) & 0x1FFFFFu;
        return mix((ux << 42) | (uy << 21) | uz);
    }

    static std::size_t ptrHash(const void* p)
    {
        return mix(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)));
    }

    template <class T>
    static T* ensurePage(std::atomic<T*>& at)
    {
        T* page = at.load(std::memory_order_acquire);
        if (page != nullptr) {
            return page;
        }
        T* const fresh = new (std::nothrow) T[kPageSize];
        if (fresh == nullptr) {
            return nullptr;
        }
        if (at.compare_exchange_strong(page, fresh, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
            return fresh;
        }
        delete[] fresh;
        return page;
    }

    Slot* ensureSlotPage(std::size_t i)
    {
        Slot* const page = ensurePage(slotPages_[i >> kPageBits]);
        return page != nullptr ? &page[i & (kPageSize - 1)] : nullptr;
    }

    Note* ensureNotePage(std::size_t n)
    {
        Note* const page = ensurePage(notePages_[n >> kPageBits]);
        return page != nullptr ? &page[n & (kPageSize - 1)] : nullptr;
    }

    const Note* notePage(std::size_t n) const
    {
        if (n >= kMaxSlots) {
            return nullptr;
        }
        const Note* const page = notePages_[n >> kPageBits].load(std::memory_order_acquire);
        return page != nullptr ? &page[n & (kPageSize - 1)] : nullptr;
    }

    std::atomic<Slot*> slotPages_[kMaxPages] = {};
    std::atomic<Note*> notePages_[kMaxPages] = {};
    std::atomic<std::size_t> count_{0};
    std::atomic<std::size_t> noteCount_{0};
    std::atomic<std::uint32_t> baseHeads_[kBuckets] = {};
    std::atomic<std::uint32_t> ptrHeads_[kBuckets] = {};
};

}
