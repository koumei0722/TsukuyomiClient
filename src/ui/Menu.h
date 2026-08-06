#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace tsukuyomi {

enum class MenuItemKind {
    Submenu,
    Back,
    Toggle,
    Cycle,
    Keybind,
    Number,
    Action,
};

struct MenuItem {
    MenuItemKind kind = MenuItemKind::Action;

    std::function<std::wstring()> label;
    std::function<std::wstring()> value;
    std::function<bool()> available;
    std::function<bool()> isOn;
    std::function<void()> activate;

    std::function<std::vector<int>()> getKeys;
    std::function<void(std::vector<int>)> setKeys;

    std::function<float()> getNumber;
    std::function<void(float)> setNumber;
    bool numberIsInteger = false;

    std::vector<MenuItem> children;

    std::wstring labelText() const { return label ? label() : std::wstring{}; }
    std::wstring valueText() const { return value ? value() : std::wstring{}; }
    bool isAvailable() const { return available ? available() : true; }
    bool toggleState() const { return isOn && isOn(); }
};

namespace menu {

MenuItem submenu(std::wstring title, std::vector<MenuItem> children);
MenuItem back();
MenuItem toggle(std::wstring title, std::function<bool()> isOn, std::function<void()> flip);
MenuItem action(std::wstring title, std::function<void()> run);
MenuItem cycle(std::wstring title, std::function<std::wstring()> valueText,
               std::function<void()> next);
MenuItem keybind(std::wstring title, std::function<std::vector<int>()> get,
                 std::function<void(std::vector<int>)> set);
MenuItem number(std::wstring title, std::function<float()> get,
                std::function<void(float)> set, bool integer);

}

class Menu {
public:
    enum class Mode {
        Normal,
        AwaitingKeybind,
        NumberEntry,
    };

    void setRoot(MenuItem root);
    void resetToRoot();

    const MenuItem& node() const;
    const std::vector<MenuItem>& items() const;
    std::wstring title() const;
    bool atRoot() const { return m_path.empty(); }

    size_t selected() const { return m_selected; }

    size_t visibleBegin() const;
    size_t visibleEnd() const;

    void moveUp();
    void moveDown();

    void scrollView(int lines);

    void select(size_t index);
    void activate();

    bool toggleSelected();

    void back();

    Mode mode() const { return m_mode; }
    const MenuItem* editingItem() const;
    const std::wstring& numberBuffer() const { return m_numberBuffer; }
    std::wstring capturedKeysText() const;

    void captureKey(int virtualKey, std::vector<int> held);

    void appendNumberChar(wchar_t ch);
    void backspaceNumber();
    void commitNumber();
    void cancelInput();

    bool inCooldown() const;

private:
    using Clock = std::chrono::steady_clock;

    const MenuItem* editingItemMutable() const;
    void startCooldown();
    void clampSelection();

    size_t maxViewTop() const;

    void ensureSelectionVisible();

    static constexpr int kCooldownMs = 300;

    MenuItem m_root;
    std::vector<size_t> m_path;
    size_t m_selected = 0;

    size_t m_viewTop = 0;

    Mode m_mode = Mode::Normal;
    size_t m_editingIndex = 0;

    std::vector<int> m_capturedKeys;

    std::wstring m_numberBuffer;

    Clock::time_point m_cooldownUntil{};
};

}
