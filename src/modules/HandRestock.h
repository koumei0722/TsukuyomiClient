#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "game/HolderTable.h"
#include "game/ChatCommand.h"
#include "game/ItemStackOps.h"
#include "modules/Module.h"

namespace tsukuyomi {

class HandRestock : public Module {
public:
    static HandRestock& instance();

    const wchar_t* name() const override { return L"HandRestock"; }
    bool available() const override;

    void onScansReady() override;
    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    void onSetSelectedSlot(void* holder);

    void onPlayerViewUpdate();

    void noteDeliberateMove();

    bool forEachInventorySlot(const std::function<void(const void*, int)>& fn) const;

protected:
    void onEnabledChanged(bool enabled) override;

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
        std::uint8_t netTag = 0xFF;
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

    bool resolve(void* holder, Inventory& out, bool* faulted = nullptr) const;

    struct Own {
        Inventory client;
        bool haveClient = false;
        Inventory server;
        bool haveServer = false;
    };
    bool resolveOwn(Own& out, bool wantServer) const;

    bool resolveClient(Inventory& out) const;

public:
    void ownHolders(void*& client, void*& server) const;

    void* ownClientHolder() const;

private:

    bool isClientSidePlayer(void* player) const;
    void* localPlayerVtable() const;

    bool looksLikeInventory(std::byte* slots, bool* faulted = nullptr) const;

    bool readSlot(std::byte* slots, int index, SlotView& out) const;

    bool readStackAt(const std::byte* stack, SlotView& out) const;

    int countItem(const Inventory& inventory, void* item, std::uint16_t aux) const;

    int countUiItems(const Inventory& inventory, void* item, std::uint16_t aux) const;

    int findSource(const Inventory& inventory, const SlotView& wanted, int keepSlot) const;

    int applyRefill(Spot spot, int destSlot, int sourceSlot, bool durabilitySwap = false);

    void checkDurability(const Inventory& inventory);
    bool durabilityOf(const std::byte* stack, int& max, int& damage) const;
    int findDurabilitySource(const Inventory& inventory, void* item, int keepSlot, int threshold) const;

    bool predictRefill(const Inventory& inventory, Spot spot, int destSlot, int sourceSlot);

    void notifyRefilled(void* container, int destSlot, int sourceSlot);

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

    static constexpr std::size_t kLocalPlayerVtableDisp = 3;

    static constexpr ptrdiff_t kUiSlotsFirstOffset = 0xA58;
    static constexpr ptrdiff_t kUiSlotsLastOffset = 0xA60;

    static constexpr int kUiSlotLimit = 64;

    static constexpr ptrdiff_t kItemOffset = 0x08;
    static constexpr ptrdiff_t kBlockOffset = 0x18;
    static constexpr ptrdiff_t kAuxOffset = 0x20;
    static constexpr ptrdiff_t kCountOffset = 0x22;
    static constexpr ptrdiff_t kNetValueOffset = 0x80;
    static constexpr ptrdiff_t kNetTagOffset = 0x90;

    static constexpr int kSettleMs = 200;

    static constexpr int kCooldownMs = 80;

    static constexpr int kRetryMs = 50;

    static constexpr int kGiveUpMs = 3000;

    SwapSlotsFn m_swapSlots = nullptr;

    mutable std::atomic<void*> m_clientHolder{nullptr};
    mutable HolderTable m_holders;
    mutable void* m_lastGamePlayer = nullptr;
    mutable std::atomic<void*> m_faultedHolder{nullptr};
    std::ptrdiff_t m_inventoryDisp = 0;
    struct FaultedPlayer {
        std::atomic<void*> player{nullptr};
        std::atomic<unsigned long long> serial{0};
        std::atomic<unsigned long long> at{0};
    };
    static constexpr unsigned long long kFaultedPlayerForgetMs = 2000;
    mutable FaultedPlayer m_faultedPlayers[2];
    mutable std::atomic<unsigned int> m_faultedPlayerNext{0};
    bool playerFaulted(void* player) const;
    void notePlayerFaulted(void* player) const;
    bool holderIsLocal(void* holder) const;
    void* holderOfPlayer(void* player) const;

