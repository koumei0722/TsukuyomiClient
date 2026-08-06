#pragma once

#include <chrono>
#include <vector>

#include "modules/Module.h"

namespace tsukuyomi {

class FastBlockPlacement : public Module {
public:
    static FastBlockPlacement& instance();

    const wchar_t* name() const override { return L"FastBlockPlacement"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    bool onBuildBlock(void* gameMode, void* blockPos, unsigned char face, unsigned char extra);
    void onPlayerViewUpdate();

    enum class Axis {
        X = 0,
        Y = 1,
        Z = 2,
    };

protected:
    void onEnabledChanged(bool enabled) override;

    bool persistEnabled() const override { return false; }

private:
    FastBlockPlacement() = default;

    using Clock = std::chrono::steady_clock;

    struct BlockPos {
        int x = 0;
        int y = 0;
        int z = 0;
    };

    const wchar_t* axisName() const;
    void cycleAxis();

    void placeRange();

    static BlockPos shiftByFace(BlockPos pos, unsigned char face);

    void faceCandidates(float stepX, float stepY, float stepZ, unsigned char (&out)[4]) const;

    static unsigned char faceForCell(const BlockPos& pos, const unsigned char (&candidates)[4],
                                     const BlockPos* targets, int count);

    bool alreadyPlaced(const BlockPos& pos) const;
    void rememberPlaced(const BlockPos& pos);

    static constexpr int kMinRange = 1;
    static constexpr int kMaxRange = 5;
    static constexpr int kDefaultRange = 5;

    static constexpr size_t kMaxPlacedMemory = 512;

    static constexpr int kHoldMs = 200;

    static constexpr int kResendIntervalMs = 100;

    static constexpr float kMinDirection = 1.0e-4f;

    static constexpr int kLogIntervalMs = 1000;

    Axis m_axis = Axis::Y;
    int m_range = kDefaultRange;

    static constexpr int kFirstStep = 0;

    void* m_gameMode = nullptr;
    BlockPos m_base;
    bool m_hasBase = false;

    bool m_placing = false;

    bool m_manualDone = false;

    std::vector<BlockPos> m_placed;

    Clock::time_point m_holdUntil{};
    Clock::time_point m_nextResend{};
    Clock::time_point m_nextLog{};
};

}
