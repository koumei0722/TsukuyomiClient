#pragma once

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <vector>

#include "ui/ConsoleSnapshot.h"
#include "ui/Menu.h"

namespace tsukuyomi {

class Console {
public:
    bool create();
    void destroy();

    void show();
    void hide();
    void toggle();

    void setReservedKeys(std::vector<int> keys);
    bool visible() const { return m_visible; }

    Menu& menu() { return m_menu; }

    float originX() const { return m_originX; }
    float originY() const { return m_originY; }
    void setOrigin(float x, float y);
    bool hasOrigin() const { return m_hasOrigin; }

    bool logVisible() const { return m_showLog; }
    void setLogVisible(bool visible);

    void pump();

private:

    static LRESULT CALLBACK keyboardHookProc(int code, WPARAM wParam, LPARAM lParam);
    void installKeyHook();
    void removeKeyHook();

    static LRESULT CALLBACK mouseHookProc(int code, WPARAM wParam, LPARAM lParam);
    void installMouseHook();
    void removeMouseHook();

    void handleMouseEvent(WPARAM message, const MSLLHOOKSTRUCT& event);

    static bool isButtonMessage(WPARAM message);

    bool consumeKey(DWORD virtualKey, bool down);

    bool isReservedKey(DWORD virtualKey) const;

    std::vector<int> heldModifiers() const;

    static bool isMenuKey(DWORD virtualKey);
    static wchar_t numberCharFor(DWORD virtualKey);

    bool nudgeByKey(DWORD virtualKey);

    static constexpr float kNudgeStep = 24.0f;

    void onKeyDown(WPARAM key);
    void onChar(wchar_t character);
    void scrollLog(int lines);

    bool toBackbuffer(POINT screen, float& x, float& y) const;

    bool toPanel(float backbufferX, float backbufferY, float& panelX, float& panelY) const;

    void updateCursor();

    void onLeftDown(float backbufferX, float backbufferY);
    void onLeftUp();
    void onRightDown(float backbufferX, float backbufferY);
    void onWheel(int delta, float backbufferX, float backbufferY);

    bool hitsCloseButton(float x, float y) const;
    bool consumeCloseClick(float x, float y);
    void hoverItem(float x, float y);

    static bool overHeader(float y);

    bool overLogArea(float y) const;

    size_t itemAtPoint(float x, float y) const;
    static constexpr size_t kNoItem = static_cast<size_t>(-1);

    void updateAnimation();

    ConsoleSnapshot buildSnapshot() const;

    void pushSnapshot();

    float panelWidth() const;
    float panelHeight() const;

    void clampOrigin();

    using Clock = std::chrono::steady_clock;

    static Console* s_hookOwner;
    static Console* s_mouseOwner;

    HHOOK m_keyHook = nullptr;
    HHOOK m_mouseHook = nullptr;
    bool m_visible = false;

    bool m_ctrlHeld = false;
    bool m_shiftHeld = false;
    bool m_altHeld = false;

    std::vector<int> m_reservedKeys;

    float m_originX = 0.0f;
    float m_originY = 0.0f;
    bool m_hasOrigin = false;

    bool m_showLog = false;

    bool m_closeHovered = false;

    float m_cursorX = 0.0f;
    float m_cursorY = 0.0f;
    bool m_cursorValid = false;

    bool m_gameForeground = false;

    bool m_dragging = false;
    float m_dragGrabX = 0.0f;
    float m_dragGrabY = 0.0f;

    Menu m_menu;

    float m_selectionRow = 0.0f;

    bool m_selectionVisible = true;

    size_t m_lastVisibleBegin = 0;
    size_t m_logScroll = 0;
    size_t m_logCapacity = 1;

    Clock::time_point m_lastFrame = Clock::now();
};

}
