#pragma once

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

    std::vector<int> defaultKeys;

    std::function<float()> getNumber;
    std::function<void(float)> setNumber;
    bool numberIsInteger = false;

    float numberMin = 0.0f;
    float numberMax = 0.0f;

    std::vector<std::wstring> choices;
    std::function<int()> getChoice;
    std::function<void(int)> setChoice;

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

MenuItem choice(std::wstring title, std::vector<std::wstring> choices, std::function<int()> get,
                std::function<void(int)> set);

MenuItem keybind(std::wstring title, std::function<std::vector<int>()> get,
                 std::function<void(std::vector<int>)> set, std::vector<int> defaults = {});

MenuItem number(std::wstring title, std::function<float()> get, std::function<void(float)> set,
                bool integer, float min = 0.0f, float max = 0.0f);

}

}
