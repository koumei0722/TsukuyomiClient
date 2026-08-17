#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "input/Hotkey.h"
#include "modules/Module.h"

namespace tsukuyomi {

class OffhandSwap : public Module {
public:
    static OffhandSwap& instance();

    const wchar_t* name() const override { return L"OffhandSwap"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    bool screenSwapEnabled() const { return m_screenSwap; }

    bool legacyBridgeEnabled() const { return m_legacyBridge; }
    void onScansReady() override;

    void onSetSelectedSlot(void* holder);

    void onPlayerViewUpdate();

    bool onInventoryHotbarKey(const void* controller, int hotbarIndex);

    void onInventoryContent(const void* payload);

protected:
    void onUpdate() override;

private:
    OffhandSwap() = default;

    struct StackView {
        void* vtable = nullptr;
        void* item = nullptr;
        std::int32_t netValue = 0;
        std::uint8_t count = 0;
    };

    struct Hands {
        void* holder = nullptr;
        void* container = nullptr;
        void* player = nullptr;
        std::byte* slots = nullptr;
        std::byte* offhand = nullptr;
        int selected = -1;
    };

    bool resolve(void* holder, Hands& out) const;

    bool looksLikeInventory(std::byte* slots) const;

    bool readStack(const std::byte* stack, StackView& out) const;

    bool apply(int slot);

    static int swapWithOffhand(const Hands* hands, int count, int slot);

    static int targetSlot(const Hands& hands, int slot)
    {
        return (slot == kHeldSlot) ? hands.selected : slot;
    }

    static int slotUnderCursor();

    void servePending();

    struct NetIdFix {
        bool active = false;

        std::byte* hand = nullptr;

        std::byte* mainhand = nullptr;

        std::byte* offhand = nullptr;
        int slot = -1;

        std::int32_t handValue = 0;
        std::int32_t offhandValue = 0;

        unsigned int serial = 0;
        unsigned long long giveUpAtMs = 0;
    };
    NetIdFix m_netIdFix;

    std::atomic<std::int32_t> m_offhandNetId{0};
    std::atomic<unsigned int> m_offhandSerial{0};

    void serveNetIdFix();

    void beginNetIdFix(const Hands& hands, int slot);

    static constexpr unsigned long long kNetIdWaitMs = 2000;

    static constexpr std::ptrdiff_t kPacketContainerIdOffset = 0x30;
    static constexpr std::ptrdiff_t kPacketSlotsOffset = 0xA0;
    static constexpr std::size_t kPacketMinSize = 0xB8;
    static constexpr std::size_t kEntrySize = 0x60;
    static constexpr std::ptrdiff_t kEntryItemOffset = 0x08;
    static constexpr std::ptrdiff_t kEntryNetIdOffset = 0x20;
    static constexpr int kOffhandContainerId = 119;

    struct Pending {
        bool active = false;
        std::int32_t requestId = 0;

        std::byte* hand[2] = {};
        std::byte* offhand[2] = {};
        int count = 0;
        int selected = -1;
        unsigned long long giveUpAtMs = 0;
    };
    Pending m_pending;

    static constexpr unsigned long long kResponseWaitMs = 1500;

    static constexpr int kSlotCount = 36;
    static constexpr int kHotbarSlots = 9;
    static constexpr std::ptrdiff_t kSlotStride = 0x98;

    static constexpr int kNoRequest = -2;
    static constexpr int kHeldSlot = -1;

    static constexpr std::ptrdiff_t kSelectedSlotOffset = 0x10;
    static constexpr std::ptrdiff_t kContainerOffset = 0xB8;
    static constexpr std::ptrdiff_t kSlotsOffset = 0x198;
    static constexpr std::ptrdiff_t kPlayerOffset = 0x1B0;

    static constexpr std::ptrdiff_t kOffhandOffset = 0xD90;
    static constexpr std::ptrdiff_t kMainhandOffset = 0xE28;

    static constexpr std::ptrdiff_t kStackItemOffset = 0x08;
    static constexpr std::ptrdiff_t kStackCountOffset = 0x22;
    static constexpr std::ptrdiff_t kStackNetValueOffset = 0x80;

    Hotkey m_swapKey;

    std::atomic<void*> m_holder{nullptr};
    std::atomic<void*> m_holderAlt{nullptr};

    std::atomic<int> m_requestedSlot{kNoRequest};

    std::atomic<unsigned long long> m_requestedAtMs{0};

    static constexpr unsigned long long kQueueWaitMs = 500;

    bool m_screenSwap = false;
    bool m_legacyBridge = false;

    std::atomic<unsigned long long> m_nextWarnMs{0};
    static constexpr unsigned long long kWarnIntervalMs = 10000;
};

}
