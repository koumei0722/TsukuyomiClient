#pragma once

#include <string>
#include <vector>

namespace tsukuyomi {

class Hotkey {
public:
    Hotkey() = default;
    explicit Hotkey(std::vector<int> combo);

    void set(std::vector<int> combo);
    const std::vector<int>& combo() const { return m_combo; }
    bool empty() const { return m_combo.empty(); }

    std::wstring name() const;

    bool triggered();

    bool isDown() const;

    void reset() { m_wasDown = false; }

private:
    std::vector<int> m_combo;
    bool m_wasDown = false;
};

}
