#pragma once

#include <atomic>
#include <mutex>
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

    void onScansReady() override;

    void onPlayerViewUpdate();

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

    bool apply();

    static int swapWithOffhand(const Hands* hands, int count, int slot);

    static int targetSlot(const Hands& hands, int slot)
    {
        return (slot == kHeldSlot) ? hands.selected : slot;
    }

    static constexpr std::ptrdiff_t kPacketContainerIdOffset = 0x30;
    static constexpr std::size_t kPacketMinSize = 0xB8;

    static constexpr int kSlotCount = 36;
    static constexpr int kHotbarSlots = 9;
    static constexpr std::ptrdiff_t kSlotStride = 0x98;

    static constexpr int kNoRequest = -2;
    static constexpr int kHeldSlot = -1;

    static constexpr std::ptrdiff_t kSelectedSlotOffset = 0x10;
    static constexpr std::ptrdiff_t kContainerOffset = 0xB8;
    static constexpr std::ptrdiff_t kSlotsOffset = 0x198;
    static constexpr std::ptrdiff_t kPlayerOffset = 0x1B0;

    static constexpr std::ptrdiff_t kOffhandOffset = 0xDB8;
    static constexpr std::ptrdiff_t kMainhandOffset = 0xE50;

    static constexpr std::ptrdiff_t kStackItemOffset = 0x08;
    static constexpr std::ptrdiff_t kStackCountOffset = 0x22;
    static constexpr std::ptrdiff_t kStackNetValueOffset = 0x80;

    Hotkey m_swapKey;

    std::atomic<int> m_requestedSlot{kNoRequest};

    std::atomic<unsigned long long> m_requestedAtMs{0};

    static constexpr unsigned long long kStaleRequestMs = 2000;

    std::atomic<unsigned long long> m_nextWarnMs{0};
    static constexpr unsigned long long kWarnIntervalMs = 10000;
};

}
