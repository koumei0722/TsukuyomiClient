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
using SetActionBar = void(__fastcall*)(void*, const StdString*, const OptionalString*);
bool g_failed = false;
bool g_reported = false;
bool g_actionBarFailed = false;
bool g_actionBarReported = false;

struct Request {
    Display display = nullptr;
    SetActionBar setActionBar = nullptr;
    const StdString* message = nullptr;
    const OptionalString* filtered = nullptr;
    const StdString* extra = nullptr;
};

void invoke(void* gui, const Request& request)
{
    if (request.setActionBar != nullptr) {
        request.setActionBar(gui, request.message, request.filtered);
    } else {
        request.display(gui, request.message, request.filtered, request.extra, 4);
    }
}

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

bool callGuarded(void* client, std::ptrdiff_t slot, const Request& request, bool& failed)
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
            invoke(handle.gui, request);
            shown = true;
        }
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        failed = true;
        shown = false;
    }
    if (got && !releaseGuarded(handle)) {
        failed = true;
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

std::int32_t guiDataSlot(void*& client)
{
    const auto* site = Scanner::instance().address(Target::ClientGetGuiDataSite);
    client = hooks::gameClientInstance();
    if (site == nullptr || client == nullptr || !memory::isReadable(site + 10, 4)
        || !memory::isReadable(client, sizeof(void*))) return 0;
    std::int32_t slot = 0;
    std::memcpy(&slot, site + 10, sizeof(slot));
    if (slot < 0x100 || slot >= 0x2000 || slot % 8 != 0) return 0;
    return slot;
}
}

bool printLocal(const std::string& utf8)
{
    if (g_failed) return false;
    const auto display = Scanner::instance().addressAs<Display>(Target::GuiDataDisplayClientMessage);
    void* client = nullptr;
    const std::int32_t slot = guiDataSlot(client);
    if (display == nullptr || slot == 0) return false;
    const StdString message = wrap(utf8);
    const OptionalString filtered{};
    const StdString extra{};
    Request request;
    request.display = display;
    request.message = &message;
    request.filtered = &filtered;
    request.extra = &extra;
    const bool shown = callGuarded(client, slot, request, g_failed);
    if (g_failed && !g_reported) {
        g_reported = true;
        log().warn(L"ClientChat: displaying a local message faulted; further messages are disabled");
    }
    return shown;
}

bool showActionBar(const std::string& utf8)
{
    if (g_actionBarFailed) return false;
    const auto setActionBar = Scanner::instance().addressAs<SetActionBar>(Target::GuiDataSetActionBar);
    void* client = nullptr;
    const std::int32_t slot = guiDataSlot(client);
    if (setActionBar == nullptr || slot == 0) return false;
    const StdString message = wrap(utf8);
    const OptionalString filtered{};
    Request request;
    request.setActionBar = setActionBar;
    request.message = &message;
    request.filtered = &filtered;
    const bool shown = callGuarded(client, slot, request, g_actionBarFailed);
    if (g_actionBarFailed && !g_actionBarReported) {
        g_actionBarReported = true;
        log().warn(L"ClientChat: showing an actionbar message faulted; further actionbar messages are disabled");
    }
    return shown;
}
}
