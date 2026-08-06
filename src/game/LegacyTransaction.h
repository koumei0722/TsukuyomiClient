#pragma once

#include <cstddef>
#include <cstdint>

namespace tsukuyomi {

class LegacyTransaction {
public:
    static LegacyTransaction& instance();

    void onScansReady();

    bool available() const;

    enum ContainerId : std::uint8_t {
        kContainerInventory = 0,
        kContainerOffhand = 119,
        kContainerArmor = 120,

        kContainerPlayerUi = 124,
    };

    struct SlotRef {
        std::uint8_t containerId = kContainerInventory;
        std::int32_t slot = 0;
        const void* stack = nullptr;
    };

    static SlotRef inventorySlot(int slot, const void* stack)
    {
        return SlotRef{kContainerInventory, static_cast<std::int32_t>(slot), stack};
    }

    static SlotRef offhand(const void* stack)
    {
        return SlotRef{kContainerOffhand, kOffhandSlot, stack};
    }

    bool swap(void* player, const SlotRef& a, const SlotRef& b);

    struct Change {
        std::uint8_t containerId = kContainerInventory;
        std::int32_t slot = 0;
        const void* from = nullptr;
        const void* to = nullptr;
    };

    bool apply(void* player, const Change& a, const Change& b);

private:
    LegacyTransaction() = default;

    bool playerLooksUsable(const void* player) const;

    struct InventorySource {
        std::int32_t type = 0;
        std::uint8_t containerId = 0;
        std::uint8_t pad[3] = {};
        std::int32_t flags = 0;
    };
    static_assert(sizeof(InventorySource) == 12, "InventorySource は 12 バイト");

    using MakeTransactionFn = void**(__fastcall*)(void** out, std::uint32_t type);
    using MakeActionFn = void(__fastcall*)(void* self, const InventorySource* source,
                                           std::int32_t slot, const void* fromItem,
                                           const void* toItem);
    using DestroyActionFn = void(__fastcall*)(void* self);
    using AddActionFn = void(__fastcall*)(void* transaction, const void* action);
    using SendFn = void(__fastcall*)(void* player, void** transaction);

    static constexpr std::ptrdiff_t kInnerTransactionOffset = 0x10;

    static constexpr std::size_t kActionSize = 0x200;

    static constexpr std::size_t kItemStackSize = 0x98;

    static constexpr std::uint32_t kNormalTransaction = 0;

    static constexpr std::int32_t kSourceContainer = 0;

    static constexpr std::int32_t kOffhandSlot = 0;

    static constexpr std::ptrdiff_t kSendVtableOffset = 0x720;
    static constexpr std::ptrdiff_t kNetManagerOffset = 0x1D8;
    static constexpr std::ptrdiff_t kBeginRequestVtableOffset = 0x9D8;

    MakeTransactionFn m_makeTransaction = nullptr;
    MakeActionFn m_makeAction = nullptr;
    DestroyActionFn m_destroyAction = nullptr;
    AddActionFn m_addAction = nullptr;
    SendFn m_send = nullptr;

    bool m_scansReady = false;
    bool m_warnedMissing = false;
    bool m_warnedPlayer = false;
};

}
