#pragma once

#include <chrono>

#include "modules/Module.h"

namespace tsukuyomi {

class Scaffold : public Module {
public:
    static Scaffold& instance();

    const wchar_t* name() const override { return L"Scaffold"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    void onPlayerViewUpdate();

    enum class Pattern {
        Cross = 0,
        Square3 = 1,
        Square5 = 2,
        Square7 = 3,
    };

    enum class Height {
        Follow = 0,
        Manual = 1,
        OnEnable = 2,
    };

protected:
    void onEnabledChanged(bool enabled) override;

    bool persistEnabled() const override { return false; }

private:
    Scaffold() = default;

    using Clock = std::chrono::steady_clock;

    struct BlockPos {
        int x = 0;
        int y = 0;
        int z = 0;
    };

    static constexpr int kMaxTargets = 49;

    const wchar_t* patternName() const;
    const wchar_t* heightName() const;
    void cyclePattern();
    void cycleHeight();

    void captureHeight();

    bool resolveY(float footY, int& outY) const;

    int squareRadius() const;

    int buildTargets(const BlockPos& center, BlockPos (&targets)[kMaxTargets]) const;

    static unsigned char faceTowardCenter(const BlockPos& pos, const BlockPos& center);

    void placeAll();

    static constexpr int kMinY = -64;
    static constexpr int kMaxY = 319;

    static constexpr int kResendIntervalMs = 100;

    static constexpr int kLogIntervalMs = 1000;

    Pattern m_pattern = Pattern::Cross;
    Height m_height = Height::Follow;
    int m_manualY = 64;

    int m_capturedY = 0;
    bool m_hasCapturedY = false;

    BlockPos m_lastCenter;
    bool m_hasLastCenter = false;

    bool m_placing = false;

    bool m_warnedNoGameMode = false;

    Clock::time_point m_nextResend{};
    Clock::time_point m_nextLog{};
};

}
