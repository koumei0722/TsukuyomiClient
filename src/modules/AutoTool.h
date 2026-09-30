#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "modules/Module.h"

namespace tsukuyomi {

class AutoTool : public Module {
public:
    static AutoTool& instance();

    const wchar_t* name() const override { return L"AutoTool"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    void onScansReady() override;
    void shutdown() override;

    bool attackSwitch() const { return m_attackSwitch.load(std::memory_order_relaxed); }

    float onGetDestroySpeed(void* rcx, void* rdx, void* r8, void* r9);
    void onSetSelectedSlot(void* rcx, void* rdx, void* r8, void* r9);

    bool onAttack(void* gameMode, void* target, bool direct, const void* hitPos);
    bool onSendTransaction(void* player, void** transaction);

    void onPlayerViewUpdate();

protected:
    void onEnabledChanged(bool enabled) override;
    void onUpdate() override;

private:
    AutoTool() = default;

    using Clock = std::chrono::steady_clock;

    struct Owner {
        void* holder = nullptr;
        std::byte* slots = nullptr;
        int slot = -1;
    };

    void restoreSlot();

    bool applySlot(void* owner, void* twin, int slot);

    void trackHolder(void* holder);
    void forgetHolder(void* holder);
    std::size_t snapshotHolders(void** out, std::size_t capacity);

    static bool resolveSlots(void* holder, std::byte*& slots, bool* faulted = nullptr);
    static void* playerOfHolder(void* holder);
    bool canTellLive() const;
    bool holderIsLive(void* holder, int* side = nullptr) const;
    bool speedQueryIsForeign(void* context) const;
    std::byte* contextPlayer(void* context, int* side = nullptr) const;
    static bool readServerNetIds(const std::byte* slots, std::int32_t* out);

    bool findOwner(void* item, Owner& out, const void* queryPlayer);

    void* findTwin(const Owner& owner);

    static bool sameHotbar(const std::byte* a, const std::byte* b);

    static bool looksLikeSlotArray(const std::byte* slotZero, ptrdiff_t stride, int slotCount,
                                   bool* faulted = nullptr);

    static constexpr int kSlotCount = 9;
    static constexpr ptrdiff_t kSelectedSlotOffset = 0x10;

    static constexpr ptrdiff_t kContainerOffset = 0xB8;
    static constexpr ptrdiff_t kSlotsOffset = 0x198;
    static constexpr ptrdiff_t kContainerPlayerOffset = 0x1B0;

    static constexpr ptrdiff_t kItemOffset = 0x08;
    static constexpr ptrdiff_t kCountOffset = 0x22;
    static constexpr ptrdiff_t kNetValueOffset = 0x80;
    static constexpr ptrdiff_t kNetTagOffset = 0x90;
    static constexpr int kInventorySlots = 36;
    static constexpr ptrdiff_t kContextPlayerOffset = 0x08;

    static constexpr ptrdiff_t kItemPointerOffset = 0x10;
    static constexpr ptrdiff_t kSlotStride = 0x98;

    static constexpr int kIdleRestoreMs = 250;

    static constexpr std::size_t kHolderSlots = 16;
    void noteOwnerOnce(void* holder, std::size_t seen);
    void* m_ownerLogged[4] = {};

    struct Tracked {
        void* holder = nullptr;
        unsigned long long seenAt = 0;
    };
    std::mutex m_holdersMutex;
    Tracked m_holders[kHolderSlots];
    unsigned long long m_holderSeq = 0;
    std::atomic<void*> m_owner{nullptr};
    std::atomic<void*> m_ownClient{nullptr};
    std::atomic<int> m_ownerLogs{0};
    std::atomic<bool> m_fallbackLogged{false};

    std::atomic<Clock::rep> m_lastSpeedQuery{0};

    std::atomic<bool> m_restoreWanted{false};

    struct Switch {
        void* owner = nullptr;
        void* twin = nullptr;
        int originalSlot = -1;
    };
    static constexpr std::size_t kSwitchSlots = 4;

    std::mutex m_stateMutex;
    Switch m_switches[kSwitchSlots];

    bool hasSwitchFor(void* owner);
    bool anySwitched();

    struct AttackApi {
        void* damageCalc = nullptr;
        std::ptrdiff_t inventory = -1;
        std::ptrdiff_t announced = -1;
        std::ptrdiff_t category = -1;
        const void* localPlayerVtable = nullptr;
        const void* armorStandVtable = nullptr;
        bool ready = false;
    };
    AttackApi m_attack;
    void resolveAttack();
    bool callAttackHoldingIfUnannounced(void* gameMode, void* target, bool direct, const void* hitPos, void* player,
                                        std::byte* inv);

    static thread_local bool t_holdAttackTx;
    static thread_local void* t_holdPlayer;
    struct Held {
        void* tx = nullptr;
        void* player = nullptr;
        int slot = -1;
        unsigned long long atMs = 0;
    };
    static constexpr std::size_t kMaxHeld = 8;
    Held m_held[kMaxHeld];
    std::size_t m_heldCount = 0;
    int m_attackButton = -1;
    void flushHeld(bool dropAll);
    std::atomic<unsigned long long> m_lastAttackMs{0};
    std::atomic<void*> m_attackOwner{nullptr};
    std::atomic<int> m_attackSlot{-1};
    static constexpr unsigned long long kAttackRestoreMs = kIdleRestoreMs;
    bool attackedRecently() const;
    static constexpr unsigned long long kHeldTimeoutMs = 500;
    std::atomic<int> m_attackLogs{0};
    static constexpr bool kDefaultAttackSwitch = true;
    std::atomic<bool> m_attackSwitch{kDefaultAttackSwitch};
};

}
