#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "game/ItemStackOps.h"
#include "modules/Module.h"

namespace tsukuyomi {

class HandRestock : public Module {
public:
    static HandRestock& instance();

    const wchar_t* name() const override { return L"HandRestock"; }
    bool available() const override;

    void onScansReady() override;
    void saveConfig(nlohmann::json& section) const override;

    void onSetSelectedSlot(void* holder);

    void onPlayerViewUpdate();

    void noteDeliberateMove();

protected:
    void onEnabledChanged(bool enabled) override;

    bool persistEnabled() const override { return false; }

private:
    HandRestock() = default;

    using Clock = std::chrono::steady_clock;

    using SwapSlotsFn = void(__fastcall*)(void* container, int slotA, int slotB);

    enum Spot {
        kSpotHand = 0,
        kSpotOffhand = 1,
        kSpotCount = 2,
    };

    struct SlotView {
        void* vtable = nullptr;
        void* item = nullptr;
        void* block = nullptr;
        std::uint16_t aux = 0;
        std::uint8_t count = 0;
        std::int32_t netValue = 0;
    };

    struct Inventory {
        void* holder = nullptr;
        void* container = nullptr;
        std::byte* slots = nullptr;
        int hand = -1;

        void* player = nullptr;
        std::byte* offhand = nullptr;

        void* playerRaw = nullptr;
    };

    bool resolve(void* holder, Inventory& out) const;

    bool resolveClient(Inventory& out) const;

    bool isClientSidePlayer(void* player) const;

    bool looksLikeInventory(std::byte* slots) const;

    bool readSlot(std::byte* slots, int index, SlotView& out) const;

    bool readStackAt(const std::byte* stack, SlotView& out) const;

    int countItem(const Inventory& inventory, void* item, std::uint16_t aux) const;

    int countUiItems(const Inventory& inventory, void* item, std::uint16_t aux) const;

    int findSource(const Inventory& inventory, const SlotView& wanted, int keepSlot) const;

    int applyRefill(Spot spot, int destSlot, int sourceSlot);

    bool predictRefill(const Inventory& inventory, Spot spot, int destSlot, int sourceSlot);

    void serveOutstanding();

    bool rollbackOutstanding();

    bool outstandingStillValid() const;

    void clearOutstanding();

    static constexpr int kSlotCount = 36;
    static constexpr int kHotbarSlots = 9;
    static constexpr ptrdiff_t kSlotStride = 0x98;

    static constexpr ptrdiff_t kSelectedSlotOffset = 0x10;
    static constexpr ptrdiff_t kContainerOffset = 0xB8;
    static constexpr ptrdiff_t kSlotsOffset = 0x198;

    static constexpr ptrdiff_t kPlayerOffset = 0x1B0;

    static constexpr ptrdiff_t kOffhandOffset = 0xD90;
    static constexpr ptrdiff_t kMainhandOffset = 0xE28;

    static constexpr std::size_t kLocalPlayerVtableRva = 0xDDA8420;

    static constexpr ptrdiff_t kUiSlotsFirstOffset = 0xA58;
    static constexpr ptrdiff_t kUiSlotsLastOffset = 0xA60;

    static constexpr int kUiSlotLimit = 64;

    static constexpr ptrdiff_t kItemOffset = 0x08;
    static constexpr ptrdiff_t kBlockOffset = 0x18;
    static constexpr ptrdiff_t kAuxOffset = 0x20;
    static constexpr ptrdiff_t kCountOffset = 0x22;
    static constexpr ptrdiff_t kNetValueOffset = 0x80;

    static constexpr int kSettleMs = 200;

    static constexpr int kCooldownMs = 80;

    static constexpr int kRetryMs = 50;

    static constexpr int kGiveUpMs = 3000;

    SwapSlotsFn m_swapSlots = nullptr;

    std::atomic<void*> m_holder{nullptr};
    std::atomic<void*> m_holderAlt{nullptr};

    struct HandState {
        void* container = nullptr;
        int slot = -1;
        void* item = nullptr;
        void* block = nullptr;
        std::uint16_t aux = 0;
        std::uint8_t count = 0;

        int total = 0;
    };
    HandState m_last[kSpotCount];
    Clock::time_point m_nextRefillAt[kSpotCount]{};

    Clock::time_point m_ignoreUntil[kSpotCount]{};

    static constexpr int kIgnoreMoveMs = 500;

    struct Pending {
        bool active = false;
        int destSlot = -1;
        void* item = nullptr;
        void* block = nullptr;
        std::uint16_t aux = 0;

        int totalBefore = 0;

        Clock::time_point at{};
        Clock::time_point giveUpAt{};
    };
    Pending m_pending[kSpotCount];

    struct Outstanding {
        bool active = false;
        bool hasBefore = false;
        std::int32_t requestId = 0;
        Spot spot = kSpotHand;
        void* container = nullptr;
        std::byte* source = nullptr;
        int sourceSlot = -1;
        int destSlot = -1;
        Clock::time_point giveUpAt{};
    };
    Outstanding m_outstanding;

    alignas(void*) std::byte m_before[ItemStackOps::kStackBytes]{};

    static constexpr int kResponseWaitMs = 1500;

    bool m_warnedNoRequest = false;
    bool m_warnedNoPredict = false;

    bool m_clientSideKnown = false;
    bool m_loggedClientSide = false;

    void watch(Spot spot, const Inventory& inventory, const SlotView& view);

    void servePending(Spot spot);

    void dropPending(Spot spot, const wchar_t* why);

    bool emptyEverywhere(Spot spot, int destSlot) const;
};

}
