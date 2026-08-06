#include "render/GameModeOverlay.h"

#include "game/GameModeIds.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cwchar>

namespace tsukuyomi::render {

namespace {

D2D1_COLOR_F toColorF(const theme::Color& color)
{
    return D2D1::ColorF(color.r, color.g, color.b, color.a);
}

void drawCreativeIcon(ID2D1DeviceContext* context, ID2D1SolidColorBrush* brush, float centerX,
                      float centerY, float size)
{

    const float unit = size * 0.40f;
    const float gap = size * 0.14f;
    const float origin = -(unit + gap * 0.5f);

    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
            const float left = centerX + origin + static_cast<float>(column) * (unit + gap);
            const float top = centerY + origin + static_cast<float>(row) * (unit + gap);
            context->FillRectangle(D2D1::RectF(left, top, left + unit, top + unit), brush);
        }
    }
}

void drawSurvivalIcon(ID2D1DeviceContext* context, ID2D1SolidColorBrush* brush, float centerX,
                      float centerY, float size)
{

    const float half = size * 0.42f;
    const D2D1_POINT_2F center = D2D1::Point2F(centerX, centerY);

    context->SetTransform(D2D1::Matrix3x2F::Rotation(45.0f, center));

    const D2D1_RECT_F square =
        D2D1::RectF(centerX - half, centerY - half, centerX + half, centerY + half);
    context->FillRectangle(square, brush);
    context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(centerX, centerY - half), half, half), brush);
    context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(centerX - half, centerY), half, half), brush);

    context->SetTransform(D2D1::Matrix3x2F::Identity());
}

void drawAdventureIcon(ID2D1DeviceContext* context, ID2D1SolidColorBrush* brush, float centerX,
                       float centerY, float size)
{

    const float radius = size * 0.52f;

    const float stroke = std::max(2.0f, size * 0.16f);
    const D2D1_POINT_2F center = D2D1::Point2F(centerX, centerY);

    context->DrawEllipse(D2D1::Ellipse(center, radius, radius), brush, stroke);

    const float needleLength = radius * 0.70f;
    const float needleHalfWidth = std::max(1.2f, size * 0.10f);

    context->SetTransform(D2D1::Matrix3x2F::Rotation(-35.0f, center));
    context->FillRectangle(D2D1::RectF(centerX - needleHalfWidth, centerY - needleLength,
                                       centerX + needleHalfWidth, centerY),
                           brush);
    context->SetTransform(D2D1::Matrix3x2F::Identity());

    context->FillEllipse(D2D1::Ellipse(center, size * 0.13f, size * 0.13f), brush);
}

void drawSpectatorIcon(ID2D1DeviceContext* context, ID2D1SolidColorBrush* brush, float centerX,
                       float centerY, float size)
{

    const float stroke = std::max(2.0f, size * 0.16f);
    const D2D1_POINT_2F center = D2D1::Point2F(centerX, centerY);

    context->DrawEllipse(D2D1::Ellipse(center, size * 0.60f, size * 0.36f), brush, stroke);
    context->FillEllipse(D2D1::Ellipse(center, size * 0.20f, size * 0.20f), brush);
}

void drawIcon(ID2D1DeviceContext* context, ID2D1SolidColorBrush* brush, int mode, float centerX,
              float centerY, float size)
{
    switch (mode) {
    case gamemode::kCreative:
        drawCreativeIcon(context, brush, centerX, centerY, size);
        break;
    case gamemode::kSurvival:
        drawSurvivalIcon(context, brush, centerX, centerY, size);
        break;
    case gamemode::kAdventure:
        drawAdventureIcon(context, brush, centerX, centerY, size);
        break;
    case gamemode::kSpectator:
        drawSpectatorIcon(context, brush, centerX, centerY, size);
        break;
    default:
        break;
    }
}

void drawCenteredText(ID2D1DeviceContext* context, IDWriteTextFormat* format, const wchar_t* text,
                      const D2D1_RECT_F& area, ID2D1SolidColorBrush* brush)
{
    if (format == nullptr || text == nullptr || text[0] == L'\0') {
        return;
    }
    context->DrawTextW(text, static_cast<UINT32>(wcslen(text)), format, area, brush);
}

}

