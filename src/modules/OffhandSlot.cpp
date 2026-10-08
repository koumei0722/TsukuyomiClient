#include "modules/OffhandSlot.h"

#include "core/Logger.h"
#include "game/ContainerUi.h"
#include "game/GameData.h"
#include "game/InventoryHudLayout.h"
#include "game/UiProbe.h"
#include "memory/Memory.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tsukuyomi {

namespace {

namespace cui = containerui;

constexpr int kSlotOffhand = 5;

void writeBool(int slot, bool value)
{
    if (volatile std::uint8_t* const p = cui::persistentBool(slot)) {
        *p = value ? 1 : 0;
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

OffhandSlot& OffhandSlot::instance()
{
    static OffhandSlot module;
    return module;
}

void OffhandSlot::onScansReady()
{
    if (!cui::bindingsAvailable() || cui::persistentBool(0) == nullptr) {
        log().warn(L"OffhandSlot: NOT usable (bindings {} / value storage {})",
                   cui::bindingsAvailable() ? L"ready" : L"missing",
                   cui::persistentBool(0) != nullptr ? L"ready" : L"missing");
        return;
    }
    if (isWriteBlocked()) {
        return;
    }
    publish();
    cui::addHudObserver(&OffhandSlot::onHudCreated);
    uiprobe::registerDefExtension("hud", "hotbar_panel", invhud::offhandJson(), "");
    m_definitionRegistered = true;
    log().info(L"OffhandSlot: ready (the slot appears in worlds entered after injection)");
}

void OffhandSlot::publish()
{
    writeBool(kSlotOffhand, wanted() && offhandHasItem());
}

bool OffhandSlot::wanted() const
{
    return enabled() && !m_shuttingDown.load(std::memory_order_relaxed);
}

void* OffhandSlot::hudManager()
{
    return g_hudMc.load(std::memory_order_relaxed);
}

const void* OffhandSlot::offhandStackFor(const void* collectionName, int index)
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
        log().info(L"OffhandSlot: the HUD asked for offhand_items (answering with the player's offhand)");
    }
    if (!instance().wanted()) {
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

bool OffhandSlot::offhandHasItem()
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
            log().warn(L"OffhandSlot: cannot read the offhand of the player ({}); the offhand slot stays hidden",
                       read ? L"the offhand and mainhand types differ" : L"access violation");
        }
        return false;
    }
    return item != nullptr && count > 0;
}

void OffhandSlot::onUpdate()
{
    if (!m_definitionRegistered.load(std::memory_order_relaxed)) {
        return;
    }
    publish();
    if (g_offhandAsks.load(std::memory_order_relaxed) == 0 && g_hudMc.load(std::memory_order_relaxed) != nullptr
        && !g_neverAskedWarned.load(std::memory_order_relaxed) && wanted() && offhandHasItem()
        && GetTickCount64() - g_hudCreatedAt.load(std::memory_order_relaxed) > 10000) {
        g_neverAskedWarned.store(true, std::memory_order_relaxed);
        log().warn(L"OffhandSlot: the HUD never asked for offhand_items in 10 s; the offhand slot stays empty");
    }
}

void OffhandSlot::onEnabledChanged(bool)
{
    if (m_definitionRegistered.load(std::memory_order_relaxed)) {
        publish();
    }
}

void OffhandSlot::shutdown()
{
    m_shuttingDown.store(true, std::memory_order_relaxed);
    if (m_definitionRegistered.load(std::memory_order_relaxed)) {
        publish();
    }
}

void OffhandSlot::onHudCreated(void* ctrl)
{
    OffhandSlot& self = instance();
    if (!self.m_definitionRegistered) {
        return;
    }
    void* const mc = cui::hudContainerManager(ctrl);
    g_hudMc.store(mc, std::memory_order_relaxed);
    g_hudCreatedAt.store(GetTickCount64(), std::memory_order_relaxed);
    g_neverAskedWarned.store(false, std::memory_order_relaxed);
    self.publish();
    const bool bound = cui::bindPersistentBool(ctrl, invhud::kBindOffhand, kSlotOffhand);
    if (g_bindLogs.fetch_add(1, std::memory_order_relaxed) < kMaxBindLogs) {
        if (bound) {
            log().info(L"OffhandSlot: bound the slot to the HUD controller");
        } else {
            log().warn(L"OffhandSlot: could not bind the slot to the HUD controller");
        }
        if (mc == nullptr) {
            log().warn(L"OffhandSlot: the HUD container manager is unknown; the offhand slot stays empty");
        }
    }
}

}