    struct HandState {
        void* container = nullptr;
        int slot = -1;
        void* item = nullptr;
        std::uint16_t aux = 0;
        std::uint8_t count = 0;

        int total = 0;
        std::string name;
    };
    std::atomic<bool> m_resetRequested{false};
    HandState m_last[kSpotCount];
    mutable std::mutex m_excludedMutex;
    std::set<std::string> m_excluded;
    chatcommand::Reply restockCommand(const std::vector<std::string>& args);
    std::string mainHandItemName() const;
    Clock::time_point m_nextRefillAt[kSpotCount]{};

    Clock::time_point m_ignoreUntil[kSpotCount]{};

    static constexpr int kIgnoreMoveMs = 500;

    struct Pending {
        bool active = false;
        int destSlot = -1;
        void* container = nullptr;
        void* item = nullptr;
        std::uint16_t aux = 0;

        int totalBefore = 0;

        Clock::time_point at{};
        Clock::time_point giveUpAt{};

        bool retry = false;
        int attempts = 0;
        bool waitContent = false;
        std::uint32_t contentSerial = 0;
        Clock::time_point contentDeadline{};
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
        Pending retryAs;
        std::uint32_t contentSerialAtSend = 0;
        void* predItem = nullptr;
        std::uint8_t predCount = 0;
        bool durabilitySwap = false;
        void* predSourceItem = nullptr;
        std::uint8_t predSourceCount = 0;
        std::int32_t predDestNet = 0;
        std::int32_t predSourceNet = 0;
    };
    Outstanding m_outstanding;

    alignas(void*) std::byte m_before[ItemStackOps::kStackBytes]{};

    static constexpr int kResponseWaitMs = 30000;

    static constexpr int kRefusedRetryMs = 400;
    static constexpr int kMaxRefusedRetries = 6;
    static constexpr int kContentWaitMs = 20000;
    static constexpr int kContentSettleMs = 1500;
    static constexpr int kResultFailedToValidateSrcSlot = 49;
    static constexpr int kResultFailedToValidateDstSlot = 50;

    static constexpr int kDefaultDurabilityThreshold = 10;
    static constexpr int kMinDurabilityThreshold = 1;
    static constexpr int kMaxDurabilityThreshold = 100;
    std::atomic<bool> m_swapLowDurability{false};
    std::atomic<int> m_durabilityThreshold{kDefaultDurabilityThreshold};
    std::int32_t m_maxDamageSlot = 0;
    void* m_damageValue = nullptr;
    static constexpr int kDurabilityCooldownMs = 500;
    static constexpr int kDurabilityRefusedMs = 2000;
    Clock::time_point m_nextDurabilityAt[kSpotCount]{};
    void* m_noReplacementItem[kSpotCount]{};
    bool m_warnedNoSwapAction = false;
    bool m_warnedNoDurability = false;

    bool m_warnedNoRequest = false;
    bool m_warnedNoPredict = false;

    bool m_clientSideKnown = false;
    bool m_loggedClientSide = false;
    mutable bool m_loggedForeign = false;
    mutable int m_loggedServerCopy = -1;
    mutable int m_serverCopyLogs = 0;

    struct Checked {
        void* holder = nullptr;
        void* container = nullptr;
        std::byte* slots = nullptr;
    };
    mutable Checked m_checked[HolderTable::kCapacity]{};
    mutable std::size_t m_checkedNext = 0;
    bool alreadyChecked(void* holder, void* container, std::byte* slots) const;
    void rememberChecked(void* holder, void* container, std::byte* slots) const;

    std::atomic<void*> m_localPlayerVtable{nullptr};

    void watch(Spot spot, const Inventory& inventory, const SlotView& view);

    void servePending(Spot spot);

    void dropPending(Spot spot, const wchar_t* why);

    bool emptyEverywhere(Spot spot, int destSlot) const;
};

}
