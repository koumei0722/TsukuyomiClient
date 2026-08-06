#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstddef>
#include <string>
#include <vector>

#include "ui/ConsoleSnapshot.h"
#include "ui/Theme.h"

namespace tsukuyomi {

class Renderer {
public:

    bool createResources(ID2D1RenderTarget* target, IDWriteFactory* writeFactory,
                         ID2D1SolidColorBrush* brush);
    void releaseResources();

    bool ready() const { return m_target != nullptr && m_brush != nullptr; }

    void render(const ConsoleSnapshot& snapshot, float scale);

    static constexpr float panelWidth() { return theme::kWindowWidth; }
    static constexpr float panelHeight(bool showLog)
    {
        return showLog ? theme::kWindowHeight : theme::kWindowHeightNoLog;
    }

private:
    void fill(const D2D1_RECT_F& rect, const theme::Color& color);
    void fillRounded(const D2D1_RECT_F& rect, float radius, const theme::Color& color);

    void fillTopRounded(const D2D1_RECT_F& rect, float radius, const theme::Color& color);
    void fillBottomRounded(const D2D1_RECT_F& rect, float radius, const theme::Color& color);

    void strokeRounded(const D2D1_RECT_F& rect, float radius, const theme::Color& color,
                       float width);
    void drawLine(const D2D1_POINT_2F& from, const D2D1_POINT_2F& to, const theme::Color& color,
                  float width);

    void drawText(std::wstring_view text, IDWriteTextFormat* format, const D2D1_RECT_F& rect,
                  const theme::Color& color);
    float measureText(std::wstring_view text, IDWriteTextFormat* format) const;

    void drawHeader(const ConsoleSnapshot& snapshot, const D2D1_RECT_F& area);
    void drawCloseButton(const D2D1_RECT_F& area, bool hovered);
    void drawItems(const ConsoleSnapshot& snapshot, const D2D1_RECT_F& area);
    void drawInputPrompt(const ConsoleSnapshot& snapshot, const D2D1_RECT_F& area);
    void drawLogs(const std::vector<LogEntry>& logs, size_t scroll, const D2D1_RECT_F& area);
    void drawBadge(std::wstring_view text, float rightEdge, float centerY,
                   const theme::Color& textColor, const theme::Color& backColor);

    void drawCursor(float x, float y, float scale);

    bool createCursorShape();

    ID2D1RenderTarget* m_target = nullptr;
    IDWriteFactory* m_writeFactory = nullptr;
    ID2D1SolidColorBrush* m_brush = nullptr;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_titleFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_subtitleFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_itemFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_itemRightFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_badgeFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_logFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_promptFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_promptLabelFormat;

    Microsoft::WRL::ComPtr<ID2D1PathGeometry> m_cursorShape;
};

}
