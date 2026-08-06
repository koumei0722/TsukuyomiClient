#include "ui/Console.h"

#include "core/Logger.h"
#include "input/Capture.h"
#include "input/Foreground.h"
#include "input/Keys.h"
#include "input/MouseCapture.h"
#include "render/Overlay.h"
#include "ui/Renderer.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace tsukuyomi {

namespace {

HMODULE currentModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&currentModule), &module);
    return module;
}

}

Console* Console::s_hookOwner = nullptr;
Console* Console::s_mouseOwner = nullptr;

bool Console::create()
{

    return true;
}

void Console::destroy()
{

    if (m_visible) {
        hide();
    }

    removeKeyHook();
    removeMouseHook();
    input::setConsoleCapturing(false);

    render::setConsoleSnapshot(nullptr);

    m_visible = false;
}

float Console::panelWidth() const
{
    return Renderer::panelWidth();
}

float Console::panelHeight() const
{
    return Renderer::panelHeight(m_showLog);
}

void Console::setOrigin(float x, float y)
{
    m_originX = x;
    m_originY = y;
    m_hasOrigin = true;
}

void Console::clampOrigin()
{
    const render::Viewport view = render::overlayViewport();
    if (!view.valid) {
        return;
    }

    const float width = panelWidth() * view.scale;
    const float height = panelHeight() * view.scale;

    if (!m_hasOrigin) {

        m_originX = (view.width - width) * 0.5f;
        m_originY = (view.height - height) * 0.5f;
        m_hasOrigin = true;
        return;
    }

    const float maxX = view.width - width;
    const float maxY = view.height - height;
    m_originX = (std::max)(0.0f, (std::min)(m_originX, maxX));
    m_originY = (std::max)(0.0f, (std::min)(m_originY, maxY));
}

void Console::setLogVisible(bool visible)
{
    if (m_showLog == visible) {
        return;
    }
    m_showLog = visible;

    if (!visible) {
        m_logScroll = 0;
    }

    clampOrigin();
}

void Console::installKeyHook()
{
    if (m_keyHook != nullptr) {
        return;
    }

    s_hookOwner = this;
    m_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, &Console::keyboardHookProc, currentModule(), 0);
    if (m_keyHook == nullptr) {
        s_hookOwner = nullptr;
        log().warn(L"Could not hook the keyboard (error {}). "
                   L"Menu keys will also reach the game",
                   GetLastError());
    }
}

void Console::installMouseHook()
{
    if (m_mouseHook != nullptr) {
        return;
    }

    s_mouseOwner = this;
    m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &Console::mouseHookProc, currentModule(), 0);
    if (m_mouseHook == nullptr) {
        s_mouseOwner = nullptr;
        log().warn(L"Could not hook the mouse (error {}). "
                   L"Clicks and the wheel will not reach the console",
                   GetLastError());
    }
}

void Console::removeMouseHook()
{
    if (m_mouseHook == nullptr) {
        return;
    }

    UnhookWindowsHookEx(m_mouseHook);
    m_mouseHook = nullptr;
    s_mouseOwner = nullptr;

    m_dragging = false;
}

void Console::removeKeyHook()
{
    if (m_keyHook == nullptr) {
        return;
    }

    UnhookWindowsHookEx(m_keyHook);
    m_keyHook = nullptr;
    s_hookOwner = nullptr;

    m_ctrlHeld = false;
    m_shiftHeld = false;
    m_altHeld = false;
}

void Console::setReservedKeys(std::vector<int> keys)
{
    for (int& key : keys) {
        key = keys::normalize(key);
    }
    m_reservedKeys = std::move(keys);
}

bool Console::isReservedKey(DWORD virtualKey) const
{
    const int key = keys::normalize(static_cast<int>(virtualKey));
    switch (key) {
    case VK_INSERT:
    case VK_END:
    case VK_LWIN:
    case VK_RWIN:
        return true;
    default:
        break;
    }

    return std::find(m_reservedKeys.begin(), m_reservedKeys.end(), key) != m_reservedKeys.end();
}

std::vector<int> Console::heldModifiers() const
{
    std::vector<int> combo;
    if (m_ctrlHeld) {
        combo.push_back(VK_CONTROL);
    }
    if (m_shiftHeld) {
        combo.push_back(VK_SHIFT);
    }
    if (m_altHeld) {
        combo.push_back(VK_MENU);
    }
    return combo;
}