float overlayScale(float height)
{
    return std::max(0.80f, height / 1080.0f);
}

void drawGameModeSwitcher(ID2D1DeviceContext* context, const GameModeStyle& style, float width,
                          float height, int selectedMode)
{
    if (context == nullptr || style.brush == nullptr) {
        return;
    }

    ID2D1SolidColorBrush* const brush = style.brush;
    const float scale = overlayScale(height);

    const float cellWidth = 84.0f * scale;
    const float cellHeight = 78.0f * scale;
    const float cellNameHeight = 20.0f * scale;
    const float gap = 10.0f * scale;
    const float padding = 14.0f * scale;
    const float innerGap = 8.0f * scale;
    const float labelHeight = 34.0f * scale;
    const float hintHeight = 22.0f * scale;
    const float count = static_cast<float>(gamemode::kCycleCount);

    const float barWidth = cellWidth * count + gap * (count - 1.0f) + padding * 2.0f;
    const float barHeight =
        padding * 2.0f + labelHeight + innerGap + cellHeight + innerGap + hintHeight;
    const float barLeft = (width - barWidth) * 0.5f;

    const float barBottom = height * 0.44f;
    const D2D1_RECT_F bar =
        D2D1::RectF(barLeft, barBottom - barHeight, barLeft + barWidth, barBottom);

    const D2D1_ROUNDED_RECT barShape = D2D1::RoundedRect(bar, 10.0f * scale, 10.0f * scale);
    brush->SetColor(D2D1::ColorF(0.04f, 0.05f, 0.07f, 0.90f));
    context->FillRoundedRectangle(barShape, brush);
    brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f));
    context->DrawRoundedRectangle(barShape, brush, 1.4f * scale);

    const float labelTop = bar.top + padding;
    brush->SetColor(toColorF(theme::kText));
    drawCenteredText(context, style.labelFormat, gamemode::displayName(selectedMode),
                     D2D1::RectF(bar.left, labelTop, bar.right, labelTop + labelHeight), brush);

    const float cellTop = labelTop + labelHeight + innerGap;
    for (int index = 0; index < gamemode::kCycleCount; ++index) {
        const int mode = gamemode::kCycle[index];
        const bool active = (mode == selectedMode);

        const float left = bar.left + padding + static_cast<float>(index) * (cellWidth + gap);
        const D2D1_RECT_F box =
            D2D1::RectF(left, cellTop, left + cellWidth, cellTop + cellHeight);
        const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(box, 8.0f * scale, 8.0f * scale);

        brush->SetColor(D2D1::ColorF(0.11f, 0.12f, 0.16f, 0.92f));
        context->FillRoundedRectangle(rounded, brush);

        if (active) {
            brush->SetColor(toColorF(theme::kSelectionFill));
            context->FillRoundedRectangle(rounded, brush);
            brush->SetColor(toColorF(theme::kSelectionStroke));
            context->DrawRoundedRectangle(rounded, brush, 2.0f * scale);
        }

        brush->SetColor(active ? toColorF(theme::kText) : toColorF(theme::kTextDim));

        const float iconCenterY = box.top + (cellHeight - cellNameHeight) * 0.5f;
        drawIcon(context, brush, mode, (box.left + box.right) * 0.5f, iconCenterY,
                 cellWidth * 0.26f);

        drawCenteredText(context, style.cellFormat, gamemode::shortName(mode),
                         D2D1::RectF(box.left, box.bottom - cellNameHeight, box.right, box.bottom),
                         brush);
    }

    const float hintTop = cellTop + cellHeight + innerGap;
    brush->SetColor(toColorF(theme::kTextDim));
    drawCenteredText(context, style.hintFormat, L"Tap again to cycle, release to apply",
                     D2D1::RectF(bar.left, hintTop, bar.right, hintTop + hintHeight), brush);
}

}
