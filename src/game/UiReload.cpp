#include "game/UiReload.h"

#include "core/Logger.h"
#include "hooks/Detours.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>

namespace tsukuyomi::uireload {
namespace {

using ReloadFn = void(__fastcall*)(void* const* reference);

std::atomic<int> g_state{0};

enum class Result { Reloaded, NotReady, Faulted };

Result reloadRaw(ReloadFn fn, void* ci)
{
    __try {
        void* const control = *reinterpret_cast<void**>(static_cast<std::byte*>(ci) + 8);
        if (control == nullptr || *static_cast<unsigned char*>(control) != 1) {
            return Result::NotReady;
        }
        void* const reference[3] = {control, nullptr, ci};
        fn(reference);
        return Result::Reloaded;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return Result::Faulted;
    }
}

}

void request()
{
    int expected = 0;
    g_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
}

void pump()
{
    if (g_state.load(std::memory_order_acquire) != 1) {
        return;
    }
    void* const ci = hooks::gameClientInstance();
    if (ci == nullptr) {
        return;
    }
    const auto fn = Scanner::instance().addressAs<ReloadFn>(Target::ReloadUiDefinitions);
    if (fn == nullptr || !memory::inGameModule(reinterpret_cast<const void*>(fn))) {
        g_state.store(2, std::memory_order_release);
        log().warn(L"UiReload: the UI definition reload was not found; the parts added to the inventory screen "
                   L"appear after F3+T or in the next world");
        return;
    }
    const Result result = reloadRaw(fn, ci);
    if (result == Result::NotReady) {
        return;
    }
    g_state.store(2, std::memory_order_release);
    if (result == Result::Reloaded) {
        log().info(L"UiReload: reloaded the UI definitions once (injected after the screens were built; same as F3+T)");
    } else {
        log().warn(L"UiReload: reloading the UI definitions faulted; the parts added to the inventory screen "
                   L"appear after F3+T or in the next world");
    }
}

}
