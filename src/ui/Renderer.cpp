#include "ui/Renderer.h"

#include "core/Logger.h"
#include "core/Version.h"

#include <algorithm>
#include <format>
#include <iterator>

namespace tsukuyomi {

namespace {

D2D1_COLOR_F toD2D(const theme::Color& color)
{
    return D2D1::ColorF(color.r, color.g, color.b, color.a);
}

theme::Color panelColor(const theme::Color& base)
{
    theme::Color color = base;
    color.a = theme::kOverlayAlpha;
    return color;
}

const theme::Color& logColor(LogLevel level)
{
    switch (level) {
    case LogLevel::Success: return theme::kSuccess;
    case LogLevel::Warning: return theme::kWarning;
    case LogLevel::Error:   return theme::kError;
    case LogLevel::Info:
    default:                return theme::kTextDim;
    }
}

}

bool Renderer::createResources(ID2D1RenderTarget* target, IDWriteFactory* writeFactory,
                               ID2D1SolidColorBrush* brush)
{
    if (target == nullptr || writeFactory == nullptr || brush == nullptr) {
        log().error(L"Renderer: createResources was given a null target, factory or brush");
        return false;
    }

    m_target = target;
    m_writeFactory = writeFactory;
    m_brush = brush;

    struct FormatSpec {
        Microsoft::WRL::ComPtr<IDWriteTextFormat>* slot;
        const wchar_t* family;
        DWRITE_FONT_WEIGHT weight;
        float size;
        DWRITE_TEXT_ALIGNMENT alignment;
    };

    const FormatSpec specs[] = {
        {&m_titleFormat, theme::kUiFont, DWRITE_FONT_WEIGHT_SEMI_BOLD, theme::kTitleSize,
         DWRITE_TEXT_ALIGNMENT_LEADING},
        {&m_subtitleFormat, theme::kUiFont, DWRITE_FONT_WEIGHT_NORMAL, theme::kSubtitleSize,
         DWRITE_TEXT_ALIGNMENT_LEADING},
        {&m_itemFormat, theme::kUiFont, DWRITE_FONT_WEIGHT_NORMAL, theme::kItemSize,
         DWRITE_TEXT_ALIGNMENT_LEADING},
        {&m_itemRightFormat, theme::kUiFont, DWRITE_FONT_WEIGHT_NORMAL, theme::kItemSize,
         DWRITE_TEXT_ALIGNMENT_TRAILING},
        {&m_badgeFormat, theme::kUiFont, DWRITE_FONT_WEIGHT_SEMI_BOLD, theme::kBadgeSize,
         DWRITE_TEXT_ALIGNMENT_CENTER},
        {&m_logFormat, theme::kLogFont, DWRITE_FONT_WEIGHT_NORMAL, theme::kLogSize,
         DWRITE_TEXT_ALIGNMENT_LEADING},
        {&m_promptFormat, theme::kUiFont, DWRITE_FONT_WEIGHT_SEMI_BOLD, 20.0f,
         DWRITE_TEXT_ALIGNMENT_CENTER},
        {&m_promptLabelFormat, theme::kUiFont, DWRITE_FONT_WEIGHT_NORMAL, theme::kItemSize,
         DWRITE_TEXT_ALIGNMENT_CENTER},
    };

    for (const FormatSpec& spec : specs) {
        HRESULT hr = m_writeFactory->CreateTextFormat(spec.family, nullptr, spec.weight,
                                                      DWRITE_FONT_STYLE_NORMAL,
                                                      DWRITE_FONT_STRETCH_NORMAL, spec.size, L"",
                                                      spec.slot->ReleaseAndGetAddressOf());
        if (FAILED(hr)) {

            hr = m_writeFactory->CreateTextFormat(L"Segoe UI", nullptr, spec.weight,
                                                  DWRITE_FONT_STYLE_NORMAL,
                                                  DWRITE_FONT_STRETCH_NORMAL, spec.size, L"",
                                                  spec.slot->ReleaseAndGetAddressOf());
        }
        if (FAILED(hr)) {
            log().error(L"Could not create the text formats (0x{:08X})", static_cast<uint32_t>(hr));
            releaseResources();
            return false;
        }
        (*spec.slot)->SetTextAlignment(spec.alignment);
        (*spec.slot)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        (*spec.slot)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    m_logFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    createCursorShape();

    return true;
}

bool Renderer::createCursorShape()
{
    m_cursorShape.Reset();

    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    m_target->GetFactory(&factory);
    if (!factory) {
        return false;
    }

    if (FAILED(factory->CreatePathGeometry(&m_cursorShape))) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(m_cursorShape->Open(&sink))) {
        m_cursorShape.Reset();
        return false;
    }

    constexpr size_t count = std::size(theme::kCursorShape);
    sink->BeginFigure(D2D1::Point2F(theme::kCursorShape[0].x, theme::kCursorShape[0].y),
                      D2D1_FIGURE_BEGIN_FILLED);
    for (size_t index = 1; index < count; ++index) {
        sink->AddLine(D2D1::Point2F(theme::kCursorShape[index].x, theme::kCursorShape[index].y));
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);

    if (FAILED(sink->Close())) {
        m_cursorShape.Reset();
        return false;
    }

    return true;
}

void Renderer::drawCursor(float x, float y, float scale)
{
    if (!m_cursorShape) {
        return;
    }

    m_target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale)
                           * D2D1::Matrix3x2F::Translation(x, y));

    m_brush->SetColor(toD2D(theme::kCursorFill));
    m_target->FillGeometry(m_cursorShape.Get(), m_brush);

    m_brush->SetColor(toD2D(theme::kCursorOutline));
    m_target->DrawGeometry(m_cursorShape.Get(), m_brush, theme::kCursorOutlineWidth);

    m_target->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Renderer::releaseResources()
{
    m_cursorShape.Reset();
    m_promptLabelFormat.Reset();
    m_promptFormat.Reset();
    m_logFormat.Reset();
    m_badgeFormat.Reset();
    m_itemRightFormat.Reset();
    m_itemFormat.Reset();
    m_subtitleFormat.Reset();
    m_titleFormat.Reset();

    m_brush = nullptr;
    m_writeFactory = nullptr;
    m_target = nullptr;
}

