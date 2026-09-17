#include "modules/FastInventory.h"

#include "core/Logger.h"
#include "game/InventoryScreen.h"
#include "hooks/Detours.h"
#include "memory/Scanner.h"

#include <Windows.h>

namespace tsukuyomi {

namespace {

}

FastInventory& FastInventory::instance()
{
    static FastInventory module;
    return module;
}

bool FastInventory::available() const
{
    return Scanner::instance().found(Target::OpenInventoryScreen);
}

void FastInventory::onScansReady()
{
    if (!available() && enabled()) {
        log().warn(L"FastInventory: the screen-open function was not found, so the module "
                   L"cannot work");
    }
}

void FastInventory::onInventoryOpenSent(void* client)
{
    if (!enabled() || client == nullptr || !available()) {
        return;
    }
    if (m_broken.load(std::memory_order_acquire)) {
        return;
    }

    if (InventoryScreen::instance().screenLooksOpen()) {
        return;
    }

    const hooks::InventoryOpenResult result = hooks::callOpenInventoryScreen(client);

    switch (result) {
    case hooks::InventoryOpenResult::Opened:
        if (!m_logged.exchange(true, std::memory_order_relaxed)) {
            log().info(L"FastInventory: opened the inventory without waiting for the server");
        }
        break;

    case hooks::InventoryOpenResult::NotReady:
        if (!m_warned.exchange(true, std::memory_order_relaxed)) {
            log().warn(L"FastInventory: the inventory could not be opened (no screen context, "
                       L"or the game's own guard refused)");
        }
        break;

    case hooks::InventoryOpenResult::Faulted:
        if (!m_broken.exchange(true, std::memory_order_acq_rel)) {
            log().error(L"FastInventory: the screen-open function faulted; it will not be "
                        L"called again (back to waiting for the server)");
        }
        break;
    }
}

}
