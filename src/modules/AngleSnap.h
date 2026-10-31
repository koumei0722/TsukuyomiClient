#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "input/Hotkey.h"
#include "modules/Module.h"

namespace tsukuyomi {

class AngleSnap : public Module {
public:
    static AngleSnap& instance();

    const wchar_t* name() const override { return L"AngleSnap"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    bool active() const { return m_mode.load(std::memory_order_acquire) != 0; }

    void afterTurn(float* look, const float before[3]);
    void afterOrbitTurn(float* orbit, const float before[2]);

protected:
    void onUpdate() override;

private:
    AngleSnap() = default;

    static constexpr float kMinStep = 1.0f;
    static constexpr float kMaxStep = 90.0f;
    static constexpr float kDefaultStep = 15.0f;

    Hotkey m_snapKey;
    Hotkey m_lockYawKey;
    Hotkey m_lockPitchKey;
    std::atomic<float> m_step{kDefaultStep};

    enum Axis : std::uint8_t { kFree = 0, kSnap = 1, kLock = 2 };
    std::atomic<std::uint8_t> m_mode{0};
    std::atomic<std::uint32_t> m_pressSerial{0};

    struct Angles {
        float yaw = 0.0f;
        float pitch = 0.0f;
    };
    struct Track {
        const float* component = nullptr;
        std::uint32_t serial = 0;
        bool seeded = false;
        Angles raw;
        Angles lock;
        Angles wrote;
    };
    Track& trackFor(const float* component);
    Angles snap(Track& track, Angles before, Angles now, float low, float high);

    Track m_tracks[8]{};
    std::size_t m_nextTrack = 0;
};

}