void Renderer::fill(const D2D1_RECT_F& rect, const theme::Color& color)
{
    m_brush->SetColor(toD2D(color));
    m_target->FillRectangle(rect, m_brush);
}

void Renderer::fillRounded(const D2D1_RECT_F& rect, float radius, const theme::Color& color)
{
    m_brush->SetColor(toD2D(color));
    m_target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), m_brush);
}

void Renderer::fillTopRounded(const D2D1_RECT_F& rect, float radius, const theme::Color& color)
{
    if (radius <= 0.0f) {
        fill(rect, color);
        return;
    }

    fillRounded(rect, radius, color);
    fill(D2D1_RECT_F{rect.left, rect.top + radius, rect.right, rect.bottom}, color);
}

void Renderer::fillBottomRounded(const D2D1_RECT_F& rect, float radius, const theme::Color& color)
{
    if (radius <= 0.0f) {
        fill(rect, color);
        return;
    }
    fillRounded(rect, radius, color);
    fill(D2D1_RECT_F{rect.left, rect.top, rect.right, rect.bottom - radius}, color);
}

void Renderer::strokeRounded(const D2D1_RECT_F& rect, float radius, const theme::Color& color,
                             float width)
{
    m_brush->SetColor(toD2D(color));
    m_target->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), m_brush, width);
}

void Renderer::drawLine(const D2D1_POINT_2F& from, const D2D1_POINT_2F& to,
                        const theme::Color& color, float width)
{
    m_brush->SetColor(toD2D(color));
    m_target->DrawLine(from, to, m_brush, width);
}

