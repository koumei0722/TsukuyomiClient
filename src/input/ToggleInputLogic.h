#pragma once

#include <cstdint>

namespace tsukuyomi::toggleinput {

inline constexpr std::uint32_t kSneakLow = 1u << 0;
inline constexpr std::uint32_t kSneakHeld = 1u << 21;
inline constexpr std::uint32_t kSneakRelease = 1u << 22;
inline constexpr std::uint32_t kSneakPress = 1u << 23;
inline constexpr std::uint32_t kSneakHeldBits = kSneakLow | kSneakHeld;
inline constexpr std::uint32_t kSneakAllBits = kSneakHeldBits | kSneakRelease | kSneakPress;
inline constexpr std::uint32_t kSprint = 1u << 8;

class ToggleInput {
public:
    std::uint32_t apply(std::uint32_t raw, bool sneakOn, bool sprintOn)
    {
        return applySprint(applySneak(raw, sneakOn), raw, sprintOn);
    }

    void passThrough(std::uint32_t raw)
    {
        m_sneakShown = (raw & kSneakHeld) != 0;
        m_sprintRawPrev = (raw & kSprint) != 0;
    }

    bool sneakLatched() const { return m_sneakLatched; }
    bool sprintLatched() const { return m_sprintLatched; }

private:
    std::uint32_t applySneak(std::uint32_t raw, bool on)
    {
        const bool pressed = (raw & kSneakPress) != 0;
        const bool held = (raw & kSneakHeld) != 0;
        if (m_sneakSwallow && !held) {
            m_sneakSwallow = false;
        }
        if (!on) {
            m_sneakLatched = false;
            m_sneakSwallow = false;
        } else if (pressed && !m_sneakSwallow) {
            if (m_sneakLatched) {
                m_sneakLatched = false;
                m_sneakSwallow = true;
            } else {
                m_sneakLatched = true;
            }
        }
        const bool show = m_sneakLatched || (held && !m_sneakSwallow);
        std::uint32_t out = raw & ~kSneakAllBits;
        if (show) {
            out |= kSneakHeldBits;
            if (!m_sneakShown) out |= kSneakPress;
        } else if (m_sneakShown) {
            out |= kSneakRelease;
        }
        m_sneakShown = show;
        return out;
    }

    std::uint32_t applySprint(std::uint32_t out, std::uint32_t raw, bool on)
    {
        const bool held = (raw & kSprint) != 0;
        const bool rose = held && !m_sprintRawPrev;
        m_sprintRawPrev = held;
        if (m_sprintSwallow && !held) {
            m_sprintSwallow = false;
        }
        if (!on) {
            m_sprintLatched = false;
            m_sprintSwallow = false;
        } else if (rose) {
            if (m_sprintLatched) {
                m_sprintLatched = false;
                m_sprintSwallow = true;
            } else {
                m_sprintLatched = true;
            }
        }
        const bool show = m_sprintLatched || (held && !m_sprintSwallow);
        return show ? (out | kSprint) : (out & ~kSprint);
    }

    bool m_sneakLatched = false;
    bool m_sneakSwallow = false;
    bool m_sneakShown = false;
    bool m_sprintLatched = false;
    bool m_sprintSwallow = false;
    bool m_sprintRawPrev = false;
};

}
