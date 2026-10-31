#include "game/BuildOver.h"
#include "game/BuildOverLogic.h"

#include "core/Logger.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

namespace tsukuyomi::buildover {
namespace {

using BlockSourceFn = void* (__fastcall*)(void*);
using GetBlockFn = const void* (__fastcall*)(void*, const std::int32_t*);
using BuildOverFn = bool (__fastcall*)(void*, const void*, void*, const std::int32_t*);

bool g_resolved = false;
bool g_ready = false;
PlaceFields g_place{};
int g_faultLogs = 0;

int accessViolationFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

int askGuarded(void* gameMode, const std::int32_t* pos)
{
    __try {
        void* player = *reinterpret_cast<void**>(static_cast<std::byte*>(gameMode) + 8);
        if (!player) return -1;
        void* dimension = *reinterpret_cast<void**>(static_cast<std::byte*>(player) + g_place.dimensionOffset);
        if (!dimension) return -1;
        void** dimensionVtable = *reinterpret_cast<void***>(dimension);
        if (!dimensionVtable) return -1;
        auto sourceFn = reinterpret_cast<BlockSourceFn>(
            *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(dimensionVtable) + g_place.blockSourceSlot));
        if (!sourceFn) return -1;
        void* source = sourceFn(dimension);
        if (!source) return -1;
        void** sourceVtable = *reinterpret_cast<void***>(source);
        if (!sourceVtable) return -1;
        auto getBlock = reinterpret_cast<GetBlockFn>(
            *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(sourceVtable) + g_place.getBlockSlot));
        if (!getBlock) return -1;
        const void* block = getBlock(source, pos);
        if (!block) return -1;
        void* type = *reinterpret_cast<void* const*>(static_cast<const std::byte*>(block) + g_place.blockTypeOffset);
        if (!type) return -1;
        void** typeVtable = *reinterpret_cast<void***>(type);
        if (!typeVtable) return -1;
        auto buildOver = reinterpret_cast<BuildOverFn>(
            *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(typeVtable) + g_place.buildOverSlot));
        if (!buildOver) return -1;
        return buildOver(type, block, source, pos) ? 1 : 0;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return -2;
    }
}

}

void resolve()
{
    if (g_resolved) return;
    g_resolved = true;
    const Scanner& scanner = Scanner::instance();
    if (!scanner.found(Target::BuildBlockPlaceCheckSite)) {
        log().warn(L"BuildOver: BuildBlockPlaceCheckSite was not found");
        return;
    }
    const std::byte* site = scanner.address(Target::BuildBlockPlaceCheckSite);
    if (!memory::isReadable(site, kPlaceSiteBytes) || !readPlaceFields(site, kPlaceSiteBytes, g_place)) {
        log().warn(L"BuildOver: the build-over check fields could not be read");
        return;
    }
    g_ready = true;
    log().info(L"BuildOver: resolved (dimension +{:#x}, block source +{:#x}, get block +{:#x}, block type +{:#x}, build over +{:#x})",
               g_place.dimensionOffset, g_place.blockSourceSlot, g_place.getBlockSlot, g_place.blockTypeOffset,
               g_place.buildOverSlot);
}

bool ready()
{
    return g_ready;
}

int canBuildOver(void* gameMode, const std::int32_t pos[3])
{
    if (!g_ready || !gameMode || !pos) return -1;
    const int answer = askGuarded(gameMode, pos);
    if (answer == -2 && g_faultLogs < 3) {
        ++g_faultLogs;
        log().warn(L"BuildOver: asking the game faulted at {}, {}, {}", pos[0], pos[1], pos[2]);
    }
    return answer < 0 ? -1 : answer;
}

}