void Renderer::drawText(std::wstring_view text, IDWriteTextFormat* format,
                        const D2D1_RECT_F& rect, const theme::Color& color)
{
    if (text.empty() || format == nullptr) {
        return;
    }
    m_brush->SetColor(toD2D(color));
    m_target->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format, rect,
                        m_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Renderer::measureText(std::wstring_view text, IDWriteTextFormat* format) const
{
    if (text.empty() || format == nullptr || m_writeFactory == nullptr) {
        return 0.0f;
    }

    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    const HRESULT hr = m_writeFactory->CreateTextLayout(text.data(),
                                                        static_cast<UINT32>(text.size()), format,
                                                        4096.0f, 128.0f, layout.GetAddressOf());
    if (FAILED(hr)) {
        return 0.0f;
    }

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) {
        return 0.0f;
    }
    return metrics.width;
}

void Renderer::drawBadge(std::wstring_view text, float rightEdge, float centerY,
                         const theme::Color& textColor, const theme::Color& backColor)
{
    if (text.empty()) {
        return;
    }

    constexpr float paddingX = 10.0f;
    constexpr float height = 21.0f;

    const float width = measureText(text, m_badgeFormat.Get()) + paddingX * 2.0f;
    const D2D1_RECT_F box{rightEdge - width, centerY - height * 0.5f, rightEdge,
                          centerY + height * 0.5f};

    fillRounded(box, theme::kBadgeRadius, backColor);
    drawText(text, m_badgeFormat.Get(), box, textColor);
}

void Renderer::drawCloseButton(const D2D1_RECT_F& area, bool hovered)
{
    const theme::RectF box = theme::closeButtonRect(area.right - area.left);
    const D2D1_RECT_F rect{area.left + box.left, area.top + box.top, area.left + box.right,
                           area.top + box.bottom};

    if (hovered) {
        fillRounded(rect, theme::kCloseButtonRadius, theme::kCloseHover);
    }

    const float inset = theme::kCloseMarkInset;
    const theme::Color mark = hovered ? theme::kText : theme::kTextDim;
    drawLine(D2D1::Point2F(rect.left + inset, rect.top + inset),
             D2D1::Point2F(rect.right - inset, rect.bottom - inset), mark, theme::kCloseMarkWidth);
    drawLine(D2D1::Point2F(rect.right - inset, rect.top + inset),
             D2D1::Point2F(rect.left + inset, rect.bottom - inset), mark, theme::kCloseMarkWidth);
}

void Renderer::drawHeader(const ConsoleSnapshot& snapshot, const D2D1_RECT_F& area)
{

    fillTopRounded(area, theme::kPanelRadius, panelColor(theme::kHeaderBackground));

    const D2D1_RECT_F titleRect{area.left + theme::kPadding, area.top + 8.0f,
                                area.right - theme::kPadding - 90.0f, area.top + 32.0f};
    drawText(snapshot.title, m_titleFormat.Get(), titleRect, theme::kText);

    std::wstring subtitle;
    switch (snapshot.mode) {
    case Menu::Mode::AwaitingKeybind:
        subtitle = L"Waiting for a key  -  ESC to clear";
        break;
    case Menu::Mode::NumberEntry:
        subtitle = L"Type a number  -  Enter to confirm / ESC to cancel";
        break;
    case Menu::Mode::Normal:
    default:

        subtitle = snapshot.atRoot
                       ? std::wstring(L"v" TSUKUYOMI_VERSION_W
                                      L"  -  W/S select   Shift+Space toggle   "
                                      L"Space confirm   ESC close")
                       : std::wstring(L".. to go back  -  W/S select   Shift+Space toggle   "
                                      L"Space confirm   ESC back");
        break;
    }

    const D2D1_RECT_F subtitleRect{area.left + theme::kPadding, area.top + 30.0f,
                                   area.right - theme::kPadding - theme::kCloseButtonSize,
                                   area.top + 50.0f};
    drawText(subtitle, m_subtitleFormat.Get(), subtitleRect, theme::kTextFaint);

    drawCloseButton(area, snapshot.closeHovered);

    const D2D1_RECT_F divider{area.left, area.bottom - 1.0f, area.right, area.bottom};
    fill(divider, theme::kDivider);
}

