#pragma once

#include <atomic>
#include <chrono>

#include "modules/Module.h"

namespace tsukuyomi {

class Scaffold : public Module {
public:
    static Scaffold& instance();

    const wchar_t* name() const override { return L"Scaffold"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void saveConfig(nlohmann::json& section) const override;

    void onClientTick();

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

    static constexpr int kMaxTargets = 5;

    bool readFeet(float& outX, float& outY, float& outZ) const;

    int buildTargets(const BlockPos& center, BlockPos (&targets)[kMaxTargets]) const;

    static unsigned char faceTowardCenter(const BlockPos& pos, const BlockPos& center);

    static bool alreadyFilled(const BlockPos& pos);

    void placeAll();

    static constexpr int kMinY = -64;
    static constexpr int kMaxY = 319;

    static constexpr int kLogIntervalMs = 1000;

    std::atomic<bool> m_resetRequested{false};
    BlockPos m_lastCenter;
    bool m_hasLastCenter = false;

    bool m_placing = false;

    bool m_warnedNoGameMode = false;

    static constexpr int kFeetMissNotice = 40;
    mutable int m_feetMisses = 0;

    Clock::time_point m_nextLog{};
};

}
