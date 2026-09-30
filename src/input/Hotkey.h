#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace tsukuyomi {

class Hotkey {
public:
    Hotkey() = default;
    explicit Hotkey(std::vector<int> combo);
    Hotkey(std::vector<int> combo, const char* output);

    void useLegacyOnly() { m_legacyOnly = true; }
    void set(std::vector<int> combo);
    const std::vector<int>& combo() const { return m_combo; }
    bool empty() const { return m_combo.empty(); }

    std::wstring name() const;

    bool triggered();
    bool releasedAlone();

    bool isDown() const;

private:
    std::vector<int> m_combo;
    bool m_wasDown = false;
    bool m_legacyOnly = false;
    int m_slot = -1;
    const char* m_output = nullptr;
    std::uint64_t m_seenSeq = 0;
    std::uint64_t m_seenAloneSeq = 0;
};

}
