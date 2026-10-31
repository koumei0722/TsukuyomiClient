#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/LightLevelLogic.h"
#include "modules/Module.h"

namespace tsukuyomi {

class LightOverlay : public Module {
public:
    static LightOverlay& instance();

    const wchar_t* name() const override { return L"LightOverlay"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;
    void shutdown() override;

    void onPlayerViewUpdate();

protected:
    void onEnabledChanged(bool enabled) override;

private:
    LightOverlay() = default;

    struct Cell {
        int x = 0, y = 0, z = 0;
        int light = 0;
        lightlevel::Spawn spawn = lightlevel::Spawn::Never;
    };

    static constexpr int kDefaultRange = 16;
    static constexpr int kMinRange = 4;
    static constexpr int kMaxRange = 32;
    static constexpr int kDefaultVertical = 8;
    static constexpr int kMinVertical = 2;
    static constexpr int kMaxVertical = 16;
    static constexpr std::uint64_t kScanIntervalMs = 250;
    static constexpr std::size_t kMaxCells = 2000;

    void scan(int feetX, int feetY, int feetZ);
    void rebuildFills(int facing);
    void clear();
    static constexpr int kBlocksMotion = 1;
    static constexpr int kLiquid = 2;
    int materialOf(const void* block);
    int passable(const void* block, const void* extra);
    int spawnableFloor(const void* block, const std::int32_t pos[3]);

    std::atomic<int> m_range{kDefaultRange};
    std::atomic<int> m_vertical{kDefaultVertical};

    std::vector<Cell> m_cells;
    std::uint64_t m_lastScan = 0;
    std::uint64_t m_generation = ~0ULL;
    int m_lastFeet[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
    int m_facing = -1;
    bool m_drawn = false;
    bool m_broken = false;
    bool m_loggedCap = false;
    static constexpr int kMaxSummaryLogs = 5;
    static constexpr std::size_t kMaxRejectedNames = 8;
    int m_summaryLogs = 0;
    bool m_summaryDone = false;
    std::vector<std::string> m_rejectedNames;
    std::wstring m_lastSummary;
    std::unordered_map<const void*, std::int8_t> m_material;
    std::unordered_map<const void*, std::int8_t> m_spawnable;
};

}
