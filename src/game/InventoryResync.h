#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include "game/StackIdentity.h"

namespace tsukuyomi {

class InventoryResync {
public:
    static InventoryResync& instance();

    void markStale();
    bool stale() const { return m_stale.load(std::memory_order_acquire); }

    void onInventoryOpening();
    void onContainerOpened();
    void onInventoryContent(int containerId, const void* packet);
    void onPlayerViewUpdate();

private:
    InventoryResync() = default;

    static constexpr int kSlotCount = 36;

    struct Entry {
        inventoryresync::StackIdentity identity;
        bool present = false;
        std::uint16_t count = 0;
        std::int32_t net = 0;
    };
    using Window = std::array<Entry, kSlotCount>;

    void send();
    void checkArrived();
    static bool readClient(const std::byte* stack, Entry& out);
    static bool lineUp(std::byte* stack, const Entry& arrived, const inventoryresync::StackIdentity& sent,
                       std::int32_t& before);

    static constexpr unsigned long long kAnswerWaitMs = 3000;
    static constexpr unsigned long long kSettleMs = 100;
    static constexpr unsigned long long kOffhandGraceMs = 500;
    static constexpr int kMaxLogs = 40;

    static constexpr int kInventoryWindow = 0;
    static constexpr int kOffhandWindow = 119;
    static constexpr std::ptrdiff_t kPacketSlotsOffset = 0xA0;
    static constexpr std::size_t kEntrySize = 0x60;
    static constexpr std::ptrdiff_t kEntryItemOffset = 0x08;
    static constexpr std::ptrdiff_t kEntryCountOffset = 0x10;
    static constexpr std::ptrdiff_t kEntryNetIdOffset = 0x20;
    static constexpr std::size_t kStackSize = 0x98;
    static constexpr std::ptrdiff_t kStackItemOffset = 0x08;
    static constexpr std::ptrdiff_t kStackCountOffset = 0x22;
    static constexpr std::ptrdiff_t kStackNetIdOffset = 0x80;
    static constexpr std::ptrdiff_t kStackNetTagOffset = 0x90;

    std::atomic<bool> m_stale{false};
    std::atomic<unsigned> m_staleSerial{0};
    std::atomic<bool> m_wanted{false};
    std::atomic<int> m_wantedBy{0};

    std::atomic<unsigned long long> m_sentAtMs{0};
    unsigned m_sentSerial = 0;
    std::array<std::int32_t, kSlotCount> m_beforeNet{};
    std::array<inventoryresync::StackIdentity, kSlotCount> m_beforeIdentity{};
    inventoryresync::StackIdentity m_beforeOffhand{};

    std::mutex m_arrivedLock;
    bool m_arrived = false;
    unsigned long long m_arrivedAtMs = 0;
    Window m_arrivedSlots{};
    bool m_arrivedOffhandSeen = false;
    Entry m_arrivedOffhand{};
    unsigned long long m_arrivedOffhandAtMs = 0;

    std::atomic<int> m_logs{0};
};

}
