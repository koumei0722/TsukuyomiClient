#include "ui/Menu.h"

#include "input/Keys.h"
#include "ui/Theme.h"

#include <Windows.h>

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

MenuItem keybind(std::wstring title, std::function<std::vector<int>()> get,
                 std::function<void(std::vector<int>)> set)
{
    MenuItem item;
    item.kind = MenuItemKind::Keybind;
    item.label = constant(std::move(title));
    item.getKeys = std::move(get);
    item.setKeys = std::move(set);
    item.value = [getter = item.getKeys]() {
        return getter ? keys::comboName(getter()) : std::wstring{};
    };
    return item;
}

MenuItem number(std::wstring title, std::function<float()> get,
                std::function<void(float)> set, bool integer)
{
    MenuItem item;
    item.kind = MenuItemKind::Number;
    item.label = constant(std::move(title));
    item.getNumber = std::move(get);
    item.setNumber = std::move(set);
    item.numberIsInteger = integer;
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

void Menu::setRoot(MenuItem root)
{
    m_root = std::move(root);
    resetToRoot();
}

void Menu::resetToRoot()
{
    m_path.clear();
    m_selected = 0;
    m_mode = Mode::Normal;
    m_capturedKeys.clear();
    m_numberBuffer.clear();
    clampSelection();
}

const MenuItem& Menu::node() const
{
    const MenuItem* current = &m_root;
    for (const size_t index : m_path) {
        if (index >= current->children.size()) {
            break;
        }
        current = &current->children[index];
    }
    return *current;
}

const std::vector<MenuItem>& Menu::items() const
{
    return node().children;
}

std::wstring Menu::title() const
{
    return m_path.empty() ? std::wstring(L"Tsukuyomi") : node().labelText();
}

size_t Menu::maxViewTop() const
{
    const size_t count = items().size();
    const auto perPage = static_cast<size_t>(theme::kVisibleRows);
    return (count <= perPage) ? 0 : count - perPage;
}

size_t Menu::visibleBegin() const
{

    return (std::min)(m_viewTop, maxViewTop());
}

void Menu::scrollView(int lines)
{
    const size_t limit = maxViewTop();
    if (limit == 0) {
        m_viewTop = 0;
        return;
    }

    if (lines < 0) {
        const auto up = static_cast<size_t>(-static_cast<long long>(lines));
        m_viewTop = (m_viewTop > up) ? m_viewTop - up : 0;
    } else {
        m_viewTop = (std::min)(m_viewTop + static_cast<size_t>(lines), limit);
    }
}

void Menu::ensureSelectionVisible()
{
    const auto perPage = static_cast<size_t>(theme::kVisibleRows);
    const size_t limit = maxViewTop();
    if (limit == 0) {
        m_viewTop = 0;
        return;
    }

    if (m_selected < m_viewTop) {
        m_viewTop = m_selected;
    } else if (m_selected >= m_viewTop + perPage) {
        m_viewTop = m_selected - perPage + 1;
    }
    m_viewTop = (std::min)(m_viewTop, limit);
}

size_t Menu::visibleEnd() const
{
    return (std::min)(visibleBegin() + static_cast<size_t>(theme::kVisibleRows), items().size());
}

void Menu::clampSelection()
{
    const size_t count = items().size();
    if (count == 0) {
        m_selected = 0;
    } else if (m_selected >= count) {
        m_selected = count - 1;
    }

    ensureSelectionVisible();
}

void Menu::moveUp()
{
    const auto& list = items();
    if (list.empty()) {
        return;
    }

    for (size_t step = 0; step < list.size(); ++step) {
        m_selected = (m_selected == 0) ? list.size() - 1 : m_selected - 1;
        if (list[m_selected].isAvailable()) {
            ensureSelectionVisible();
            return;
        }
    }
}

void Menu::moveDown()
{
    const auto& list = items();
    if (list.empty()) {
        return;
    }

    for (size_t step = 0; step < list.size(); ++step) {
        m_selected = (m_selected + 1 >= list.size()) ? 0 : m_selected + 1;
        if (list[m_selected].isAvailable()) {
            ensureSelectionVisible();
            return;
        }
    }
}

void Menu::select(size_t index)
{
    if (m_mode != Mode::Normal) {
        return;
    }

    const auto& list = items();
    if (index >= list.size() || !list[index].isAvailable()) {
        return;
    }
    m_selected = index;

}

bool Menu::toggleSelected()
{
    if (m_mode != Mode::Normal) {
        return false;
    }

    const auto& list = items();
    if (m_selected >= list.size()) {
        return false;
    }

    const MenuItem& item = list[m_selected];
    if (!item.isAvailable()) {
        return false;
    }

    if (item.kind == MenuItemKind::Toggle) {
        if (item.activate) {
            item.activate();
        }
        return true;
    }

    if (item.kind == MenuItemKind::Submenu && item.isOn) {
        for (const MenuItem& child : item.children) {
            if (child.kind == MenuItemKind::Toggle && child.isAvailable()) {
                if (child.activate) {
                    child.activate();
                }
                return true;
            }
        }
    }

    return false;
}

void Menu::activate()
{
    const auto& list = items();
    if (m_selected >= list.size()) {
        return;
    }

    const MenuItem& item = list[m_selected];
    if (!item.isAvailable()) {
        return;
    }

    switch (item.kind) {
    case MenuItemKind::Submenu: {

        m_path.push_back(m_selected);
        m_selected = 0;
        clampSelection();

        if (!items().empty() && !items()[m_selected].isAvailable()) {
            moveDown();
        }
        break;
    }

    case MenuItemKind::Back:
        back();
        break;

    case MenuItemKind::Keybind:
        m_mode = Mode::AwaitingKeybind;
        m_editingIndex = m_selected;
        m_capturedKeys.clear();

        startCooldown();
        break;

    case MenuItemKind::Number: {
        m_mode = Mode::NumberEntry;
        m_editingIndex = m_selected;
        m_numberBuffer.clear();
        if (item.getNumber) {
            const float current = item.getNumber();
            m_numberBuffer = item.numberIsInteger ? std::format(L"{}", static_cast<int>(current))
                                                  : std::format(L"{:g}", current);
        }
        break;
    }

    case MenuItemKind::Toggle:
    case MenuItemKind::Cycle:
    case MenuItemKind::Action:
    default:
        if (item.activate) {
            item.activate();
        }
        break;
    }
}

void Menu::back()
{
    if (m_mode != Mode::Normal) {
        cancelInput();
        return;
    }
    if (m_path.empty()) {
        return;
    }

    const size_t childIndex = m_path.back();
    m_path.pop_back();
    m_selected = childIndex;
    clampSelection();
}

const MenuItem* Menu::editingItem() const
{
    const auto& list = items();
    if (m_mode == Mode::Normal || m_editingIndex >= list.size()) {
        return nullptr;
    }
    return &list[m_editingIndex];
}

std::wstring Menu::capturedKeysText() const
{
    if (m_capturedKeys.empty()) {
        return L"— — —";
    }

    return keys::comboName(m_capturedKeys) + L" + ...";
}

void Menu::captureKey(int virtualKey, std::vector<int> held)
{
    if (m_mode != Mode::AwaitingKeybind) {
        return;
    }

    const int key = keys::normalize(virtualKey);

    std::vector<int> combo;

    if (key != VK_ESCAPE) {
        combo = std::move(held);

        if (keys::isModifier(key)) {

            m_capturedKeys = std::move(combo);
            return;
        }

        combo.push_back(key);
    }

    const MenuItem* item = editingItem();
    if (item != nullptr && item->setKeys) {
        item->setKeys(std::move(combo));
    }

    m_capturedKeys.clear();
    m_mode = Mode::Normal;
    startCooldown();
}

void Menu::appendNumberChar(wchar_t ch)
{
    if (m_mode != Mode::NumberEntry) {
        return;
    }
    if (m_numberBuffer.size() >= 16) {
        return;
    }

    const bool isDigit = (ch >= L'0' && ch <= L'9');
    const bool isDot = (ch == L'.' && m_numberBuffer.find(L'.') == std::wstring::npos);
    const bool isSign = (ch == L'-' && m_numberBuffer.empty());

    if (isDigit || isDot || isSign) {
        m_numberBuffer.push_back(ch);
    }
}

void Menu::backspaceNumber()
{
    if (m_mode == Mode::NumberEntry && !m_numberBuffer.empty()) {
        m_numberBuffer.pop_back();
    }
}

void Menu::commitNumber()
{
    if (m_mode != Mode::NumberEntry) {
        return;
    }

    const MenuItem* item = editingItem();
    if (item != nullptr && item->setNumber && !m_numberBuffer.empty()) {
        try {
            const float parsed = std::stof(m_numberBuffer);
            item->setNumber(parsed);
        } catch (const std::exception&) {

        }
    }

    m_numberBuffer.clear();
    m_mode = Mode::Normal;
    startCooldown();
}

void Menu::cancelInput()
{
    if (m_mode == Mode::Normal) {
        return;
    }
    m_mode = Mode::Normal;
    m_capturedKeys.clear();
    m_numberBuffer.clear();
    startCooldown();
}

void Menu::startCooldown()
{
    m_cooldownUntil = Clock::now() + std::chrono::milliseconds(kCooldownMs);
}

bool Menu::inCooldown() const
{
    return Clock::now() < m_cooldownUntil;
}

}
