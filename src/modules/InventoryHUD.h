#pragma once

#include <atomic>
#include <cstdint>

#include "game/InventoryHudLayout.h"
#include "modules/Module.h"

namespace tsukuyomi {

class InventoryHUD : public Module {
public:
    static InventoryHUD& instance();

    const wchar_t* name() const override { return L"InventoryHUD"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    void onScansReady() override;
    void shutdown() override;

    int anchor() const { return m_anchor.load(std::memory_order_relaxed); }
    int offsetX() const { return m_offsetX.load(std::memory_order_relaxed); }
    int offsetY() const { return m_offsetY.load(std::memory_order_relaxed); }
    bool offhandSlot() const { return m_offhand.load(std::memory_order_relaxed); }

    static void* hudManager();
    static const void* offhandStackFor(const void* collectionName, int index);

protected:
    void onUpdate() override;
    void onEnabledChanged(bool enabled) override;

private:
    InventoryHUD() = default;

    void publish();

    static bool offhandHasItem();
    bool offhandWanted() const;

    static void onHudCreated(void* ctrl);

    std::atomic<int> m_anchor{invhud::kDefaultAnchor};
    std::atomic<int> m_offsetX{invhud::kDefaultOffsetX};
    std::atomic<int> m_offsetY{invhud::kDefaultOffsetY};
    std::atomic<bool> m_offhand{invhud::kDefaultOffhand};
    std::atomic<bool> m_offhandBlocked{false};
    std::atomic<bool> m_definitionRegistered{false};
    std::atomic<bool> m_shuttingDown{false};
};

}
