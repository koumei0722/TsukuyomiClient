#include "game/HeldItem.h"
#include "game/HeldItemLogic.h"

#include "core/Logger.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

namespace tsukuyomi::helditem {
namespace {

using SelectedItemFn = void* (__fastcall*)(void*);

bool g_resolved = false;
bool g_ready = false;
std::uint32_t g_selectedSlot = 0;
int g_faultLogs = 0;

int accessViolationFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

void noteFault()
{
    if (g_faultLogs < 3) {
        ++g_faultLogs;
        log().warn(L"HeldItem: reading game memory faulted; skipping the request");
    }
}

}

void resolve()
{
    if (g_resolved) return;
    g_resolved = true;
    const Scanner& scanner = Scanner::instance();
    if (!scanner.found(Target::SelectedItemSlotSite)) {
        log().warn(L"HeldItem: SelectedItemSlotSite was not found");
        return;
    }
    const std::byte* selected = scanner.address(Target::SelectedItemSlotSite);
    if (!memory::isReadable(selected, 24)
        || !readSelectedItemSlot(selected, 24, g_selectedSlot)) {
        log().warn(L"HeldItem: selected item slot could not be resolved");
        return;
    }
    g_ready = true;
    log().info(L"HeldItem: resolved (selected item +{:#x})", g_selectedSlot);
}

bool ready()
{
    return g_ready;
}

bool mainHandEmpty(void* gameMode, bool& empty)
{
    if (!g_ready || !gameMode) return false;
    __try {
        void* player = *reinterpret_cast<void**>(static_cast<std::byte*>(gameMode) + 8);
        if (!player) return false;
        void** vtable = *reinterpret_cast<void***>(player);
        if (!vtable) return false;
        auto selectedItem = reinterpret_cast<SelectedItemFn>(
            *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(vtable) + g_selectedSlot));
        if (!selectedItem) return false;
        auto* stack = static_cast<std::byte*>(selectedItem(player));
        if (!stack) return false;
        empty = *reinterpret_cast<std::uint8_t*>(stack + 0x23) != 1
                || *reinterpret_cast<std::uint8_t*>(stack + 0x22) == 0;
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        noteFault();
        return false;
    }
}

}
