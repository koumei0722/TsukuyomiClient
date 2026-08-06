#pragma once

#include <atomic>

#include "game/GameModeIds.h"
#include "input/Hotkey.h"
#include "modules/Module.h"

namespace tsukuyomi {

class GameModeSwitch : public Module {
public:
    static GameModeSwitch& instance();

    const wchar_t* name() const override { return L"GameModeSwitch"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;
    void onScansReady() override;

    void onSetGameMode(void* self, int mode, int extra);

    void onPlayerViewUpdate();

protected:
    void onUpdate() override;

private:
    GameModeSwitch() = default;

    static constexpr int kSurvival = gamemode::kSurvival;
    static constexpr int kCreative = gamemode::kCreative;
    static constexpr int kAdventure = gamemode::kAdventure;
    static constexpr int kSpectator = gamemode::kSpectator;
    static constexpr int kUnknown = gamemode::kUnknown;

    enum class Request {
        None = 0,
        Switch,
        Spectator,
        Commit,
    };

    int switchTarget() const;
    int spectatorTarget() const;

    static int nextInCycle(int mode);

    static bool modifiersDown(const Hotkey& key);

    bool hasTarget();

    void request(Request wanted);
    void applyRequest(Request wanted);
    void apply(int mode);

    int countTargets() const;

    void publishSelection() const;

    static const wchar_t* modeName(int mode);

    Hotkey m_switchKey;
    Hotkey m_spectatorKey;

    std::atomic<void*> m_self{nullptr};
    std::atomic<void*> m_selfAlt{nullptr};
    std::atomic<int> m_extra{0};
    std::atomic<int> m_current{kUnknown};
    std::atomic<int> m_previous{kUnknown};
    std::atomic<Request> m_request{Request::None};

    std::atomic<bool> m_selecting{false};
    std::atomic<int> m_selected{kUnknown};

    std::atomic<long long> m_selfCapturedMs{0};
    std::atomic<long long> m_selfAltCapturedMs{0};

    std::atomic<long long> m_lastViewMs{0};

    static constexpr long long kViewGapMs = 1500;

    bool m_hadEnabledSetting = false;

    bool m_warnedNoSelf = false;

    bool m_warnedUnusualMode = false;
};

}
