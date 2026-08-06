#pragma once

#include "game/ItemStackOps.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace tsukuyomi {

class InventoryActionBridge {
public:
    static InventoryActionBridge& instance();

    void onSetSelectedSlot(void* holder);

    void onAddRequestAction(void* const* clientHolder, void* const* action);

    bool shouldBlockPacket(void* packet);

    void onFrame();

    void onContainerOpen(void* packet);

private:
    InventoryActionBridge() = default;

    struct SlotView {
        std::uint64_t container = 0;
        std::uint8_t slot = 0;
        std::int32_t netValue = 0;
        std::int32_t netAlt = 0;
        std::uint8_t netTag = 0xFF;
    };

    struct Places {
        void* container = nullptr;
        std::byte* slots = nullptr;
        void* player = nullptr;
        std::byte* cursor = nullptr;
    };

    struct Spot {
        std::uint8_t legacyContainer = 0;
        std::int32_t legacySlot = 0;
        std::byte* stack = nullptr;

        int notifySlot = -1;

        void* notifyContainer = nullptr;
    };

    enum class Plan {
        None,

        Swap,

        Split,

        Merge,
    };

    struct Entry {
        std::uint8_t containerA = 0;
        std::int32_t slotA = 0;
        std::uint8_t containerB = 0;
        std::int32_t slotB = 0;
        void* player = nullptr;
        alignas(void*) std::byte beforeA[ItemStackOps::kStackBytes]{};
        alignas(void*) std::byte beforeB[ItemStackOps::kStackBytes]{};
        alignas(void*) std::byte afterA[ItemStackOps::kStackBytes]{};
        alignas(void*) std::byte afterB[ItemStackOps::kStackBytes]{};
    };

    static constexpr int kQueueSize = 256;

    static constexpr int kRequestPacketId = 147;

    static constexpr int kPacketProbeLimit = 12;

    static constexpr std::size_t kActionSize = 0x68;
    static constexpr std::ptrdiff_t kKindOffset = 0x08;
    static constexpr std::ptrdiff_t kAmountOffset = 0x12;
    static constexpr std::ptrdiff_t kSrcOffset = 0x18;
    static constexpr std::ptrdiff_t kDstOffset = 0x40;

    static constexpr std::size_t kActionHeadSize = 0x13;

    static constexpr std::uint8_t kKindTake = 0;
    static constexpr std::uint8_t kKindPlace = 1;
    static constexpr std::uint8_t kKindSwap = 2;

    static constexpr std::ptrdiff_t kSlotContainerOffset = 0x00;
    static constexpr std::ptrdiff_t kSlotIndexOffset = 0x0C;
    static constexpr std::ptrdiff_t kSlotNetValueOffset = 0x10;
    static constexpr std::ptrdiff_t kSlotNetAltOffset = 0x18;
    static constexpr std::ptrdiff_t kSlotNetTagOffset = 0x20;

    static constexpr std::uint64_t kContainerWholeInventory = 12;
    static constexpr std::uint64_t kContainerHotbar = 28;
    static constexpr std::uint64_t kContainerInventory = 29;
    static constexpr std::uint64_t kContainerCursor = 59;

    static constexpr std::uint64_t kContainerOpened = 7;

    static constexpr std::ptrdiff_t kPendingOffset = 0x60;
    static constexpr std::ptrdiff_t kRequestIdOffset = 0x08;
    static constexpr std::ptrdiff_t kRequestActionsBeginOffset = 0x30;
    static constexpr std::ptrdiff_t kRequestActionsEndOffset = 0x38;
    static constexpr std::size_t kRequestHeadSize = 0x40;

    static constexpr std::ptrdiff_t kContainerOffset = 0xB8;
    static constexpr std::ptrdiff_t kSlotsOffset = 0x198;
    static constexpr std::ptrdiff_t kPlayerOffset = 0x1B0;

    static constexpr std::ptrdiff_t kUiItemsOffset = 0xA58;

    static constexpr std::size_t kStackSize = ItemStackOps::kStackBytes;
    static constexpr std::ptrdiff_t kStackItemOffset = 0x08;
    static constexpr std::ptrdiff_t kStackAuxOffset = 0x20;
    static constexpr std::ptrdiff_t kStackCountOffset = 0x22;

    static constexpr int kMaxStackCount = 255;

    static constexpr int kSlotCount = 36;

    static constexpr std::ptrdiff_t kNameResolverOffset = 0x11C0;
    static constexpr std::ptrdiff_t kGetItemVtableOffset = 0x40;

    static constexpr std::size_t kQuerySize = 0x28;
    static constexpr std::ptrdiff_t kQuerySizeOffset = 0x10;
    static constexpr std::ptrdiff_t kQueryCapacityOffset = 0x18;
    static constexpr std::ptrdiff_t kQueryIndexOffset = 0x20;
    static constexpr std::size_t kShortStringCapacity = 15;

