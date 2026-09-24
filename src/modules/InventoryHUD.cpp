#include "modules/InventoryHUD.h"

#include "config/Config.h"
#include "config/WriteSwitches.h"
#include "core/Logger.h"
#include "game/ContainerUi.h"
#include "game/GameData.h"
#include "game/UiProbe.h"
#include "memory/Memory.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tsukuyomi {

namespace {

namespace cui = containerui;

enum BoolSlot { kSlotVisible = 0, kSlotVMid, kSlotVBottom, kSlotHMid, kSlotHRight, kSlotOffhand };
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

constexpr std::ptrdiff_t kOffhandOffset = 0xD90;
constexpr std::ptrdiff_t kMainhandOffset = 0xE28;
constexpr std::ptrdiff_t kStackItemOffset = 0x08;
constexpr std::ptrdiff_t kStackCountOffset = 0x22;

std::atomic<bool> g_offhandUnreadableLogged{false};

LONG accessFilter(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

bool readHands(const std::byte* player, bool& sameType, void*& item, std::uint8_t& count)
{
    __try {
        void* const offVtable = *reinterpret_cast<void* const*>(player + kOffhandOffset);
        void* const mainVtable = *reinterpret_cast<void* const*>(player + kMainhandOffset);
        sameType = offVtable != nullptr && offVtable == mainVtable;
        item = *reinterpret_cast<void* const*>(player + kOffhandOffset + kStackItemOffset);
        count = *reinterpret_cast<const std::uint8_t*>(player + kOffhandOffset + kStackCountOffset);
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

bool readCollectionName(const void* str, char (&out)[16], std::size_t& size)
{
    __try {
        const auto* const bytes = static_cast<const std::byte*>(str);
        const std::size_t length = *reinterpret_cast<const std::size_t*>(bytes + 0x10);
        const std::size_t capacity = *reinterpret_cast<const std::size_t*>(bytes + 0x18);
        if (length >= sizeof(out)) {
            return false;
        }
        const char* const text = capacity >= 16 ? *reinterpret_cast<const char* const*>(bytes)
                                                : reinterpret_cast<const char*>(bytes);
        for (std::size_t i = 0; i < length; ++i) {
            out[i] = text[i];
        }
        size = length;
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

constexpr char kOffhandCollection[] = "offhand_items";
constexpr std::size_t kOffhandCollectionLength = sizeof(kOffhandCollection) - 1;

std::atomic<unsigned long long> g_offhandAsks{0};
std::atomic<void*> g_hudMc{nullptr};
std::atomic<unsigned long long> g_hudCreatedAt{0};
std::atomic<bool> g_neverAskedWarned{false};

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
    m_offhandBlocked.store(writes::blocked("InventoryHUD:offhand"), std::memory_order_relaxed);
    publish();
    cui::addHudObserver(&InventoryHUD::onHudCreated);
    uiprobe::registerDefExtension("hud", "hud_content", "", invhud::layoutJson());
    uiprobe::registerDefExtension("hud", "hotbar_panel", invhud::offhandJson(), "");
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
    writeBool(kSlotOffhand, offhandWanted() && offhandHasItem());
}

bool InventoryHUD::offhandWanted() const
{
    return enabled() && !m_shuttingDown.load(std::memory_order_relaxed) && offhandSlot()
           && !m_offhandBlocked.load(std::memory_order_relaxed);
}

void* InventoryHUD::hudManager()
{
    return g_hudMc.load(std::memory_order_relaxed);
}

const void* InventoryHUD::offhandStackFor(const void* collectionName, int index)
{
    if (index != 0 || collectionName == nullptr) {
        return nullptr;
    }
    char name[16] = {};
    std::size_t size = 0;
    if (!readCollectionName(collectionName, name, size) || size != kOffhandCollectionLength
        || std::memcmp(name, kOffhandCollection, kOffhandCollectionLength) != 0) {
        return nullptr;
    }
    if (g_offhandAsks.fetch_add(1, std::memory_order_relaxed) == 0) {
        log().info(L"InventoryHUD: the HUD asked for offhand_items (answering with the player's offhand)");
    }
    if (!instance().offhandWanted()) {
        return nullptr;
    }
    void* const player = GameData::instance().player();
    if (!memory::plausiblePointer(player)) {
        return nullptr;
    }
    bool sameType = false;
    void* item = nullptr;
    std::uint8_t count = 0;
    if (!readHands(static_cast<const std::byte*>(player), sameType, item, count) || !sameType) {
        return nullptr;
    }
    return static_cast<const std::byte*>(player) + kOffhandOffset;
}

bool InventoryHUD::offhandHasItem()
{
    void* const player = GameData::instance().player();
    if (!memory::plausiblePointer(player)) {
        return false;
    }
    bool sameType = false;
    void* item = nullptr;
    std::uint8_t count = 0;
    const bool read = readHands(static_cast<const std::byte*>(player), sameType, item, count);
    if (!read || !sameType) {
        if (!g_offhandUnreadableLogged.exchange(true, std::memory_order_relaxed)) {
            log().warn(L"InventoryHUD: cannot read the offhand of the player ({}); the offhand slot stays hidden",
                       read ? L"the offhand and mainhand types differ" : L"access violation");
        }
        return false;
    }
    return item != nullptr && count > 0;
}

void InventoryHUD::onUpdate()
{
    publish();
    if (g_offhandAsks.load(std::memory_order_relaxed) == 0 && g_hudMc.load(std::memory_order_relaxed) != nullptr
        && !g_neverAskedWarned.load(std::memory_order_relaxed) && offhandWanted() && offhandHasItem()
        && GetTickCount64() - g_hudCreatedAt.load(std::memory_order_relaxed) > 10000) {
        g_neverAskedWarned.store(true, std::memory_order_relaxed);
        log().warn(L"InventoryHUD: the HUD never asked for offhand_items in 10 s; the offhand slot stays empty");
    }
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
    void* const mc = cui::hudContainerManager(ctrl);
    g_hudMc.store(mc, std::memory_order_relaxed);
    g_hudCreatedAt.store(GetTickCount64(), std::memory_order_relaxed);
    g_neverAskedWarned.store(false, std::memory_order_relaxed);
    self.publish();
    int ok = 0;
    ok += cui::bindPersistentBool(ctrl, invhud::kBindVisible, kSlotVisible) ? 1 : 0;
    ok += cui::bindPersistentBool(ctrl, invhud::kBindVMid, kSlotVMid) ? 1 : 0;
    ok += cui::bindPersistentBool(ctrl, invhud::kBindVBottom, kSlotVBottom) ? 1 : 0;
    ok += cui::bindPersistentBool(ctrl, invhud::kBindHMid, kSlotHMid) ? 1 : 0;
    ok += cui::bindPersistentBool(ctrl, invhud::kBindHRight, kSlotHRight) ? 1 : 0;
    ok += cui::bindPersistentFloat(ctrl, invhud::kBindX, kSlotX) ? 1 : 0;
    ok += cui::bindPersistentFloat(ctrl, invhud::kBindY, kSlotY) ? 1 : 0;
    ok += cui::bindPersistentBool(ctrl, invhud::kBindOffhand, kSlotOffhand) ? 1 : 0;
    constexpr int kWanted = 8;
    if (g_bindLogs.fetch_add(1, std::memory_order_relaxed) < kMaxBindLogs) {
        if (ok == kWanted) {
            log().info(L"InventoryHUD: bound {} values to the HUD controller", ok);
        } else {
            log().warn(L"InventoryHUD: bound only {} of {} values to the HUD controller", ok, kWanted);
        }
        if (mc == nullptr && !self.m_offhandBlocked.load(std::memory_order_relaxed)) {
            log().warn(L"InventoryHUD: the HUD container manager is unknown; the offhand slot stays empty");
        }
    }
}

MenuItem InventoryHUD::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(menu::back());
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
    {
        MenuItem offhand = menu::toggle(
            L"Offhand slot", [this] { return offhandSlot(); },
            [this] { m_offhand.store(!offhandSlot(), std::memory_order_relaxed); });
        offhand.hidden = writes::blocked("InventoryHUD:offhand");
        children.push_back(std::move(offhand));
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
    m_offhand.store(Config::getBool(section, "offhandSlot", invhud::kDefaultOffhand), std::memory_order_relaxed);
}

void InventoryHUD::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["anchor"] = anchor();
    section["offsetX"] = offsetX();
    section["offsetY"] = offsetY();
    section["offhandSlot"] = offhandSlot();
}

}
