#include "modules/FastBlockBreak.h"

#include "core/Logger.h"
#include "hooks/Detours.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <chrono>

namespace tsukuyomi {

namespace {

using StopFn = void(__fastcall*)(void*, const void*);
using StartFn = bool(__fastcall*)(void*, const void*, const void*, std::uint32_t, bool*);

thread_local void* t_localContinueGm = nullptr;

bool readLocalPlayerVtable(void* gameMode, std::size_t playerOff, void*& out)
{
    if (!memory::plausiblePointer(gameMode)) return false;
    __try {
        void* player = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(gameMode) + playerOff);
        if (!memory::plausiblePointer(player)) return false;
        out = *reinterpret_cast<void**>(player);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readUntil(void* gameMode, std::size_t untilOff, std::int64_t& out)
{
    __try {
        out = *reinterpret_cast<std::int64_t*>(reinterpret_cast<std::byte*>(gameMode) + untilOff);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readPos(const void* pos, fastblockbreak::BlockPos& out)
{
    if (pos == nullptr) return false;
    __try {
        out = *reinterpret_cast<const fastblockbreak::BlockPos*>(pos);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}

FastBlockBreak& FastBlockBreak::instance()
{
    static FastBlockBreak module;
    return module;
}

bool FastBlockBreak::available() const
{
    return m_ready.load(std::memory_order_acquire)
        && hooks::hasGameModeContinueDestroyBlock() && hooks::hasGameModeDestroyBlock();
}

MenuItem FastBlockBreak::buildMenu()
{
    return Module::buildMenu();
}

void FastBlockBreak::onScansReady()
{
    const Scanner& scanner = Scanner::instance();
    const std::byte* const continueAt = scanner.address(Target::GameModeContinueDestroyBlock);
    const std::byte* const destroyAt = scanner.address(Target::GameModeDestroyBlock);
    const std::byte* const stopAt = scanner.address(Target::GameModeStopDestroyBlock);
    const std::byte* const startAt = scanner.address(Target::GameModeStartDestroyWrapper);
    const std::byte* const playerRef = scanner.address(Target::PlayerVtableRef);

    fastblockbreak::Offsets offsets;
    if (continueAt == nullptr || destroyAt == nullptr || stopAt == nullptr || startAt == nullptr
        || playerRef == nullptr
        || !memory::isReadable(continueAt, 94) || !memory::isReadable(stopAt, 47)
        || !fastblockbreak::readOffsets({continueAt, 94}, {stopAt, 47}, offsets)) {
        log().warn(L"FastBlockBreak: preparation failed (signatures or offsets)");
        return;
    }

    m_localPlayerVtable = memory::ripTarget(playerRef, 3);
    if (m_localPlayerVtable == nullptr || !hooks::hasGameModeContinueDestroyBlock()
        || !hooks::hasGameModeDestroyBlock()) {
        log().warn(L"FastBlockBreak: preparation failed (player vtable or hooks)");
        return;
    }

    m_playerOff = offsets.player;
    m_untilOff = offsets.until;
    m_ready.store(true, std::memory_order_release);
    m_active.store(enabled(), std::memory_order_release);

    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    log().info(L"FastBlockBreak: ready continue RVA {:#x}, destroy RVA {:#x}, stop RVA {:#x}, start RVA {:#x}, "
               L"kPlayerOff {:#x}, kUntilOff {:#x}",
               reinterpret_cast<std::uintptr_t>(continueAt) - base,
               reinterpret_cast<std::uintptr_t>(destroyAt) - base,
               reinterpret_cast<std::uintptr_t>(stopAt) - base,
               reinterpret_cast<std::uintptr_t>(startAt) - base, m_playerOff, m_untilOff);
}

void FastBlockBreak::onEnabledChanged(bool enabled)
{
    m_toggleGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_active.store(enabled, std::memory_order_release);
}

bool FastBlockBreak::isLocalGameMode(void* gameMode) const
{
    void* vtable = nullptr;
    return readLocalPlayerVtable(gameMode, m_playerOff, vtable) && vtable == m_localPlayerVtable;
}

bool FastBlockBreak::onContinueDestroyBlock(void* gameMode, const void* pos, std::uint8_t face,
                                            const void* playerPos, bool* out)
{
    if (!available() || !m_active.load(std::memory_order_acquire)
        || !isLocalGameMode(gameMode)) {
        return hooks::callGameModeContinueDestroyBlock(gameMode, pos, face, playerPos, out);
    }

    const unsigned int generation = m_toggleGeneration.load(std::memory_order_acquire);
    if (m_seenGeneration != generation) {
        m_armedGm = nullptr;
        m_seenGeneration = generation;
    }
    if (!m_active.load(std::memory_order_acquire)) {
        return hooks::callGameModeContinueDestroyBlock(gameMode, pos, face, playerPos, out);
    }

    if (m_armedGm == gameMode) {
        fastblockbreak::BlockPos nextPos;
        std::int64_t until = 0;
        const std::int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
        const bool readable = readUntil(gameMode, m_untilOff, until) && readPos(pos, nextPos);
        if (!readable || until <= now) {
            m_armedGm = nullptr;
        } else if (fastblockbreak::shouldStop(gameMode, gameMode, until, now, m_brokenPos, nextPos)) {
            m_armedGm = nullptr;
            const auto stop = Scanner::instance().addressAs<StopFn>(Target::GameModeStopDestroyBlock);
            stop(gameMode, &m_brokenPos);
            const auto start = Scanner::instance().addressAs<StartFn>(Target::GameModeStartDestroyWrapper);
            const bool started = start(gameMode, pos, nullptr, face, out);
            if (m_skipLogs < 8) {
                ++m_skipLogs;
                log().info(L"FastBlockBreak: skipped the wait ({} ms left) at {},{},{} (started {})",
                           (until - now) / 1000000, nextPos.x, nextPos.y, nextPos.z, started);
            }
            return started;
        }
    }

    t_localContinueGm = gameMode;
    const bool result = hooks::callGameModeContinueDestroyBlock(gameMode, pos, face, playerPos, out);
    t_localContinueGm = nullptr;
    return result;
}

bool FastBlockBreak::onDestroyBlock(void* gameMode, const void* pos, std::uint8_t face)
{
    const bool result = hooks::callGameModeDestroyBlock(gameMode, pos, face);
    if (t_localContinueGm == gameMode) {
        fastblockbreak::BlockPos brokenPos;
        if (readPos(pos, brokenPos)) {
            m_brokenPos = brokenPos;
            m_armedGm = gameMode;
        }
    }
    return result;
}

}
