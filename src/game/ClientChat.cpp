#include "game/ClientChat.h"

#include <Windows.h>
#include <intrin.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/Logger.h"
#include "hooks/Detours.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

namespace tsukuyomi::clientchat {
namespace {
struct StdString {
    union { char inline_[16]; const char* pointer; } data{};
    std::uint64_t length = 0;
    std::uint64_t capacity = 15;
};
static_assert(sizeof(StdString) == 0x20);
struct OptionalString { StdString text; std::uint8_t hasValue = 0; std::uint8_t pad[7]{}; };
static_assert(sizeof(OptionalString) == 0x28);
struct GuiHandle { void* block = nullptr; std::byte* ctrl = nullptr; void* gui = nullptr; };
static_assert(sizeof(GuiHandle) == 0x18);
using GetGui = void*(__fastcall*)(void*, void*);
using Display = void(__fastcall*)(void*, const StdString*, const OptionalString*, const StdString*, std::int32_t);
bool g_failed = false;
bool g_reported = false;

void release(GuiHandle& handle)
{
    if (handle.ctrl == nullptr) return;
    auto* const ctrl = handle.ctrl;
    auto** const vt = *reinterpret_cast<void***>(ctrl);
    if (_InterlockedDecrement(reinterpret_cast<volatile long*>(ctrl + 8)) == 0) {
        reinterpret_cast<void(__fastcall*)(void*)>(vt[0])(ctrl);
        if (_InterlockedDecrement(reinterpret_cast<volatile long*>(ctrl + 0xC)) == 0)
            reinterpret_cast<void(__fastcall*)(void*)>(vt[1])(ctrl);
    }
}

bool releaseGuarded(GuiHandle& handle)
{
    __try {
        release(handle);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool callGuarded(void* client, std::ptrdiff_t slot, Display display, const StdString* message,
                 const OptionalString* filtered, const StdString* extra)
{
    GuiHandle handle{};
    volatile bool shown = false;
    volatile bool got = false;
    __try {
        auto** vt = *reinterpret_cast<void***>(client);
        reinterpret_cast<GetGui>(vt[slot / 8])(client, &handle);
        got = true;
        if (handle.block != nullptr && *static_cast<const std::uint8_t*>(handle.block) != 0
            && handle.gui != nullptr) {
            display(handle.gui, message, filtered, extra, 4);
            shown = true;
        }
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        g_failed = true;
        shown = false;
    }
    if (got && !releaseGuarded(handle)) {
        g_failed = true;
    }
    return shown;
}

StdString wrap(const std::string& value)
{
    StdString result{};
    result.length = value.size();
    if (value.size() < 16) {
        std::memcpy(result.data.inline_, value.c_str(), value.size() + 1);
    } else {
        result.data.pointer = value.c_str();
        result.capacity = value.size();
    }
    return result;
}
}

bool printLocal(const std::string& utf8)
{
    if (g_failed) return false;
    const Scanner& scanner = Scanner::instance();
    const auto* site = scanner.address(Target::ClientGetGuiDataSite);
    const auto display = scanner.addressAs<Display>(Target::GuiDataDisplayClientMessage);
    void* client = hooks::gameClientInstance();
    if (site == nullptr || display == nullptr || client == nullptr || !memory::isReadable(site + 10, 4)
        || !memory::isReadable(client, sizeof(void*))) return false;
    std::int32_t slot = 0;
    std::memcpy(&slot, site + 10, sizeof(slot));
    if (slot < 0x100 || slot >= 0x2000 || slot % 8 != 0) return false;
    const StdString message = wrap(utf8);
    const OptionalString filtered{};
    const StdString extra{};
    const bool shown = callGuarded(client, slot, display, &message, &filtered, &extra);
    if (g_failed && !g_reported) {
        g_reported = true;
        log().warn(L"ClientChat: displaying a local message faulted; further messages are disabled");
    }
    return shown;
}
}
