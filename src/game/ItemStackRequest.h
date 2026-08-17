#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "memory/Signatures.h"

namespace tsukuyomi {

class ItemStackRequest {
public:
    static ItemStackRequest& instance();

    void onScansReady();

    bool available() const;

    struct SlotRef {
        std::uint64_t container = 0;
        int slot = 0;
        const void* stack = nullptr;
    };

    static SlotRef inventorySlot(const void* slots, int index);

    static SlotRef offhandSlot(const void* stack);

    bool requestSwap(const SlotRef& a, const SlotRef& b);

    bool requestMove(const void* slots, int fromSlot, int toSlot);

    bool canSwap() const { return m_makeSwapAction != nullptr; }

    std::int32_t lastRequestId() const
    {
        return m_lastRequestId.load(std::memory_order_acquire);
    }

    void onResponse(const void* entries);

    bool takeResponse(std::int32_t id, int& result);

    static constexpr int kResultSuccess = 0;

    bool hasNetManager() const { return m_client.load(std::memory_order_acquire) != nullptr; }

    bool isBuildingRequest() const { return m_building.load(std::memory_order_acquire) != 0; }

    void forget();

    void observePacket(void* packet);

    static int packetId(void* packet);

    void onNotifyInventoryOpen(void* client);

    static std::byte* findContainerOpenHandle();

    static std::byte* resolvePacketHandle(Target getIdTarget, const wchar_t* what);

    static std::byte* resolvePacketReader(Target getIdTarget, const wchar_t* what);

    static std::byte* findInventoryContentReader();

    static std::byte* findContainerOpenReader();

    bool onContainerOpenHandle(void* packet, void* result);

    void rememberContainerOpenResult(const void* result);

    void onFrame();

    bool hasClientInstance() const
    {
        return m_clientInstance.load(std::memory_order_acquire) != nullptr;
    }

    bool suppressingInputReset() const;

    bool serverInventoryOpen() const
    {
        return m_serverInventoryOpen.load(std::memory_order_acquire);
    }

private:
    ItemStackRequest() = default;

    bool notifyServerInventoryOpen();

    bool closeServerInventory();

    bool sendSwap(const SlotRef& a, const SlotRef& b);

    static std::byte* findPacketHandle(std::byte* getId, const wchar_t* what,
                                       std::ptrdiff_t vtableOffset);

    void* findNetManager();

    void* netManager();

    struct NetId {
        std::int32_t value = 0;
        std::int32_t alt = 0;
        std::uint8_t tag = 0;
    };

#pragma pack(push, 1)
    struct SlotInfo {
        std::uint64_t container = 0;
        std::uint32_t unused08 = 0;
        std::uint8_t slot = 0;
        std::uint8_t unused0D = 0;
        std::uint16_t unused0E = 0;
        std::int32_t netValue = 0;
        std::uint32_t unused14 = 0;
        std::int32_t netAlt = 0;
        std::uint32_t unused1C = 0;
        std::uint8_t netTag = 0;
        std::uint8_t unused21[7] = {};
    };
#pragma pack(pop)
    static_assert(sizeof(SlotInfo) == 0x28, "SlotInfo のレイアウトが実測と合っていません");

    static SlotInfo makeSlotInfo(const SlotRef& ref, const NetId& net);
    static SlotInfo makeCursorInfo(const NetId& net);

    static bool readStackAt(const void* stack, NetId& net, std::uint8_t& count);

    using BeginRequestFn = void(__fastcall*)(void* client, void* zero);

    using MakeTransferActionFn = void(__fastcall*)(void** outAction, const std::uint8_t* amount,
                                                  const void* src, const void* dst);

    using MakeSwapActionFn = void(__fastcall*)(void** outAction, const void* src, const void* dst);
    using AddRequestActionFn = void(__fastcall*)(void** clientHolder, void** action);

    using EndRequestFn = void(__fastcall*)(void* guard);

    static constexpr std::uint64_t kContainerHotbar = 28;
    static constexpr std::uint64_t kContainerInventory = 29;

    static constexpr std::uint64_t kContainerOffhand = 34;

