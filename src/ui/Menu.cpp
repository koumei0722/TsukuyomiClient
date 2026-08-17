#include "ui/Menu.h"

#include "input/Keys.h"

#include <algorithm>
#include <format>
#include <utility>

namespace tsukuyomi {

namespace menu {

namespace {

std::function<std::wstring()> constant(std::wstring text)
{
    return [text = std::move(text)]() { return text; };
}

}

MenuItem submenu(std::wstring title, std::vector<MenuItem> children)
{
    MenuItem item;
    item.kind = MenuItemKind::Submenu;
    item.label = constant(std::move(title));
    item.children = std::move(children);
    return item;
}

MenuItem back()
{
    MenuItem item;
    item.kind = MenuItemKind::Back;
    item.label = constant(L"..");
    return item;
}

MenuItem toggle(std::wstring title, std::function<bool()> isOn, std::function<void()> flip)
{
    MenuItem item;
    item.kind = MenuItemKind::Toggle;
    item.label = constant(std::move(title));
    item.isOn = std::move(isOn);
    item.activate = std::move(flip);
    return item;
}

MenuItem action(std::wstring title, std::function<void()> run)
{
    MenuItem item;
    item.kind = MenuItemKind::Action;
    item.label = constant(std::move(title));
    item.activate = std::move(run);
    return item;
}

MenuItem cycle(std::wstring title, std::function<std::wstring()> valueText,
               std::function<void()> next)
{
    MenuItem item;
    item.kind = MenuItemKind::Cycle;
    item.label = constant(std::move(title));
    item.value = std::move(valueText);
    item.activate = std::move(next);
    return item;
}

MenuItem choice(std::wstring title, std::vector<std::wstring> choices, std::function<int()> get,
                std::function<void(int)> set)
{
    MenuItem item;
    item.kind = MenuItemKind::Cycle;
    item.label = constant(std::move(title));
    item.choices = std::move(choices);
    item.getChoice = std::move(get);
    item.setChoice = std::move(set);

    item.value = [list = item.choices, getter = item.getChoice]() -> std::wstring {
        if (!getter) {
            return {};
        }
        const int at = getter();
        if (at < 0 || static_cast<size_t>(at) >= list.size()) {
            return {};
        }
        return list[static_cast<size_t>(at)];
    };
    item.activate = [count = item.choices.size(), getter = item.getChoice,
                     setter = item.setChoice]() {
        if (!getter || !setter || count == 0) {
            return;
        }
        setter(static_cast<int>((static_cast<size_t>(getter()) + 1) % count));
    };
    return item;
}

MenuItem keybind(std::wstring title, std::function<std::vector<int>()> get,
                 std::function<void(std::vector<int>)> set, std::vector<int> defaults)
{
    MenuItem item;
    item.kind = MenuItemKind::Keybind;
    item.label = constant(std::move(title));
    item.getKeys = std::move(get);
    item.setKeys = std::move(set);
    item.defaultKeys = std::move(defaults);
    item.value = [getter = item.getKeys]() {
        return getter ? keys::comboName(getter()) : std::wstring{};
    };
    return item;
}

MenuItem number(std::wstring title, std::function<float()> get, std::function<void(float)> set,
                bool integer, float min, float max)
{
    MenuItem item;
    item.kind = MenuItemKind::Number;
    item.label = constant(std::move(title));
    item.getNumber = std::move(get);
    item.setNumber = std::move(set);
    item.numberIsInteger = integer;
    item.numberMin = min;
    item.numberMax = max;
    item.value = [getter = item.getNumber, integer]() -> std::wstring {
        if (!getter) {
            return {};
        }
        const float current = getter();
        return integer ? std::format(L"{}", static_cast<int>(current))
                       : std::format(L"{:g}", current);
    };
    return item;
}

}

}
