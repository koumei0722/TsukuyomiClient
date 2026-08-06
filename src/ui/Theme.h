#pragma once

#include <cstddef>
#include <cstdint>

namespace tsukuyomi::theme {

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

constexpr Color rgb(uint32_t value, float alpha = 1.0f)
{
    return Color{
        static_cast<float>((value >> 16) & 0xFF) / 255.0f,
        static_cast<float>((value >> 8) & 0xFF) / 255.0f,
        static_cast<float>(value & 0xFF) / 255.0f,
        alpha,
    };
}

inline constexpr Color kWindowBackground = rgb(0x14161D);
inline constexpr Color kHeaderBackground = rgb(0x1B1E28);
inline constexpr Color kLogBackground    = rgb(0x0F1116);
inline constexpr Color kSelectionFill    = rgb(0x5B6CFF, 0.20f);
inline constexpr Color kSelectionStroke  = rgb(0x6C7BFF, 0.85f);
inline constexpr Color kAccent           = rgb(0x6C7BFF);
inline constexpr Color kText             = rgb(0xE4E7F0);
inline constexpr Color kTextDim          = rgb(0x8A90A6);
inline constexpr Color kTextFaint        = rgb(0x565C72);
inline constexpr Color kDivider          = rgb(0x272B38);
inline constexpr Color kBadgeBackground  = rgb(0x252936);
inline constexpr Color kOn               = rgb(0x3DD68C);
inline constexpr Color kOff              = rgb(0x596074);
inline constexpr Color kWarning          = rgb(0xFFB454);
inline constexpr Color kError            = rgb(0xFF6B6B);
inline constexpr Color kSuccess          = rgb(0x3DD68C);
inline constexpr Color kCloseHover       = rgb(0xE0424F);

inline constexpr float kWindowWidth  = 560.0f;
inline constexpr float kPadding      = 18.0f;
inline constexpr float kHeaderHeight = 56.0f;
inline constexpr float kRowHeight    = 38.0f;
inline constexpr float kRowGap       = 4.0f;
inline constexpr float kRowRadius    = 8.0f;
inline constexpr float kBadgeRadius  = 7.0f;
inline constexpr float kLogHeight    = 132.0f;
inline constexpr int   kVisibleRows = 12;

inline constexpr float kLogLineHeight  = 17.0f;
inline constexpr float kLogAreaPadding = 12.0f;

inline constexpr size_t logCapacity()
{
    const float lines = (kLogHeight - kLogAreaPadding) / kLogLineHeight;
    return static_cast<size_t>(lines > 1.0f ? lines : 1.0f);
}

inline constexpr float kItemAreaPadding = 10.0f;
inline constexpr float kWindowHeightNoLog = kHeaderHeight + kItemAreaPadding + kRowHeight
                                            + (kRowHeight + kRowGap) * (kVisibleRows - 1)
                                            + kItemAreaPadding;

inline constexpr float kLogAreaSlack = 38.0f;
inline constexpr float kWindowHeight = kWindowHeightNoLog + kLogHeight + kLogAreaSlack;

inline constexpr float kCloseButtonSize   = 26.0f;
inline constexpr float kCloseButtonInset  = 10.0f;
inline constexpr float kCloseButtonRadius = 6.0f;
inline constexpr float kCloseMarkInset    = 8.0f;
inline constexpr float kCloseMarkWidth    = 1.6f;

struct RectF {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

inline constexpr RectF closeButtonRect(float panelWidth)
{
    const float right = panelWidth - kCloseButtonInset;
    return RectF{right - kCloseButtonSize, kCloseButtonInset, right,
                 kCloseButtonInset + kCloseButtonSize};
}

inline constexpr bool hitsCloseButton(float panelWidth, float x, float y)
{
    const RectF box = closeButtonRect(panelWidth);
    return x >= box.left && x < box.right && y >= box.top && y < box.bottom;
}

inline constexpr const wchar_t* kUiFont  = L"Segoe UI";
inline constexpr const wchar_t* kLogFont = L"Consolas";
inline constexpr float kTitleSize  = 16.0f;
inline constexpr float kSubtitleSize = 11.5f;
inline constexpr float kItemSize   = 14.5f;
inline constexpr float kBadgeSize  = 11.5f;
inline constexpr float kLogSize    = 12.0f;

inline constexpr float kSelectionAnimationMs = 120.0f;

inline constexpr float kOverlayAlpha = 250.0f / 255.0f;

inline constexpr float kPanelRadius = 10.0f;

inline constexpr Color kCursorFill    = rgb(0xFFFFFF);
inline constexpr Color kCursorOutline = rgb(0x14161D, 0.85f);
inline constexpr float kCursorOutlineWidth = 1.2f;

struct PointF {
    float x = 0.0f;
    float y = 0.0f;
};
inline constexpr PointF kCursorShape[] = {
    {0.0f, 0.0f},   {0.0f, 16.8f}, {4.0f, 13.0f}, {6.6f, 19.0f},
    {9.4f, 17.8f},  {6.8f, 12.0f}, {11.6f, 12.0f},
};

}
