#pragma once

#include <Windows.h>

#include <memory>

namespace tsukuyomi {
struct ConsoleSnapshot;
}

namespace tsukuyomi::render {

bool installOverlayHooks();

void shutdownOverlay();

void setGameModeSelection(bool visible, int selectedMode);

void setConsoleSnapshot(std::shared_ptr<const ConsoleSnapshot> snapshot);

struct Viewport {
    HWND window = nullptr;
    float width = 0.0f;
    float height = 0.0f;
    float scale = 1.0f;
    bool valid = false;
};
Viewport overlayViewport();

}
