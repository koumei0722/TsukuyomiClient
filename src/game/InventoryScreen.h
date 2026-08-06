#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace tsukuyomi {

class InventoryScreen {
public:
    static InventoryScreen& instance();

    enum class Area {
        None,
        Hotbar,
        Inventory,
        Offhand,
        Armor,
        Other,
    };

    struct Hovered {
        Area area = Area::None;
        int index = -1;
        int containerSlot = -1;
    };

    void onController(const void* controller);

    Hovered hovered() const;

    bool screenLooksOpen() const;

    bool ownScreenOpen() const;

    const void* controller() const;

    static Hovered readFrom(const void* controller);

private:
    InventoryScreen() = default;

    static bool readString(const void* text, char* out, std::size_t outSize);

    static constexpr std::ptrdiff_t kHoverNameOffset = 0xF98;
    static constexpr std::ptrdiff_t kHoverIndexOffset = 0xFB8;

    static constexpr std::size_t kStringSize = 0x20;
    static constexpr std::ptrdiff_t kStringLengthOffset = 0x10;
    static constexpr std::ptrdiff_t kStringCapacityOffset = 0x18;
    static constexpr std::size_t kShortStringCapacity = 15;

    static constexpr std::size_t kNameMax = 32;

    static constexpr int kHotbarSlots = 9;
    static constexpr int kSlotCount = 36;
    static constexpr int kArmorSlots = 4;

    static constexpr unsigned long long kFreshMs = 500;

    static std::uint64_t pack(const Hovered& value);
    static Hovered unpack(std::uint64_t value);

    static constexpr std::size_t kOwnControllerVtableRva = 0xDDD94F0;

    std::atomic<std::uint64_t> m_hovered{0};
    std::atomic<unsigned long long> m_seenAtMs{0};

    std::atomic<void*> m_controllerVtable{nullptr};

    std::atomic<const void*> m_controller{nullptr};
};

}
