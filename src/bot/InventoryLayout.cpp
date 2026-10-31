#include "InventoryLayout.h"

namespace tsukuyomi::bot::inventorylayout {
bool toBot(std::uint8_t& container, std::uint8_t& slot)
{
    if (container == 59) return slot == 0;
    if (container != kChestContainer) return false;
    if (slot < 27) { container = 29; slot = static_cast<std::uint8_t>(slot + 9); }
    else if (slot < 36) { container = 28; slot = static_cast<std::uint8_t>(slot - 27); }
    else if (slot < 40) { container = 6; slot = static_cast<std::uint8_t>(slot - 36); }
    else if (slot == 41) { container = 34; slot = 1; }
    else return false;
    return true;
}
bool toChest(std::uint8_t& container, std::uint8_t& slot)
{
    if (container == 59) return slot == 0;
    if ((container == 28 || container == 29 || container == 12) && slot < 36)
        slot = static_cast<std::uint8_t>(slot < 9 ? slot + 27 : slot - 9);
    else if (container == 6 && slot < 4) slot = static_cast<std::uint8_t>(slot + 36);
    else if (container == 34 && slot <= 1) slot = 41;
    else return false;
    container = kChestContainer;
    return true;
}
}
