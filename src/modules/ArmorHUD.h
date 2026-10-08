#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "game/ArmorHudLayout.h"
#include "modules/Module.h"

namespace tsukuyomi {

class ArmorHUD : public Module {
public:
    static ArmorHUD& instance();

    const wchar_t* name() const override { return L"ArmorHUD"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    void onScansReady() override;
    void shutdown() override;

    void onPlayerViewUpdate();

    int orientation() const { return m_orientation.load(std::memory_order_relaxed); }
    bool showBar() const { return m_bar.load(std::memory_order_relaxed); }
    int textMode() const { return m_text.load(std::memory_order_relaxed); }
    int anchor() const { return m_anchor.load(std::memory_order_relaxed); }
    int offsetX() const { return m_offsetX.load(std::memory_order_relaxed); }
    int offsetY() const { return m_offsetY.load(std::memory_order_relaxed); }

    static void* hudManager();
    static const void* stackFor(const void* collectionName, int index);

protected:
    void onUpdate() override;
    void onEnabledChanged(bool enabled) override;

private:
    ArmorHUD() = default;

    void publish();
    bool wanted() const;
    const void* stackOf(int index) const;
    const void* selectedStack(const std::byte* player, void* stackVtable) const;

    static void onHudCreated(void* ctrl);

    std::atomic<int> m_orientation{armorhud::kDefaultOrientation};
    std::atomic<bool> m_bar{armorhud::kDefaultBar};
    std::atomic<int> m_text{armorhud::kDefaultText};
    std::atomic<int> m_anchor{armorhud::kDefaultAnchor};
    std::atomic<int> m_offsetX{armorhud::kDefaultOffsetX};
    std::atomic<int> m_offsetY{armorhud::kDefaultOffsetY};
    std::atomic<bool> m_definitionRegistered{false};
    std::atomic<bool> m_shuttingDown{false};

    std::size_t m_equipStride = 0;
    std::size_t m_armorField = 0;
    std::size_t m_itemsOffset = 0;
    std::size_t m_stackStride = 0;
    std::int32_t m_maxDamageSlot = 0;
    std::ptrdiff_t m_inventoryField = 0;
    std::ptrdiff_t m_containerIdField = 0;
    std::ptrdiff_t m_selectedField = 0;
    void* m_damageValue = nullptr;

    std::atomic<void*> m_armorContainer{nullptr};
    std::atomic<void*> m_armorOwner{nullptr};

    std::string m_lastText[armorhud::kItems];
};

}
