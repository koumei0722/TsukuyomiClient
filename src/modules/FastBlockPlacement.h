#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "input/Hotkey.h"
#include "modules/FastBlockPlacementLogic.h"
#include "modules/Module.h"

namespace tsukuyomi {

class FastBlockPlacement : public Module {
public:
    static FastBlockPlacement& instance();

    const wchar_t* name() const override { return L"FastBlockPlacement"; }
    bool available() const override;
    void onScansReady() override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    bool onBuildBlock(void* gameMode, void* blockPos, unsigned char face, unsigned char extra,
                      bool simTick);
    void onClientTick();
    bool wantsPick() const;
    void onPicked();
    bool suppressHotbarSlot(int slot) const;

protected:
    void onEnabledChanged(bool enabled) override;
    void onUpdate() override;

private:
    FastBlockPlacement() = default;

    struct Hold {
        bool active = false;
        void* gameMode = nullptr;
        std::int32_t first[3]{};
        int sideFirst = 1;
        float clickInFirst[3]{};
        int horizontalFirst = 3;
        std::int32_t last[3]{};
        int placed = 0;
        int scheduled = 0;
        bool saidUsed = false;
        bool saidRotation = false;
    };

    static bool readAim(std::int32_t pos[3], int& face, float rel[3]);
    static bool placementCell(void* gameMode, const std::int32_t pos[3], int face, std::int32_t out[3]);
    static bool vacant(void* gameMode, const std::int32_t pos[3]);
    static bool usedByClick(void* gameMode, const std::int32_t pos[3], int face, const float click[3], bool sneaking);

    void startHold(void* gameMode, const std::int32_t pos[3], int face, bool buildOver);
    void endHold(const wchar_t* why);
    bool placeOnce();

    void applyReset();
    std::atomic<bool> m_resetRequested{false};

    bool handReady(void* gameMode) const;

    std::atomic<int> m_restriction{static_cast<int>(fastplace::Restriction::Face)};
    std::atomic<int> m_layerHeight{1};
    std::atomic<bool> m_rememberOrientation{true};
    static constexpr int kPlacementsPerTick = 16;
    static const std::array<std::vector<int>, fastplace::kRestrictionCount>& defaultModeKeys();
    std::array<Hotkey, fastplace::kRestrictionCount> m_modeKeys{
        Hotkey{{'Z', '1'}}, Hotkey{{'Z', '2'}}, Hotkey{{'Z', '3'}},
        Hotkey{{'Z', '4'}}, Hotkey{{'Z', '6'}}, Hotkey{{'Z', '5'}}};

    int m_useButton = -1;
    Hold m_hold;
    int m_budget = 0;
    int m_maxPerTick = 0;
    std::atomic<unsigned long> m_gameThread{0};
    std::atomic<std::uint64_t> m_holdId{0};
    int m_holdLogs = 0;
    static constexpr int kMaxHoldLogs = 64;
};

}
