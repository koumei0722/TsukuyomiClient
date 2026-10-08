#include "input/Hotkey.h"

#include "input/Keys.h"
#include "input/GameButtons.h"
#include "input/PadKeys.h"

#include <algorithm>
#include <utility>

namespace tsukuyomi {

Hotkey::Hotkey(std::vector<int> combo)
{
    set(std::move(combo));
}

Hotkey::Hotkey(std::vector<int> combo, const char* output)
    : m_output(output)
{
    set(std::move(combo));
}

void Hotkey::set(std::vector<int> combo)
{
    const Guard guard(m_lock);
    std::vector<int> distinct;
    for (int key : combo) {
        key = keys::normalize(key);
        if (std::find(distinct.begin(), distinct.end(), key) == distinct.end()) distinct.push_back(key);
    }
    m_combo = std::move(distinct);
    m_wasDown = false;
    if (!m_legacyOnly) {
        GameButtons& buttons = GameButtons::instance();
        if (m_slot < 0) m_slot = buttons.attach(m_combo, m_output);
        else buttons.setKeys(m_slot, m_combo);
        m_seenSeq = buttons.pressSeq(m_slot);
        m_seenAloneSeq = buttons.aloneReleaseSeq(m_slot);
    }
}

std::vector<int> Hotkey::combo() const
{
    const Guard guard(m_lock);
    return m_combo;
}

void Hotkey::setPad(std::vector<int> combo)
{
    const Guard guard(m_lock);
    if (m_legacyOnly) return;
    std::vector<int> distinct;
    for (int value : combo) {
        if (std::none_of(padkeys::kButtons.begin(), padkeys::kButtons.end(),
                [value](const padkeys::Button& button) { return button.value == value; })) return;
        if (std::find(distinct.begin(), distinct.end(), value) == distinct.end()) distinct.push_back(value);
    }
    if (distinct.size() > gamebuttonlogic::kMaxComboKeys) return;
    m_padCombo = std::move(distinct);
    GameButtons& buttons = GameButtons::instance();
    if (m_slot < 0) m_slot = buttons.attach(m_combo, m_output);
    buttons.setPadKeys(m_slot, m_padCombo);
    m_seenSeq = buttons.pressSeq(m_slot);
    m_seenAloneSeq = buttons.aloneReleaseSeq(m_slot);
    m_wasDown = false;
}

std::vector<int> Hotkey::padCombo() const
{
    const Guard guard(m_lock);
    return m_padCombo;
}

bool Hotkey::empty() const
{
    const Guard guard(m_lock);
    return m_combo.empty() && m_padCombo.empty();
}

std::wstring Hotkey::name() const
{
    const Guard guard(m_lock);
    return keys::comboName(m_combo);
}

bool Hotkey::isDown() const
{
    const Guard guard(m_lock);
    return isDownLocked();
}

bool Hotkey::isDownLocked() const
{
    if (m_legacyOnly) {
        return keys::isComboDown(m_combo);
    }
    GameButtons& buttons = GameButtons::instance();
    return buttons.ready(m_slot) && buttons.held(m_slot);
}

bool Hotkey::triggered()
{
    const Guard guard(m_lock);
    if (m_combo.empty() && m_padCombo.empty()) {
        return false;
    }

    if (!m_legacyOnly) {
        GameButtons& buttons = GameButtons::instance();
        const std::uint64_t seq = buttons.pressSeq(m_slot);
        if (seq == m_seenSeq) return false;
        m_seenSeq = seq;
        return buttons.ready(m_slot) && gamebuttonlogic::freshPress(GetTickCount64(), buttons.lastPressMs(m_slot));
    }

    const bool down = isDownLocked();
    if (down && !m_wasDown) {
        m_wasDown = true;
        return true;
    }
    if (!down) {
        m_wasDown = false;
    }
    return false;
}

bool Hotkey::releasedAlone()
{
    const Guard guard(m_lock);
    if ((m_combo.empty() && m_padCombo.empty()) || m_legacyOnly) return false;
    GameButtons& buttons = GameButtons::instance();
    const std::uint64_t seq = buttons.aloneReleaseSeq(m_slot);
    if (seq == m_seenAloneSeq) return false;
    m_seenAloneSeq = seq;
    return buttons.ready(m_slot);
}

}