bool Console::isMenuKey(DWORD virtualKey)
{
    switch (virtualKey) {
    case 'W': case 'A': case 'S': case 'D':
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
    case VK_SPACE: case VK_RETURN: case VK_ESCAPE: case VK_BACK:
    case VK_PRIOR: case VK_NEXT:
    case VK_OEM_PERIOD: case VK_DECIMAL:
    case VK_OEM_MINUS: case VK_SUBTRACT:
        return true;
    default:
        break;
    }

    return (virtualKey >= '0' && virtualKey <= '9')
           || (virtualKey >= VK_NUMPAD0 && virtualKey <= VK_NUMPAD9);
}

wchar_t Console::numberCharFor(DWORD virtualKey)
{
    if (virtualKey >= '0' && virtualKey <= '9') {
        return static_cast<wchar_t>(virtualKey);
    }
    if (virtualKey >= VK_NUMPAD0 && virtualKey <= VK_NUMPAD9) {
        return static_cast<wchar_t>(L'0' + (virtualKey - VK_NUMPAD0));
    }
    if (virtualKey == VK_OEM_PERIOD || virtualKey == VK_DECIMAL) {
        return L'.';
    }
    if (virtualKey == VK_OEM_MINUS || virtualKey == VK_SUBTRACT) {
        return L'-';
    }
    return L'\0';
}

bool Console::nudgeByKey(DWORD virtualKey)
{
    float dx = 0.0f;
    float dy = 0.0f;
    switch (virtualKey) {
    case VK_LEFT:  dx = -kNudgeStep; break;
    case VK_RIGHT: dx = kNudgeStep;  break;
    case VK_UP:    dy = -kNudgeStep; break;
    case VK_DOWN:  dy = kNudgeStep;  break;
    default: return false;
    }

    setOrigin(m_originX + dx, m_originY + dy);
    clampOrigin();
    return true;
}

bool Console::consumeKey(DWORD virtualKey, bool down)
{
    if (!m_visible) {
        return false;
    }

    if (!input::isGameForeground()) {
        return false;
    }

    switch (keys::normalize(static_cast<int>(virtualKey))) {
    case VK_CONTROL: m_ctrlHeld = down; break;
    case VK_SHIFT:   m_shiftHeld = down; break;
    case VK_MENU:    m_altHeld = down; break;
    default: break;
    }

    if (isReservedKey(virtualKey)) {
        return false;
    }

    const bool capturing = (m_menu.mode() == Menu::Mode::AwaitingKeybind);

    if (down) {

        if (!capturing && m_ctrlHeld && nudgeByKey(virtualKey)) {
            return true;
        }

        if (capturing || isMenuKey(virtualKey)) {
            onKeyDown(virtualKey);
        }

        if (m_menu.mode() == Menu::Mode::NumberEntry) {
            if (const wchar_t character = numberCharFor(virtualKey); character != L'\0') {
                onChar(character);
            }
        }
    }

    return true;
}

