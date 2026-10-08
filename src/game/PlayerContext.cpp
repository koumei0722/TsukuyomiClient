#include "game/PlayerContext.h"

#include "game/GameData.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>

#include "core/Logger.h"
#include "game/BlockWrite.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

namespace tsukuyomi {

namespace {

constexpr std::size_t kPlayerVtableDisp = 3;

const std::byte* vtableFrom(Target target, std::size_t disp)
{
    std::byte* const ref = Scanner::instance().address(target);
    if (ref == nullptr) {
        return nullptr;
    }
    return static_cast<const std::byte*>(memory::ripTarget(ref, disp));
}

constexpr std::ptrdiff_t kEntityContextOffset = 0x08;

int accessViolationFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

bool readPointerGuarded(const void* address, void*& value)
{
    __try {
        value = *static_cast<void* const*>(address);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool hasVtable(const void* address, Target target, std::size_t disp)
{
    const std::byte* const vtable = vtableFrom(target, disp);
    if (address == nullptr || vtable == nullptr) {
        return false;
    }
    void* head = nullptr;
    return readPointerGuarded(address, head) && head == vtable;
}

}

PlayerContext& PlayerContext::instance()
{
    static PlayerContext object;
    return object;
}

void PlayerContext::onEntityContext(void* entityContext)
{
    if (entityContext == nullptr) {
        return;
    }
    auto* const player = static_cast<std::byte*>(entityContext) - kEntityContextOffset;
    if (!hasVtable(player, Target::PlayerVtableRef, kPlayerVtableDisp)) {
        GameData::instance().setPlayerAlt(player);
        return;
    }

    GameData::instance().setPlayer(player);

    if (void* const previous = m_player.exchange(player, std::memory_order_acq_rel);
        previous != player) {
        constexpr unsigned long long kLiveViewMs = 2000;
        const unsigned long long sinceView = GameData::instance().msSinceView();
        const bool leftAWorld = previous != nullptr || sinceView > kLiveViewMs;
        blockwrite::noteWorldChanged(leftAWorld);
        log().info(L"PlayerContext: player found at {:#x} ({}; last view {} ms ago)",
                   reinterpret_cast<std::uintptr_t>(player),
                   leftAWorld ? L"came from another world" : L"first seen in this world",
                   sinceView == ~0ULL ? -1LL : static_cast<long long>(sinceView));
    }
}

}
