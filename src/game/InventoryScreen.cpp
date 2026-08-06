#include "game/InventoryScreen.h"

#include "memory/Memory.h"

#include <Windows.h>

#include <cstring>

namespace tsukuyomi {

namespace {

constexpr char kHotbarName[] = "hotbar_items";
constexpr char kInventoryName[] = "inventory_items";
constexpr char kOffhandName[] = "offhand_items";
constexpr char kArmorName[] = "armor_items";

}

InventoryScreen& InventoryScreen::instance()
{
    static InventoryScreen screen;
    return screen;
}

std::uint64_t InventoryScreen::pack(const Hovered& value)
{
    const auto area = static_cast<std::uint64_t>(static_cast<int>(value.area)) & 0xFFull;
    const auto index =
        static_cast<std::uint64_t>(static_cast<std::uint16_t>(static_cast<std::int16_t>(value.index)));
    const auto slot = static_cast<std::uint64_t>(
        static_cast<std::uint16_t>(static_cast<std::int16_t>(value.containerSlot)));
    return area | (index << 8) | (slot << 24);
}

InventoryScreen::Hovered InventoryScreen::unpack(std::uint64_t value)
{
    Hovered out;
    out.area = static_cast<Area>(static_cast<int>(value & 0xFFull));
    out.index = static_cast<std::int16_t>((value >> 8) & 0xFFFFull);
    out.containerSlot = static_cast<std::int16_t>((value >> 24) & 0xFFFFull);
    return out;
}

bool InventoryScreen::readString(const void* text, char* out, std::size_t outSize)
{
    const auto* const base = static_cast<const std::byte*>(text);

    std::size_t length = 0;
    std::size_t capacity = 0;
    std::memcpy(&length, base + kStringLengthOffset, sizeof(length));
    std::memcpy(&capacity, base + kStringCapacityOffset, sizeof(capacity));

    if (length == 0 || length >= outSize || capacity < length) {
        return false;
    }

    const std::byte* chars = base;
    if (capacity > kShortStringCapacity) {

        const void* pointer = nullptr;
        std::memcpy(&pointer, base, sizeof(pointer));
        if (!memory::isReadable(pointer, length)) {
            return false;
        }
        chars = static_cast<const std::byte*>(pointer);
    }

    std::memcpy(out, chars, length);
    out[length] = '\0';
    return true;
}

InventoryScreen::Hovered InventoryScreen::readFrom(const void* controller)
{
    Hovered out;
    if (controller == nullptr) {
        return out;
    }

    const auto* const base = static_cast<const std::byte*>(controller);
    if (!memory::isReadable(base + kHoverNameOffset, kStringSize)
        || !memory::isReadable(base + kHoverIndexOffset, sizeof(std::int32_t))) {
        return out;
    }

    char name[kNameMax] = {};
    if (!readString(base + kHoverNameOffset, name, sizeof(name))) {
        return out;
    }

    std::int32_t index = 0;
    std::memcpy(&index, base + kHoverIndexOffset, sizeof(index));

    if (std::strcmp(name, kHotbarName) == 0) {
        if (index >= 0 && index < kHotbarSlots) {
            out.area = Area::Hotbar;
            out.index = index;
            out.containerSlot = index;
        }
    } else if (std::strcmp(name, kInventoryName) == 0) {
        if (index >= 0 && index < kSlotCount - kHotbarSlots) {
            out.area = Area::Inventory;
            out.index = index;
            out.containerSlot = kHotbarSlots + index;
        }
    } else if (std::strcmp(name, kOffhandName) == 0) {
        if (index == 0) {
            out.area = Area::Offhand;
            out.index = index;
        }
    } else if (std::strcmp(name, kArmorName) == 0) {
        if (index >= 0 && index < kArmorSlots) {
            out.area = Area::Armor;
            out.index = index;
        }
    } else {
        out.area = Area::Other;
    }

    return out;
}

void InventoryScreen::onController(const void* controller)
{
    m_hovered.store(pack(readFrom(controller)), std::memory_order_relaxed);
    m_seenAtMs.store(GetTickCount64(), std::memory_order_relaxed);

    void* vtable = nullptr;
    if (controller != nullptr && memory::isReadable(controller, sizeof(void*))) {
        vtable = *reinterpret_cast<void* const*>(controller);
    }
    m_controllerVtable.store(vtable, std::memory_order_relaxed);

    m_controller.store(controller, std::memory_order_relaxed);
}

bool InventoryScreen::ownScreenOpen() const
{
    if (!screenLooksOpen()) {
        return false;
    }
    void* const vtable = m_controllerVtable.load(std::memory_order_relaxed);
    if (vtable == nullptr) {
        return false;
    }
    const auto* const base = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));
    if (base == nullptr) {
        return false;
    }
    return vtable == static_cast<const void*>(base + kOwnControllerVtableRva);
}

const void* InventoryScreen::controller() const
{

    if (!screenLooksOpen()) {
        return nullptr;
    }
    return m_controller.load(std::memory_order_relaxed);
}

bool InventoryScreen::screenLooksOpen() const
{
    const unsigned long long seen = m_seenAtMs.load(std::memory_order_relaxed);
    if (seen == 0) {
        return false;
    }
    const unsigned long long now = GetTickCount64();
    return now >= seen && now - seen <= kFreshMs;
}

InventoryScreen::Hovered InventoryScreen::hovered() const
{
    if (!screenLooksOpen()) {
        return Hovered{};
    }
    return unpack(m_hovered.load(std::memory_order_relaxed));
}

}