LRESULT CALLBACK Console::keyboardHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && s_hookOwner != nullptr) {
        const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        const bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
        if (down || up) {
            const auto* const event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
            if (s_hookOwner->consumeKey(event->vkCode, down)) {
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK Console::mouseHookProc(int code, WPARAM wParam, LPARAM lParam)
{

    Console* const self = s_mouseOwner;

    if (code == HC_ACTION && self != nullptr) {
        self->handleMouseEvent(wParam, *reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam));

        if (self->m_visible && self->m_gameForeground && isButtonMessage(wParam)) {
            return 1;
        }
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool Console::isButtonMessage(WPARAM message)
{
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        return true;
    default:
        return false;
    }
}

void Console::handleMouseEvent(WPARAM message, const MSLLHOOKSTRUCT& event)
{
    if (!m_visible) {
        return;
    }

    if (message == WM_MOUSEMOVE) {
        return;
    }

    if (message == WM_LBUTTONUP) {
        onLeftUp();
        return;
    }

    if (!m_gameForeground) {
        return;
    }

    float x = 0.0f;
    float y = 0.0f;
    if (!toBackbuffer(event.pt, x, y)) {
        return;
    }

    switch (message) {
    case WM_LBUTTONDOWN:
        onLeftDown(x, y);
        break;

    case WM_RBUTTONDOWN:

        onRightDown(x, y);
        break;

    case WM_MOUSEWHEEL:

        onWheel(GET_WHEEL_DELTA_WPARAM(event.mouseData) / WHEEL_DELTA, x, y);
        break;

    default:
        break;
    }
}

void Console::show()
{
    m_visible = true;
    installKeyHook();
    installMouseHook();

    input::setConsoleCapturing(true);

    input::setMouseCaptured(true);

    m_gameForeground = input::isGameForeground();

    clampOrigin();
    updateCursor();
    pushSnapshot();
}

void Console::hide()
{

    m_menu.cancelInput();

    m_closeHovered = false;
    m_dragging = false;
    m_cursorValid = false;
    m_gameForeground = false;

    removeKeyHook();
    removeMouseHook();

    input::setMouseCaptured(false);
    input::setConsoleCapturing(false);

    m_visible = false;

    render::setConsoleSnapshot(nullptr);
}

void Console::toggle()
{
    if (m_visible) {
        hide();
    } else {
        show();
    }
}

void Console::scrollLog(int lines)
{
    if (!m_showLog) {
        return;
    }

    if (lines > 0) {
        m_logScroll += static_cast<size_t>(lines);
    } else {
        const auto back = static_cast<size_t>(-lines);
        m_logScroll = (m_logScroll > back) ? m_logScroll - back : 0;
    }
}

bool Console::overLogArea(float y) const
{
    if (!m_showLog) {
        return false;
    }

    return y >= panelHeight() - theme::kLogHeight;
}

size_t Console::itemAtPoint(float x, float y) const
{
    if (m_menu.mode() != Menu::Mode::Normal) {
        return kNoItem;
    }

    const float left = theme::kPadding - 8.0f;
    const float right = panelWidth() - theme::kPadding + 8.0f;
    if (x < left || x >= right) {
        return kNoItem;
    }

    const float areaBottom = m_showLog ? panelHeight() - theme::kLogHeight : panelHeight();
    const float startY = theme::kHeaderHeight + 10.0f;
    if (y < startY || y >= areaBottom) {
        return kNoItem;
    }

    const float stride = theme::kRowHeight + theme::kRowGap;
    const float offset = y - startY;
    const auto row = static_cast<int>(offset / stride);
    if (offset - static_cast<float>(row) * stride >= theme::kRowHeight) {
        return kNoItem;
    }
    if (row >= theme::kVisibleRows) {
        return kNoItem;
    }

    const size_t index = m_menu.visibleBegin() + static_cast<size_t>(row);
    return (index < m_menu.visibleEnd()) ? index : kNoItem;
}

bool Console::overHeader(float y)
{
    return y >= 0.0f && y < theme::kHeaderHeight;
}

bool Console::hitsCloseButton(float x, float y) const
{
    return theme::hitsCloseButton(panelWidth(), x, y);
}

bool Console::consumeCloseClick(float x, float y)
{
    if (!hitsCloseButton(x, y)) {
        return false;
    }

    hide();
    return true;
}

void Console::hoverItem(float x, float y)
{
    const size_t index = itemAtPoint(x, y);
    if (index == kNoItem || index == m_menu.selected()) {
        return;
    }
    m_menu.select(index);
}

bool Console::toBackbuffer(POINT screen, float& x, float& y) const
{
    const render::Viewport view = render::overlayViewport();
    if (!view.valid || view.window == nullptr) {
        return false;
    }

    RECT client{};
    if (!GetClientRect(view.window, &client)) {
        return false;
    }

    const auto clientWidth = static_cast<float>(client.right - client.left);
    const auto clientHeight = static_cast<float>(client.bottom - client.top);
    if (clientWidth <= 0.0f || clientHeight <= 0.0f) {
        return false;
    }

    POINT point = screen;
    if (!ScreenToClient(view.window, &point)) {
        return false;
    }

    x = static_cast<float>(point.x) * (view.width / clientWidth);
    y = static_cast<float>(point.y) * (view.height / clientHeight);
    return true;
}

bool Console::toPanel(float backbufferX, float backbufferY, float& panelX, float& panelY) const
{
    const render::Viewport view = render::overlayViewport();
    if (!view.valid || view.scale <= 0.0f) {
        return false;
    }

    panelX = (backbufferX - m_originX) / view.scale;
    panelY = (backbufferY - m_originY) / view.scale;

    return panelX >= 0.0f && panelX < panelWidth() && panelY >= 0.0f && panelY < panelHeight();
}

void Console::updateCursor()
{

    if (!m_gameForeground) {
        m_cursorValid = false;
        return;
    }

    POINT screen{};
    if (GetCursorPos(&screen) == FALSE) {
        m_cursorValid = false;
        return;
    }

    float x = 0.0f;
    float y = 0.0f;
    if (!toBackbuffer(screen, x, y)) {
        m_cursorValid = false;
        return;
    }

    m_cursorX = x;
    m_cursorY = y;
    m_cursorValid = true;

    if (m_dragging) {
        setOrigin(x - m_dragGrabX, y - m_dragGrabY);
        clampOrigin();
        return;
    }

    float panelX = 0.0f;
    float panelY = 0.0f;
    if (!toPanel(x, y, panelX, panelY)) {
        m_closeHovered = false;
        return;
    }

    m_closeHovered = hitsCloseButton(panelX, panelY);
    hoverItem(panelX, panelY);
}

void Console::onLeftDown(float backbufferX, float backbufferY)
{
    float x = 0.0f;
    float y = 0.0f;
    if (!toPanel(backbufferX, backbufferY, x, y)) {
        return;
    }

    if (consumeCloseClick(x, y)) {
        return;
    }

    if (overHeader(y)) {
        m_dragging = true;
        m_dragGrabX = backbufferX - m_originX;
        m_dragGrabY = backbufferY - m_originY;
        return;
    }

    switch (m_menu.mode()) {
    case Menu::Mode::NumberEntry:

        m_menu.commitNumber();
        return;

    case Menu::Mode::AwaitingKeybind:

        return;

    case Menu::Mode::Normal:
    default:
        break;
    }

    const size_t index = itemAtPoint(x, y);
    if (index == kNoItem) {
        return;
    }

    m_menu.select(index);
    if (m_menu.selected() != index) {
        return;
    }

    if (!m_menu.toggleSelected()) {
        m_menu.activate();
    }
}

void Console::onLeftUp()
{
    if (!m_dragging) {
        return;
    }

    m_dragging = false;
    clampOrigin();
}

void Console::onRightDown(float backbufferX, float backbufferY)
{
    float x = 0.0f;
    float y = 0.0f;
    if (!toPanel(backbufferX, backbufferY, x, y)) {
        return;
    }

    if (consumeCloseClick(x, y)) {
        return;
    }

    switch (m_menu.mode()) {
    case Menu::Mode::NumberEntry:
        m_menu.commitNumber();
        return;

    case Menu::Mode::AwaitingKeybind:
        return;

    case Menu::Mode::Normal:
    default:
        break;
    }

    const size_t index = itemAtPoint(x, y);
    if (index == kNoItem) {
        return;
    }

    m_menu.select(index);
    if (m_menu.selected() != index) {
        return;
    }

    m_menu.activate();
}

void Console::onWheel(int delta, float backbufferX, float backbufferY)
{
    if (delta == 0) {
        return;
    }

    float x = 0.0f;
    float y = 0.0f;
    if (!toPanel(backbufferX, backbufferY, x, y)) {
        return;
    }

    if (overLogArea(y)) {
        scrollLog(delta);
        return;
    }

    m_menu.scrollView(-delta);
}

void Console::onKeyDown(WPARAM key)
{

    if (m_menu.inCooldown()) {
        return;
    }

    if (m_menu.mode() == Menu::Mode::AwaitingKeybind) {
        m_menu.captureKey(static_cast<int>(key), heldModifiers());
        return;
    }

    if (m_menu.mode() == Menu::Mode::NumberEntry) {
        switch (key) {
        case VK_RETURN:
            m_menu.commitNumber();
            break;
        case VK_ESCAPE:
            m_menu.cancelInput();
            break;
        case VK_BACK:
            m_menu.backspaceNumber();
            break;
        default:
            break;
        }
        return;
    }

    switch (key) {
    case 'W':
    case VK_UP:
        m_menu.moveUp();
        break;

    case 'S':
    case VK_DOWN:
        m_menu.moveDown();
        break;

    case VK_SPACE:
    case VK_RETURN:

        if (m_shiftHeld) {
            m_menu.toggleSelected();
        } else {
            m_menu.activate();
        }
        break;

    case VK_BACK:
        m_menu.back();
        break;

    case VK_ESCAPE:

        if (m_menu.atRoot()) {
            hide();
        } else {
            m_menu.back();
        }
        break;

    case VK_PRIOR:
        scrollLog(static_cast<int>(m_logCapacity));
        break;

    case VK_NEXT:
        scrollLog(-static_cast<int>(m_logCapacity));
        break;

    default:
        break;
    }
}

void Console::onChar(wchar_t character)
{
    if (m_menu.mode() != Menu::Mode::NumberEntry || m_menu.inCooldown()) {
        return;
    }
    if (character == L'\r' || character == L'\n' || character == L'\b') {
        return;
    }

    m_menu.appendNumberChar(character);
}

void Console::updateAnimation()
{
    const auto now = Clock::now();
    const auto elapsed = std::chrono::duration<float, std::milli>(now - m_lastFrame).count();
    m_lastFrame = now;

    const size_t begin = m_menu.visibleBegin();
    const size_t end = m_menu.visibleEnd();
    const size_t selected = m_menu.selected();

    if (selected < begin || selected >= end) {
        m_selectionVisible = false;
        m_lastVisibleBegin = begin;
        return;
    }

    if (!m_selectionVisible) {

        m_selectionVisible = true;
        m_selectionRow = static_cast<float>(selected - begin);
        m_lastVisibleBegin = begin;
        return;
    }

    const auto target = static_cast<float>(selected - begin);

    if (begin != m_lastVisibleBegin) {

        m_selectionRow = target;
        m_lastVisibleBegin = begin;
        return;
    }

    const float distance = target - m_selectionRow;
    if (std::fabs(distance) < 0.002f) {
        m_selectionRow = target;
        return;
    }

    const float rate = 1.0f - std::exp(-elapsed / (theme::kSelectionAnimationMs / 3.0f));
    m_selectionRow += distance * (std::min)(rate, 1.0f);
}

ConsoleSnapshot Console::buildSnapshot() const
{
    ConsoleSnapshot snapshot;

    snapshot.title = m_menu.title();
    snapshot.mode = m_menu.mode();
    snapshot.atRoot = m_menu.atRoot();
    snapshot.closeHovered = m_closeHovered;

    snapshot.selectionRow = m_selectionRow;
    snapshot.selectionVisible = m_selectionVisible;

    if (snapshot.mode == Menu::Mode::Normal) {
        const std::vector<MenuItem>& items = m_menu.items();
        const size_t begin = m_menu.visibleBegin();
        const size_t end = m_menu.visibleEnd();
        const size_t selected = m_menu.selected();

        snapshot.rows.reserve(end - begin);
        for (size_t index = begin; index < end; ++index) {
            const MenuItem& item = items[index];

            MenuRow row;
            row.kind = item.kind;
            row.label = item.labelText();
            row.value = item.valueText();
            row.available = item.isAvailable();

            row.hasState = static_cast<bool>(item.isOn);
            row.on = item.toggleState();

            snapshot.rows.push_back(std::move(row));
        }

        snapshot.selectedRow = (selected >= begin && selected < end)
                                   ? selected - begin
                                   : ConsoleSnapshot::kNoRow;
    } else {
        const MenuItem* item = m_menu.editingItem();
        snapshot.promptLabel = (item != nullptr) ? item->labelText() : std::wstring{};
        snapshot.promptBody = (snapshot.mode == Menu::Mode::AwaitingKeybind)
                                  ? m_menu.capturedKeysText()
                                  : (m_menu.numberBuffer().empty() ? std::wstring(L"_")
                                                                   : m_menu.numberBuffer());
    }

    snapshot.showLog = m_showLog;
    if (m_showLog) {

        snapshot.logs = log().snapshot();
        snapshot.logScroll = m_logScroll;
    }

    snapshot.originX = m_originX;
    snapshot.originY = m_originY;

    snapshot.cursorX = m_cursorX;
    snapshot.cursorY = m_cursorY;
    snapshot.cursorValid = m_cursorValid;

    return snapshot;
}

void Console::pushSnapshot()
{
    if (!m_visible) {
        render::setConsoleSnapshot(nullptr);
        return;
    }

    clampOrigin();
    m_logCapacity = theme::logCapacity();

    auto snapshot = std::make_shared<ConsoleSnapshot>(buildSnapshot());

    if (snapshot->showLog) {
        const size_t count = snapshot->logs.size();
        const size_t maxScroll = (count > m_logCapacity) ? count - m_logCapacity : 0;
        if (m_logScroll > maxScroll) {
            m_logScroll = maxScroll;
            snapshot->logScroll = m_logScroll;
        }
    }

    render::setConsoleSnapshot(std::move(snapshot));
}

void Console::pump()
{

    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (!m_visible) {
        return;
    }

    m_gameForeground = input::isGameForeground();

    updateCursor();

    updateAnimation();

    pushSnapshot();
}

}
