#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "game/ContainerUi.h"

namespace tsukuyomi {

class MaterialCollector : public containerui::Listener {
public:
    static MaterialCollector& instance();

    void onScansReady();
    void shutdown();

    void loadConfig(const nlohmann::json& section);
    void saveConfig(nlohmann::json& section) const;
    void watchConfigFile();

    bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }
    void toggle() {
        const bool next=!m_enabled.load();
        if (!next) m_stopRequested.store(true);
        m_enabled.store(next);
    }

    bool onSlotButton(std::uint32_t, int, const std::string&, int) override { return false; }
    void onScreenTick() override;
    void onScreenLost() override;
    void onScreenCreated(void* ctrl) override;

private:
    MaterialCollector() = default;

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_closeGui{true};
    std::atomic<int> m_extraAmount{0};
    std::atomic<bool> m_roundUpToStack{false};
    std::atomic<bool> m_requireSufficientSupply{false};
    std::atomic<bool> m_retainItem{false};
    std::atomic<int> m_retainItemAmount{1};
    std::atomic<bool> m_takeShulkerBoxes{false};
    std::atomic<float> m_shulkerBoxFillThreshold{1.0F};
    void readSettings(const nlohmann::json& section);
    long long m_cfgStamp = 0;
    unsigned long long m_cfgCheckMs = 0;
    bool m_cfgWarned = false;

    enum class Phase { Idle, Waiting, Running, Closing, Finished };
    Phase m_phase = Phase::Idle;
    void* m_ctrl = nullptr;
    unsigned long long m_createdAt = 0;
    unsigned long long m_closingSince = 0;
    int m_clicks = 0;

    struct Want {
        std::string name;
        long long listNeed = 0;
        long long target = 0;
        int maxStack = 0;
        long long taken = 0;
        bool stopped = false;
        bool skipped = false;
    };
    std::vector<Want> m_wants;
    std::size_t m_wantAt = 0;
    int m_stage = 0;
    int m_looseNext = 0;
    struct Box {
        int index = 0;
        int amount = 0;
    };
    std::vector<Box> m_boxes;
    long long m_looseAmount = 0;

    enum class When { Always, CursorEmpty, CursorHoldsWant, CursorNotEmpty };
    struct Press {
        std::string coll;
        int index = -1;
        containerui::Click kind = containerui::Click::Left;
        When when = When::Always;
    };
    struct Op {
        bool active = false;
        bool box = false;
        int boxAmount = 0;
        int index = -1;
        int before = 0;
        std::deque<Press> presses;
        bool aborted = false;
    };
    Op m_op;
    unsigned m_said = 0;

    void reset();
    void prepare();
    void run();
    bool advanceOp();
    bool nextOp();
    void finish();
    void report();
    bool pendingPredictions() const;
    std::vector<std::pair<std::string, int>> playerSlots() const;
    void countAvailable(std::map<std::string, long long>& out) const;
    bool hasContents(const void* stack) const;
    void say(const std::string& chat);
};

}