    static constexpr int kOffhandSlotIndex = 1;
    static constexpr std::uint64_t kContainerCursor = 59;
    static constexpr int kHotbarSlots = 9;
    static constexpr int kSlotCount = 36;

    static constexpr std::ptrdiff_t kSlotStride = 0x98;
    static constexpr std::ptrdiff_t kStackCountOffset = 0x22;

    static constexpr std::ptrdiff_t kStackNetValueOffset = 0x80;
    static constexpr std::ptrdiff_t kStackNetAltOffset = 0x88;
    static constexpr std::ptrdiff_t kStackNetTagOffset = 0x90;

    static constexpr std::size_t kNetManagerVtableDisp = 16;

    static constexpr std::ptrdiff_t kValidFlagOffset = 0x09;
    static constexpr std::ptrdiff_t kPendingOffset = 0x60;

    static constexpr std::ptrdiff_t kRequestIdOffset = 0x08;

    static constexpr std::size_t kResponseEntrySize = 0x30;
    static constexpr std::ptrdiff_t kResponseResultOffset = 0x00;
    static constexpr std::ptrdiff_t kResponseRequestIdOffset = 0x10;

    static constexpr int kMaxResponseEntries = 64;

    BeginRequestFn m_beginRequest = nullptr;
    MakeTransferActionFn m_makeTakeAction = nullptr;
    MakeTransferActionFn m_makePlaceAction = nullptr;

    MakeSwapActionFn m_makeSwapAction = nullptr;
    AddRequestActionFn m_addRequestAction = nullptr;
    EndRequestFn m_endRequest = nullptr;

    std::atomic<bool> m_scansReady{false};

    std::atomic<std::int32_t> m_lastRequestId{0};
    std::atomic<std::int32_t> m_responseId{0};
    std::atomic<int> m_responseResult{-1};

    std::atomic<void*> m_client{nullptr};

    std::atomic<int> m_building{0};

    std::atomic<bool> m_warnedMissing{false};

    std::atomic<bool> m_warnedOccupied{false};

    std::atomic<bool> m_warnedNoSwap{false};

    std::atomic<bool> m_serverInventoryOpen{false};
    std::atomic<bool> m_warnedNotOpen{false};

    std::atomic<bool> m_openedByUs{false};

    std::atomic<void*> m_clientInstance{nullptr};

    std::atomic<void*> m_clientVtable{nullptr};
    std::atomic<bool> m_warnedNoClient{false};
    std::atomic<bool> m_warnedNoClose{false};

    std::atomic<unsigned long> m_inputResetThread{0};

    std::byte m_closeCopy[0x38]{};
    std::atomic<bool> m_hasCloseCopy{false};

    std::atomic<int> m_suppressOpens{0};
    std::atomic<unsigned long long> m_suppressUntilMs{0};

    static constexpr std::size_t kOpenResultSize = 0x48;
    static constexpr std::ptrdiff_t kOpenResultTagOffset = 0x40;

    std::atomic<bool> m_loggedOpenResult{false};

    std::atomic<unsigned long long> m_pendingCloseAtMs{0};

    static constexpr unsigned long long kCloseDelayMs = 200;

    static constexpr unsigned long long kSuppressWindowMs = 300;

    std::atomic<int> m_openPacketLogs{0};
    static constexpr int kOpenPacketLogLimit = 6;

    static constexpr std::ptrdiff_t kHandleVtableOffset = 0x48;

    static constexpr std::ptrdiff_t kReadVtableOffset = 0x80;

    static constexpr int kGetIdVtableIndex = 1;

    static constexpr int kInteractPacketId = 33;
    static constexpr std::size_t kInteractPacketSize = 0x40;
    static constexpr std::size_t kInteractActionOffset = 0x30;
    static constexpr std::uint8_t kInteractOpenInventory = 6;

    static constexpr int kContainerClosePacketId = 47;

    static constexpr std::size_t kContainerCloseSize = 0x38;
    static constexpr std::size_t kContainerCloseTypeOffset = 0x31;

    static constexpr std::uint8_t kContainerTypeNone = 0xF7;
};

}
