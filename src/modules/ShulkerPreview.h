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

    void onContentsText(void* out, const void* tag, const void* returnAddress);
    void onHoverRender(void* self, void* ctx, void* client, void* owner);

    bool tooltipAvailable() const;
    bool miniAvailable() const;

protected:
    bool persistEnabled() const override { return true; }
    void onEnabledChanged(bool enabled) override;

private:
    ShulkerPreview() = default;

    bool wantPreview() const;

    static int miniIcon(void* ctrl, const std::string& coll, int index, std::uintptr_t arg);
    int miniIconOf(const void* stack);
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

    struct alignas(16) Elem {
        std::byte bytes[0xC0];
    };

    struct ContentSet {
        std::uint32_t id = 0;
        std::string key;
        std::array<Elem, kSlots> elems{};
        std::array<bool, kSlots> live{};
        std::array<int, kSlots> counts{};
        unsigned long long usedAt = 0;
    };
    static constexpr std::size_t kContentSets = 128;

    ContentSet* acquireSetLocked(const std::string& key, const std::vector<containerui::NbtItem>& items);
    ContentSet* findSetLocked(std::uint32_t id);
    void freeSetLocked(ContentSet& set);
    void freeStacksLocked();
    void freeStacks();

    std::mutex m_stacksLock;
    std::array<ContentSet, kContentSets> m_sets{};
    std::uint32_t m_nextSetId = 1;
    unsigned long long m_setUseSeq = 0;
    std::uint32_t m_missingId = 0;
    unsigned long long m_missingAt = 0;
    std::atomic<bool> m_addressKeyed{false};
    void forgetSetKeys();
    std::atomic<int> m_reserveRows{6};
    std::atomic<int> m_reserveSpaces{42};
    std::atomic<bool> m_screenOpen{false};
    bool m_loggedTooltip = false;
    bool m_loggedTint = false;
    bool m_lastWant = false;

    Hotkey m_previewKey;
};

}