void Renderer::drawItems(const ConsoleSnapshot& snapshot, const D2D1_RECT_F& area)
{
    const float rowStride = theme::kRowHeight + theme::kRowGap;
    const float startY = area.top + 10.0f;

    if (snapshot.rows.empty()) {
        const D2D1_RECT_F empty{area.left, area.top, area.right, area.bottom};
        drawText(L"Nothing to show", m_itemFormat.Get(), empty, theme::kTextFaint);
        return;
    }

    if (snapshot.selectionVisible) {
        const float y = startY + snapshot.selectionRow * rowStride;
        const D2D1_RECT_F bar{area.left + theme::kPadding - 8.0f, y,
                              area.right - theme::kPadding + 8.0f, y + theme::kRowHeight};
        fillRounded(bar, theme::kRowRadius, theme::kSelectionFill);
        strokeRounded(bar, theme::kRowRadius, theme::kSelectionStroke, 1.0f);
    }

    float y = startY;
    for (size_t index = 0; index < snapshot.rows.size(); ++index) {
        const MenuRow& row = snapshot.rows[index];
        const bool selected = (index == snapshot.selectedRow);

        const D2D1_RECT_F labelRect{area.left + theme::kPadding + 4.0f, y,
                                    area.right - theme::kPadding - 120.0f, y + theme::kRowHeight};

        theme::Color labelColor = theme::kTextDim;
        if (!row.available) {
            labelColor = theme::kTextFaint;
        } else if (selected) {
            labelColor = theme::kText;
        }
        drawText(row.label, m_itemFormat.Get(), labelRect, labelColor);

        const float centerY = y + theme::kRowHeight * 0.5f;
        const float rightEdge = area.right - theme::kPadding;

        if (!row.available) {
            drawBadge(L"unavailable", rightEdge, centerY, theme::kTextFaint, theme::kBadgeBackground);
        } else {
            switch (row.kind) {
            case MenuItemKind::Toggle:
                drawBadge(row.on ? L"ON" : L"OFF", rightEdge, centerY,
                          row.on ? theme::kOn : theme::kOff, theme::kBadgeBackground);
                break;

            case MenuItemKind::Submenu:

                if (row.hasState) {
                    drawBadge(row.on ? L"ON" : L"OFF", rightEdge, centerY,
                              row.on ? theme::kOn : theme::kOff, theme::kBadgeBackground);
                } else {
                    const D2D1_RECT_F arrow{rightEdge - 20.0f, y, rightEdge,
                                            y + theme::kRowHeight};
                    drawText(L"›", m_itemRightFormat.Get(), arrow, theme::kTextFaint);
                }
                break;

            case MenuItemKind::Cycle:
            case MenuItemKind::Number:
            case MenuItemKind::Keybind:
                if (!row.value.empty()) {
                    drawBadge(row.value, rightEdge, centerY, theme::kAccent,
                              theme::kBadgeBackground);
                }
                break;

            case MenuItemKind::Back:
            case MenuItemKind::Action:
            default:
                break;
            }
        }

        y += rowStride;
    }
}

