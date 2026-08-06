#pragma once

#include <d2d1_1.h>
#include <dwrite.h>

namespace tsukuyomi::render {

float overlayScale(float height);

struct GameModeStyle {
    IDWriteTextFormat* labelFormat = nullptr;
    IDWriteTextFormat* cellFormat = nullptr;
    IDWriteTextFormat* hintFormat = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
};

void drawGameModeSwitcher(ID2D1DeviceContext* context, const GameModeStyle& style, float width,
                          float height, int selectedMode);

}
