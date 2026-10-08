#include "modules/InventoryHUD.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "game/ContainerUi.h"
#include "game/UiProbe.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace tsukuyomi {

namespace {

namespace cui = containerui;

enum BoolSlot { kSlotVisible = 0, kSlotVMid, kSlotVBottom, kSlotHMid, kSlotHRight };
enum FloatSlot { kSlotX = 0, kSlotY };

void writeBool(int slot, bool value)
{
    if (volatile std::uint8_t* const p = cui::persistentBool(slot)) {
        *p = value ? 1 : 0;
    }
}
void writeFloat(int slot, float value)
{
    if (volatile float* const p = cui::persistentFloat(slot)) {
        *p = value;
    }
}

std::atomic<int> g_bindLogs{0};
constexpr int kMaxBindLogs = 4;

}

InventoryHUD& InventoryHUD::instance()
{
    static InventoryHUD module;
    return module;
}

bool InventoryHUD::available() const
{
    return m_definitionRegistered;
}

void InventoryHUD::onScansReady()
{
    if (!cui::bindingsAvailable() || !cui::floatBindingsAvailable() || cui::persistentBool(0) == nullptr) {
        log().warn(L"InventoryHUD: NOT usable (bindings {} / float bindings {} / value storage {})",
                   cui::bindingsAvailable() ? L"ready" : L"missing",
                   cui::floatBindingsAvailable() ? L"ready" : L"missing",
                   cui::persistentBool(0) != nullptr ? L"ready" : L"missing");
        return;
    }
    publish();
    cui::addHudObserver(&InventoryHUD::onHudCreated);
    uiprobe::registerDefExtension("hud", "hud_content", "", invhud::layoutJson());
    m_definitionRegistered = true;
    log().info(L"InventoryHUD: ready (the 27 cells appear in worlds entered after injection)");
}

void InventoryHUD::publish()
{
    const invhud::Placement place = invhud::placementOf(anchor());
    writeBool(kSlotVisible, enabled() && !m_shuttingDown.load(std::memory_order_relaxed));
    writeBool(kSlotVMid, place.vMid);
    writeBool(kSlotVBottom, place.vBottom);
    writeBool(kSlotHMid, place.hMid);
    writeBool(kSlotHRight, place.hRight);
    writeFloat(kSlotX, static_cast<float>(offsetX()) / static_cast<float>(invhud::kWidth));
    writeFloat(kSlotY, static_cast<float>(offsetY()) / static_cast<float>(invhud::kHeight));
}

void InventoryHUD::onUpdate()
{
    publish();
}

void InventoryHUD::onEnabledChanged(bool)
{
    publish();
}

void InventoryHUD::shutdown()
{
    m_shuttingDown.store(true, std::memory_order_relaxed);
    publish();
}

void InventoryHUD::onHudCreated(void* ctrl)
{
    InventoryHUD& self = instance();
    if (!self.m_definitionRegistered) {
        return;
    }
    self.publish();
    int ok = 0;
    ok += cui::bindPersistentBool(ctrl, invhud::kBindVisible, kSlotVisible) ? 1 : 0;
    ok += cui::bindPersistentBool(ctrl, invhud::kBindVMid, kSlotVMid) ? 1 : 0;
    ok += cui::bindPersistentBool(ctrl, invhud::kBindVBottom, kSlotVBottom) ? 1 : 0;
    ok += cui::bindPersistentBool(ctrl, invhud::kBindHMid, kSlotHMid) ? 1 : 0;
    ok += cui::bindPersistentBool(ctrl, invhud::kBindHRight, kSlotHRight) ? 1 : 0;
    ok += cui::bindPersistentFloat(ctrl, invhud::kBindX, kSlotX) ? 1 : 0;
    ok += cui::bindPersistentFloat(ctrl, invhud::kBindY, kSlotY) ? 1 : 0;
    constexpr int kWanted = 7;
    if (g_bindLogs.fetch_add(1, std::memory_order_relaxed) < kMaxBindLogs) {
        if (ok == kWanted) {
            log().info(L"InventoryHUD: bound {} values to the HUD controller", ok);
        } else {
            log().warn(L"InventoryHUD: bound only {} of {} values to the HUD controller", ok, kWanted);
        }
    }
}

MenuItem InventoryHUD::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());

    std::vector<std::wstring> anchors;
    for (int i = 0; i < invhud::kAnchorCount; ++i) {
        anchors.emplace_back(invhud::anchorLabel(i));
    }
    children.push_back(menu::choice(
        L"Anchor", std::move(anchors), [this] { return anchor(); },
        [this](int value) { m_anchor.store(invhud::clampAnchor(value), std::memory_order_relaxed); }));
    children.push_back(menu::number(
        L"Offset X", [this] { return static_cast<float>(offsetX()); },
        [this](float value) {
            m_offsetX.store(invhud::clampOffset(static_cast<int>(std::lround(value))), std::memory_order_relaxed);
        },
        true, static_cast<float>(invhud::kOffsetMin), static_cast<float>(invhud::kOffsetMax)));
    children.push_back(menu::number(
        L"Offset Y", [this] { return static_cast<float>(offsetY()); },
        [this](float value) {
            m_offsetY.store(invhud::clampOffset(static_cast<int>(std::lround(value))), std::memory_order_relaxed);
        },
        true, static_cast<float>(invhud::kOffsetMin), static_cast<float>(invhud::kOffsetMax)));
    {
        MenuItem reset = menu::action(L"Reset offset", [this] {
            m_offsetX.store(0, std::memory_order_relaxed);
            m_offsetY.store(0, std::memory_order_relaxed);
        });
        reset.value = [] { return std::wstring(L"Reset"); };
        children.push_back(std::move(reset));
    }

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void InventoryHUD::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    m_anchor.store(invhud::clampAnchor(Config::getInt(section, "anchor", invhud::kDefaultAnchor)),
                   std::memory_order_relaxed);
    m_offsetX.store(invhud::clampOffset(Config::getInt(section, "offsetX", invhud::kDefaultOffsetX)),
                    std::memory_order_relaxed);
    m_offsetY.store(invhud::clampOffset(Config::getInt(section, "offsetY", invhud::kDefaultOffsetY)),
                    std::memory_order_relaxed);
}

void InventoryHUD::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["anchor"] = anchor();
    section["offsetX"] = offsetX();
    section["offsetY"] = offsetY();
}

}
