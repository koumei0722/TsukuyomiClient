#include "game/PickItem.h"

#include <Windows.h>
#include <cstring>
#include "core/Notice.h"
#include "game/ContainerUi.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

namespace tsukuyomi::pickitem {
namespace {
using PickFn = void* (__fastcall*)(const void*, void*, const void*, const void*);
using DtorFn = void (__fastcall*)(void*);
std::int32_t g_slot = 0;
DtorFn g_dtor = nullptr;
bool initialize()
{
    static bool tried = false;
    if (tried) return g_slot != 0 && g_dtor != nullptr;
    tried = true;
    const auto& scanner = Scanner::instance();
    const auto* site = scanner.address(Target::BlockAsItemInstance);
    unsigned char instruction[7]{};
    if (site != nullptr && memory::isReadable(site + 0x84, sizeof(instruction)))
        std::memcpy(instruction, site + 0x84, sizeof(instruction));
    std::int32_t slot = 0;
    if (instruction[0] == 0x48 && instruction[1] == 0x8B && instruction[2] == 0x80)
        std::memcpy(&slot, instruction + 3, sizeof(slot));
    g_dtor = scanner.addressAs<DtorFn>(Target::ItemStackBaseDtor);
    if (instruction[0] != 0x48 || instruction[1] != 0x8B || instruction[2] != 0x80
        || slot <= 0 || slot % sizeof(void*) != 0 || g_dtor == nullptr) {
        g_dtor = nullptr;
        notice::failOnce("PickItem.signature", L"Material List: stopped because the pick item function could not be verified.",
                         "Material List pick item function is unavailable.");
        return false;
    }
    g_slot = slot;
    return true;
}
int constructRaw(const void* block, void* out)
{
    __try {
        const auto* bytes = static_cast<const std::byte*>(block);
        const auto* type = *reinterpret_cast<const std::byte* const*>(bytes + 0x68);
        if (type == nullptr) return 0;
        if ((std::to_integer<unsigned char>(type[0x29]) & 2) != 0) return 0;
        const auto* vt = *reinterpret_cast<const std::byte* const*>(type);
        using CanPickFn = bool (__fastcall*)(const void*);
        const auto canPick = *reinterpret_cast<CanPickFn const*>(vt + 0x210);
        const auto pick = *reinterpret_cast<PickFn const*>(vt + g_slot);
        if (!memory::inGameModule(reinterpret_cast<const void*>(canPick))
            || !memory::inGameModule(reinterpret_cast<const void*>(pick))) return -1;
        if (!canPick(type)) return 0;
        pick(type, out, block, nullptr);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
bool destroyRaw(void* out)
{
    __try { g_dtor(out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

Result ofBlock(const void* block)
{
    Result result;
    if (block == nullptr || !initialize() || !memory::isReadable(block, 0x70)) return result;
    alignas(16) std::byte out[0x80]{};
    const int built = constructRaw(block, out);
    if (built == 1) {
        result.item.name = containerui::itemName(out);
        result.item.count = std::to_integer<unsigned char>(out[0x22]);
        const auto item = containerui::itemKey(out);
        if (item != 0 && !result.item.name.empty()) {
            result.iconIdAux = containerui::idAuxOfItem(reinterpret_cast<const void*>(item), containerui::auxOf(out));
            result.hasIcon = result.iconIdAux != 0;
        }
        if (!destroyRaw(out)) {
            g_slot = 0;
            notice::failOnce("PickItem.destroy", L"Material List: stopped picking items because destroying the item failed.",
                             "Material List item destruction failed.");
        }
    } else if (built < 0) {
        g_slot = 0;
        notice::failOnce("PickItem.call", L"Material List: stopped because the pick item call failed.",
                         "Material List pick item call failed.");
    }
    return result;
}
}
