#include "input/MouseCapture.h"

#include "core/Logger.h"
#include "hooks/HookManager.h"
#include "memory/Patch.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <atomic>
#include <iterator>
#include <mutex>

namespace tsukuyomi::input {

namespace {

using ClipCursorFn = BOOL(WINAPI*)(const RECT*);

ClipCursorFn g_clipCursor = nullptr;

std::atomic<bool> g_freeCursor{false};

std::mutex g_clipMutex;
bool g_clipRequested = false;
bool g_clipHasRect = false;
RECT g_clipRect{};

Patch g_buttonsPatch;
Patch g_deltaXPatch;
Patch g_deltaYPatch;

constexpr ptrdiff_t kButtonsOffset = 0x7D;
constexpr ptrdiff_t kDeltaXOffset = 0x89;
constexpr ptrdiff_t kDeltaYOffset = 0x9D;

const std::byte kZeroEdi[] = {std::byte{0x31}, std::byte{0xFF}, std::byte{0x90},
                              std::byte{0x90}, std::byte{0x90}, std::byte{0x90}};
const std::byte kZeroEax[] = {std::byte{0x31}, std::byte{0xC0}, std::byte{0x90},
                              std::byte{0x90}};

bool setPatches(bool on)
{
    Patch* const patches[] = {&g_buttonsPatch, &g_deltaXPatch, &g_deltaYPatch};
    bool ok = true;
    for (Patch* const patch : patches) {
        if (patch->valid() && !patch->setEnabled(on)) {
            ok = false;
        }
    }
    return ok;
}

BOOL WINAPI detourClipCursor(const RECT* rect)
{
    {
        const std::lock_guard<std::mutex> lock(g_clipMutex);
        g_clipRequested = true;
        g_clipHasRect = (rect != nullptr);
        if (rect != nullptr) {
            g_clipRect = *rect;
        }
    }

    if (g_freeCursor.load(std::memory_order_acquire)) {

        g_clipCursor(nullptr);
        return TRUE;
    }

    return g_clipCursor(rect);
}

}

bool installMouseHooks()
{
    HMODULE const user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) {
        log().error(L"MouseCapture: user32 is not loaded");
        return false;
    }

    if (std::byte* const site = Scanner::instance().address(Target::MouseReading);
        site != nullptr) {
        g_buttonsPatch = Patch(site + kButtonsOffset,
                               {std::begin(kZeroEdi), std::end(kZeroEdi)});
        g_deltaXPatch = Patch(site + kDeltaXOffset,
                              {std::begin(kZeroEax), std::end(kZeroEax)});
        g_deltaYPatch = Patch(site + kDeltaYOffset,
                              {std::begin(kZeroEax), std::end(kZeroEax)});
    } else {
        log().warn(L"MouseCapture: the mouse reading site was not found; "
                   L"the game will keep reading the mouse while the console is open");
    }

    void* const target = reinterpret_cast<void*>(GetProcAddress(user32, "ClipCursor"));
    if (target == nullptr) {
        log().warn(L"MouseCapture: ClipCursor not found in user32");
        return false;
    }

    return HookManager::instance().create(target, reinterpret_cast<void*>(&detourClipCursor),
                                          reinterpret_cast<void**>(&g_clipCursor), L"ClipCursor");
}

void setMouseCaptured(bool captured)
{
    if (g_freeCursor.exchange(captured, std::memory_order_acq_rel) == captured) {
        return;
    }

    if (captured) {

        if (!setPatches(true)) {
            log().error(L"MouseCapture: failed to apply a mouse input patch");
        }
        if (g_clipCursor != nullptr) {

            g_clipCursor(nullptr);
        }
        return;
    }

    if (g_clipCursor != nullptr) {
        const std::lock_guard<std::mutex> lock(g_clipMutex);
        if (g_clipRequested) {
            if (g_clipHasRect) {
                SetCursorPos(g_clipRect.left, g_clipRect.top);
            }
            g_clipCursor(g_clipHasRect ? &g_clipRect : nullptr);
        }
    }

    if (!setPatches(false)) {
        log().error(L"MouseCapture: failed to restore a mouse input patch");
    }
}

}
