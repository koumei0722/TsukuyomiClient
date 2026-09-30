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

struct ModuleRange {
    const std::byte* base = nullptr;
    std::size_t size = 0;

    bool contains(const void* address) const
    {
        const auto* const value = static_cast<const std::byte*>(address);
        return base != nullptr && value >= base && value < base + size;
    }

    std::size_t rvaOf(const void* address) const
    {
        return contains(address)
                   ? static_cast<std::size_t>(static_cast<const std::byte*>(address) - base)
                   : 0;
    }
};

const ModuleRange& mainModule()
{
    static const ModuleRange range = [] {
        ModuleRange result;
        const auto* const base = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));
        if (base == nullptr) {
            return result;
        }
        const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return result;
        }
        const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            return result;
        }
        result.base = base;
        result.size = nt->OptionalHeader.SizeOfImage;
        return result;
    }();
    return range;
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
        const bool kept = GameData::instance().setPlayerAlt(player);
        void* head = nullptr;
        readPointerGuarded(player, head);
        noteOtherTarget(head, GameData::instance().isServerPlayer(player), kept);
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

void PlayerContext::noteOtherTarget(const void* vtable, bool serverType, bool kept)
{
    const std::size_t used = (std::min)(m_otherTargetCount.load(std::memory_order_acquire), kOtherTargetLogs);
    for (std::size_t i = 0; i < used; ++i) {
        if (m_otherTargetVtables[i].load(std::memory_order_relaxed) == vtable) {
            return;
        }
    }
    const std::size_t at = m_otherTargetCount.fetch_add(1, std::memory_order_acq_rel);
    if (at >= kOtherTargetLogs) {
        return;
    }
    m_otherTargetVtables[at].store(vtable, std::memory_order_relaxed);
    log().info(L"PlayerContext: skipped a game mode target whose owner is not the local player "
               L"(vtable rva {:#x}; {})",
               mainModule().rvaOf(vtable),
               kept         ? L"remembered as the server-side player of this local world"
               : serverType ? L"a ServerPlayer whose position is unreadable, so it is not remembered"
                            : L"not a ServerPlayer, so it is not remembered");
}

}