    static constexpr std::ptrdiff_t kResolverScreenOffset = 0xA8;
    static constexpr std::ptrdiff_t kModelContainerOffset = 0x1A8;

    static constexpr std::ptrdiff_t kCollectionItemsOffset = 0x110;
    static constexpr std::ptrdiff_t kCollectionNameOffset = 0x20;
    static constexpr std::ptrdiff_t kCollectionModelOffset = 0x108;

    static constexpr std::uintptr_t kScanFrom = 0x10000;
    static constexpr std::uintptr_t kScanTo = 0x7FFFFFFFFFFFull;

    static constexpr unsigned long long kScanRetryMs = 1000;

    static constexpr std::ptrdiff_t kScreenScanBytes = 0x4000;
    static constexpr std::ptrdiff_t kScreenPageBytes = 0x1000;
    static constexpr std::ptrdiff_t kInnerScanBytes = 0x400;

    static constexpr int kResolveBudget = 500000;

    static constexpr int kTraceCalls = 0;

    static constexpr std::ptrdiff_t kOpenIdOffset = 0x30;
    static constexpr std::ptrdiff_t kOpenKindOffset = 0x31;
    static constexpr std::size_t kOpenPeekBytes = 0x30;
    static constexpr int kContainerOpenProbes = 6;

    static constexpr std::ptrdiff_t kContentChangedVtableOffset = 0x110;

    static constexpr std::ptrdiff_t kContainerScanBytes = 0x300;

    static constexpr int kMaxContainerSlots = 128;

    static constexpr unsigned long long kWindowMs = 1000;
    static constexpr int kMaxPerWindow = 40;

    static bool readSlot(const void* address, SlotView& out);

    static bool readRequest(void* const* clientHolder, std::int32_t& requestId,
                            std::uint64_t& index);

    bool resolve(Places& out) const;

    static bool looksLikeInventory(const std::byte* slots);

    static bool looksLikeStacks(const std::byte* slots, int count);

    struct OpenContainer {
        void* container = nullptr;
        std::byte* slots = nullptr;
        int count = 0;
    };

    bool resolveOpenContainer(OpenContainer& out) const;

    static bool containerFrom(void* model, const void* ownNotify, const void* ownContainer,
                              void*& container);

    static std::byte* findSlots(void* container, int count, const std::byte* exclude);

    static bool looksLikeHeapPointer(const void* value);

    static std::byte* findCollection(const void* items, const char* name, std::size_t nameLength);

    static bool translate(const Places& places, const OpenContainer& open, int openId,
                          const SlotView& view, Spot& out);

    static Plan choosePlan(const Places& places, std::uint8_t kind, std::uint8_t amount,
                           const Spot& a, const Spot& b, std::byte*& empty);

    static bool applyPlan(Plan plan, std::uint8_t amount, const Spot& a, const Spot& b,
                          std::byte* empty);

    bool queueMove(const Places& places, Plan plan, std::uint8_t amount, const Spot& a,
                   const Spot& b, std::byte* empty);

    static std::uint8_t countOf(const std::byte* stack);

    static bool isEmptyStack(const std::byte* stack);

    static bool sameItem(const std::byte* a, const std::byte* b);

    static std::byte* findEmptyStack(const Places& places, const std::byte* skipA,
                                     const std::byte* skipB);

    void clearQueue();

    bool allowLog();

    std::atomic<void*> m_holder{nullptr};
    std::atomic<void*> m_holderAlt{nullptr};

    Entry m_queue[kQueueSize];
    int m_queued = 0;

    std::atomic<int> m_blockLeft{0};
    std::atomic<unsigned long long> m_blockUntilMs{0};

    static constexpr unsigned long long kBlockWindowMs = 2000;

    std::atomic<bool> m_notedOwn{false};

    std::atomic<bool> m_warnedPlaces{false};

    std::atomic<void*> m_openContainer{nullptr};

    mutable std::atomic<int> m_traceLeft{kTraceCalls};

    mutable std::atomic<void*> m_openModel{nullptr};

    mutable std::atomic<void*> m_cachedContainer{nullptr};
    mutable std::atomic<void*> m_cachedSlots{nullptr};
    mutable std::atomic<int> m_cachedCount{0};

    mutable std::atomic<unsigned long long> m_nextScanMs{0};

    std::atomic<int> m_openContainerId{-1};
    std::atomic<int> m_openContainerKind{-1};

    std::atomic<int> m_containerOpenProbes{kContainerOpenProbes};

    std::atomic<bool> m_warnedBusy{false};

    std::atomic<int> m_packetProbes{0};

    std::atomic<unsigned long long> m_windowStartMs{0};
    std::atomic<int> m_inWindow{0};
    std::atomic<int> m_dropped{0};
};

}
