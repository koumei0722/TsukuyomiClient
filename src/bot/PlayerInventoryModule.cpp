#include "PlayerInventoryModule.h"
#include "bot/PlayerInteract.h"

namespace tsukuyomi {
PlayerInventory& PlayerInventory::instance() { static PlayerInventory value; return value; }
bool PlayerInventory::available() const { return bot::playerinteract::available(); }
void PlayerInventory::onScansReady()
{
    bot::playerinteract::readLayout();
    bot::playerinteract::setEnabled(available() && enabled());
}
void PlayerInventory::onEnabledChanged(bool enabled) { bot::playerinteract::setEnabled(enabled && available()); }
}
