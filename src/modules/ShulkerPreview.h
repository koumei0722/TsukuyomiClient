#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "game/ContainerUi.h"
#include "input/Hotkey.h"
#include "modules/Module.h"

namespace tsukuyomi {

class ShulkerPreview : public Module, public containerui::Listener {
public:
    static ShulkerPreview& instance();

    const wchar_t* name() const override { return L"ShulkerPreview"; }
    bool available() const override;
    bool writeBlocked() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    void onScansReady() override;
    void shutdown() override;

    bool onSlotButton(std::uint32_t id, int state, const std::string& coll, int index) override;
    void onScreenTick() override;
    void onScreenLost() override;
    void onScreenCreated(void* ctrl) override;

    static constexpr int kSlots = 27;

    static const void* stackFor(const void* collectionName, int index);

    bool tooltipAvailable() const;
    bool miniAvailable() const;
    bool ownsWheel() const;

    struct alignas(16) Elem {
        std::byte bytes[0xC0];
    };

protected:
    void onEnabledChanged(bool enabled) override;

private:
    ShulkerPreview() = default;

    bool wantPreview() const;

    static int miniIcon(void* ctrl, const std::string& coll, int index, std::uintptr_t arg);
    static int miniIconHud(void* ctrl, const std::string& coll, int index, std::uintptr_t arg);
    static void onHudCreated(void* ctrl);
    int miniIconOf(const void* stack, bool useCache = true);
    std::atomic<bool> m_forgetNames{false};
    bool m_loggedHudMini = false;
    int idAuxOf(const std::string& name, int aux);
    int maxStackOfName(const std::string& name);
    void registerUiDefinitions();

    std::map<std::string, int> m_idAuxCache;
    std::map<std::string, int> m_maxStackCache;
    struct MiniKey {
        std::uintptr_t root = 0;
        std::uintptr_t first = 0;
        std::uintptr_t last = 0;
        bool operator==(const MiniKey& o) const { return root == o.root && first == o.first && last == o.last; }
    };
    struct MiniKeyHash {
        std::size_t operator()(const MiniKey& k) const
        {
            return std::hash<std::uintptr_t>{}(k.root ^ (k.first * 31) ^ (k.last * 131));
        }
    };
    std::unordered_map<MiniKey, int, MiniKeyHash> m_miniCache;
    bool m_loggedMini = false;

    void resolveTooltip();
    static int currentSlot(void* ctrl, const std::string& coll, int index, std::uintptr_t arg);
    static bool gridVisible(std::uintptr_t arg);
    static bool normalTooltip(std::uintptr_t arg);
    void publishCurrent(const void* stack, std::uint32_t id);
    static bool cellSelected(std::uintptr_t index);
    static bool hasSelected(std::uintptr_t arg);
    void consumeWheel();
    void select(int slot);

    struct ContentSet {
        std::uint32_t id = 0;
        std::string key;
        std::array<Elem, kSlots> elems{};
        std::array<bool, kSlots> live{};
        unsigned long long usedAt = 0;
    };
    static constexpr std::size_t kContentSets = 128;

    bool contentsKey(const std::vector<containerui::NbtItem>& items, std::string& key);
    ContentSet* acquireSetLocked(const std::string& key, const std::vector<containerui::NbtItem>& items);
    ContentSet* findSetLocked(std::uint32_t id);
    std::uint32_t setForStackLocked(const void* stack);
    void freeSetLocked(ContentSet& set);
    void freeStacksLocked();
    void freeStacks();

    std::mutex m_stacksLock;
    std::array<ContentSet, kContentSets> m_sets{};
    std::uint32_t m_nextSetId = 1;
    unsigned long long m_setUseSeq = 0;
    struct SlotKey {
        std::uintptr_t stack = 0;
        std::uintptr_t root = 0;
        std::uintptr_t first = 0;
        std::uintptr_t last = 0;
        bool operator==(const SlotKey& o) const
        {
            return stack == o.stack && root == o.root && first == o.first && last == o.last;
        }
    };
    struct SlotKeyHash {
        std::size_t operator()(const SlotKey& k) const
        {
            return std::hash<std::uintptr_t>{}(k.stack ^ (k.root * 7) ^ (k.first * 31) ^ (k.last * 131));
        }
    };
    std::unordered_map<SlotKey, std::uint32_t, SlotKeyHash> m_slotSets;
    std::uint32_t m_currentSet = 0;
    unsigned long long m_currentTick = 0;
    std::uint32_t m_publishedSet = 0;
    struct HoveredBox {
        void* screen = nullptr;
        std::string collection;
        int index = -1;
        bool operator==(const HoveredBox&) const = default;
    };
    HoveredBox m_currentBox;
    HoveredBox m_publishedBox;
    HoveredBox m_selectedBox;
    int m_selected = -1;
    std::uint32_t m_selectedSet = 0;
    int m_wheelLeftButton = -1;
    int m_wheelRightButton = -1;
    std::uint64_t m_wheelLeftSeen = 0;
    std::uint64_t m_wheelRightSeen = 0;
    unsigned long long m_tick = 0;
    bool m_lastWant = false;

    Hotkey m_previewKey;
};

}