void Renderer::drawInputPrompt(const ConsoleSnapshot& snapshot, const D2D1_RECT_F& area)
{
    const D2D1_RECT_F titleRect{area.left, area.top + 34.0f, area.right, area.top + 62.0f};
    drawText(snapshot.promptLabel, m_promptLabelFormat.Get(), titleRect, theme::kTextDim);

    const float contentWidth = measureText(snapshot.promptBody, m_promptFormat.Get()) + 56.0f;
    const float width = (std::max)(contentWidth, 200.0f);
    const float centerX = (area.left + area.right) * 0.5f;

    const D2D1_RECT_F box{centerX - width * 0.5f, area.top + 72.0f, centerX + width * 0.5f,
                          area.top + 120.0f};
    fillRounded(box, theme::kRowRadius, theme::kBadgeBackground);
    strokeRounded(box, theme::kRowRadius, theme::kSelectionStroke, 1.0f);
    drawText(snapshot.promptBody, m_promptFormat.Get(), box, theme::kAccent);

    const D2D1_RECT_F hintRect{area.left, area.top + 132.0f, area.right, area.top + 156.0f};
    const std::wstring hint = (snapshot.mode == Menu::Mode::AwaitingKeybind)
                                  ? std::wstring(L"Press the key to assign / ESC for none")
                                  : std::wstring(L"Enter to confirm / ESC to cancel");
    drawText(hint, m_promptLabelFormat.Get(), hintRect, theme::kTextFaint);
}

void Renderer::drawLogs(const std::vector<LogEntry>& logs, size_t scroll, const D2D1_RECT_F& area)
{

    fillBottomRounded(area, theme::kPanelRadius, panelColor(theme::kLogBackground));

    const D2D1_RECT_F divider{area.left, area.top, area.right, area.top + 1.0f};
    fill(divider, theme::kDivider);

    constexpr float lineHeight = theme::kLogLineHeight;
    constexpr size_t capacity = theme::logCapacity();

    if (logs.empty()) {
        return;
    }

    const size_t maxScroll = (logs.size() > capacity) ? logs.size() - capacity : 0;
    const size_t clamped = (std::min)(scroll, maxScroll);

    const size_t last = logs.size() - clamped;
    const size_t first = (last > capacity) ? last - capacity : 0;

    float y = area.top + 6.0f;
    for (size_t index = first; index < last; ++index) {
        const LogEntry& entry = logs[index];

        const D2D1_RECT_F timeRect{area.left + theme::kPadding, y,
                                   area.left + theme::kPadding + 62.0f, y + lineHeight};
        drawText(entry.timestamp, m_logFormat.Get(), timeRect, theme::kTextFaint);

        const D2D1_RECT_F textRect{area.left + theme::kPadding + 66.0f, y,
                                   area.right - theme::kPadding, y + lineHeight};
        drawText(entry.text, m_logFormat.Get(), textRect, logColor(entry.level));

        y += lineHeight;
    }

    if (clamped > 0) {
        const D2D1_RECT_F notice{area.left, area.bottom - lineHeight - 2.0f,
                                 area.right - theme::kPadding, area.bottom - 2.0f};
        drawText(std::format(L"scrolled back {} lines", clamped), m_logFormat.Get(), notice,
                 theme::kWarning);
    }
}

void Renderer::render(const ConsoleSnapshot& snapshot, float scale)
{
    if (!ready()) {
        return;
    }

    const float width = panelWidth();
    const float height = panelHeight(snapshot.showLog);

    m_target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale)
                           * D2D1::Matrix3x2F::Translation(snapshot.originX, snapshot.originY));

    const D2D1_RECT_F panel{0.0f, 0.0f, width, height};
    fillRounded(panel, theme::kPanelRadius, panelColor(theme::kWindowBackground));

    const D2D1_RECT_F headerArea{0.0f, 0.0f, width, theme::kHeaderHeight};

    const float logTop = snapshot.showLog ? height - theme::kLogHeight : height;
    const D2D1_RECT_F logArea{0.0f, logTop, width, height};
    const D2D1_RECT_F itemArea{0.0f, headerArea.bottom, width, logTop};

    drawHeader(snapshot, headerArea);

    if (snapshot.mode == Menu::Mode::Normal) {
        drawItems(snapshot, itemArea);
    } else {
        drawInputPrompt(snapshot, itemArea);
    }

    if (snapshot.showLog) {
        drawLogs(snapshot.logs, snapshot.logScroll, logArea);
    }

    m_target->SetTransform(D2D1::Matrix3x2F::Identity());

    if (snapshot.cursorValid) {
        drawCursor(snapshot.cursorX, snapshot.cursorY, scale);
    }
}

}
