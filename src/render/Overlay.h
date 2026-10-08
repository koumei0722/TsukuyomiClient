#pragma once

#include <Windows.h>

namespace tsukuyomi::render {

bool installOverlayHooks();

void shutdownOverlay();

struct Viewport {
    HWND window = nullptr;
    float width = 0.0f;
    float height = 0.0f;
    bool valid = false;
};
Viewport overlayViewport();

bool sawD3D12();

}
