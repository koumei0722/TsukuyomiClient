#include "input/Hotkey.h"

#include "input/Keys.h"

#include <algorithm>
#include <utility>

namespace tsukuyomi {

Hotkey::Hotkey(std::vector<int> combo)
{
    set(std::move(combo));
}

void Hotkey::set(std::vector<int> combo)
{
    for (int& key : combo) {
        key = keys::normalize(key);
    }

    std::stable_sort(combo.begin(), combo.end(), [](int lhs, int rhs) {
        return keys::isModifier(lhs) && !keys::isModifier(rhs);
    });
    combo.erase(std::unique(combo.begin(), combo.end()), combo.end());

    m_combo = std::move(combo);
    m_wasDown = false;
}

std::wstring Hotkey::name() const
{
    return keys::comboName(m_combo);
}

bool Hotkey::isDown() const
{
    return keys::isComboDown(m_combo);
}

bool Hotkey::triggered()
{
    if (m_combo.empty()) {
        return false;
    }

    const bool down = isDown();
    if (down && !m_wasDown) {
        m_wasDown = true;
        return true;
    }
    if (!down) {
        m_wasDown = false;
    }
    return false;
}

}
