#pragma once

#include <Windows.h>

namespace tsukuyomi::render {

bool installOverlayHooks();

void shutdownOverlay();

void setGameModeSelection(bool visible, int selectedMode);

struct Viewport {
    HWND window = nullptr;
    float width = 0.0f;
    float height = 0.0f;
    float scale = 1.0f;
    bool valid = false;
};
Viewport overlayViewport();

}
