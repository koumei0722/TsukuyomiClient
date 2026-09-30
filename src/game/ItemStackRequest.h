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

    void forget();

    void noteInventoryContent(int containerId)
    {
        if (containerId == 0) {
            m_inventoryContentSerial.fetch_add(1, std::memory_order_release);
        }
    }
    std::uint32_t inventoryContentSerial() const
    {
        return m_inventoryContentSerial.load(std::memory_order_acquire);
    }

    void observePacket(void* packet);

    static int packetId(void* packet);

    void onNotifyInventoryOpen(void* client);

    static std::byte* findContainerOpenHandle();

    static std::byte* findInventoryContentReader();

    bool suppressionPending();
    bool captureOpenShell(const void* packet, std::byte* shell);
    bool takeInventoryOpen(void* packet, const void* result, const std::byte* shell);
    static constexpr std::ptrdiff_t kOpenShellOffset = 0x30;
    static constexpr std::size_t kOpenShellBytes = 0x18;
    static constexpr int kContainerTypeInventory = -1;

    void rememberContainerOpenResult(const void* result);

    void onFrame();

    bool suppressingInputReset() const;

    bool serverInventoryOpen() const
    {
        return m_serverInventoryOpen.load(std::memory_order_acquire);
    }

private:
    ItemStackRequest() = default;

    bool synthesizeClose();

    bool notifyServerInventoryOpen();

    bool closeServerInventory();

    bool reopenForPlayer();

    bool sendSwap(const SlotRef& a, const SlotRef& b);

    static std::byte* findPacketHandle(std::byte* getId, const wchar_t* what,
                                       std::ptrdiff_t vtableOffset);
    static std::byte* findPacketVtable(std::byte* getId);

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
    static_assert(sizeof(SlotInfo) == 0x28);

    static SlotInfo makeSlotInfo(const SlotRef& ref, const NetId& net);
    static SlotInfo makeCursorInfo(const NetId& net);

    static bool readStackAt(const void* stack, NetId& net, std::uint8_t& count);

    using BeginRequestFn = void(__fastcall*)(void* client, void* zero);
    using MakeTransferActionFn = void(__fastcall*)(void** outAction, const std::uint8_t* amount,
                                                  const void* src, const void* dst);
    using MakeSwapActionFn = void(__fastcall*)(void** outAction, const void* src, const void* dst);
    using AddRequestActionFn = void(__fastcall*)(void** clientHolder, void** action);
    using EndRequestFn = void(__fastcall*)(void* client);

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

    static constexpr std::size_t kNetManagerVtableDisp = 3;

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
    static constexpr int kTrackedRequests = 8;
    std::atomic<std::int32_t> m_sentIds[kTrackedRequests]{};
    std::atomic<unsigned> m_sentNext{0};
    std::atomic<std::int32_t> m_answerIds[kTrackedRequests]{};
    std::atomic<int> m_answerResults[kTrackedRequests]{};
    std::atomic<bool> m_answerTaken[kTrackedRequests]{};
    std::atomic<unsigned> m_answerNext{0};
    bool isOurRequest(std::int32_t id) const;
    bool wasAnswered(std::int32_t id) const;

    std::atomic<void*> m_client{nullptr};

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

    std::atomic<std::int32_t> m_closeAfterRequestId{0};
    std::atomic<unsigned long long> m_closeDeadlineMs{0};
    std::atomic<std::int32_t> m_lastAnsweredId{0};
    std::atomic<std::uint32_t> m_inventoryContentSerial{0};
    std::atomic<bool> m_reopenForPlayer{false};
    std::atomic<bool> m_playerClosed{false};
    std::atomic<int> m_reopenLogs{0};

    std::atomic<void*> m_closeVtable{nullptr};
    std::byte m_packetHead[0x30]{};
    std::atomic<bool> m_hasPacketHead{false};
    std::atomic<bool> m_closeSynthetic{false};
    std::atomic<bool> m_loggedRealClose{false};

    static constexpr std::size_t kOpenResultSize = 0x48;
    static constexpr std::ptrdiff_t kOpenResultTagOffset = 0x40;

    std::atomic<bool> m_loggedOpenResult{false};

    std::atomic<unsigned long long> m_pendingCloseAtMs{0};

    static constexpr unsigned long long kCloseDelayMs = 200;
    static constexpr unsigned long long kCloseFallbackMs = 3000;

    static constexpr unsigned long long kSuppressWindowMs = 30000;

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
