#pragma once

#include <chrono>
#include <cstddef>

#include "modules/Module.h"

namespace tsukuyomi {

class FastUseItem : public Module {
public:
    static FastUseItem& instance();

    const wchar_t* name() const override { return L"FastUseItem"; }
    bool available() const override;
    void onScansReady() override;
    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;

    void saveConfig(nlohmann::json& section) const override;

    int onUseItem(void* gameMode, void* itemStack, int extra);

    int onUseItemTransaction(void* gameMode, void* itemStack, int extra);

protected:

private:
    FastUseItem() = default;

    using Clock = std::chrono::steady_clock;

    bool shouldRepeat(void* gameMode) const;

    static bool playerSneaking(void* gameMode);

    static constexpr std::size_t kGameModePlayerOffset = 0x08;
    static constexpr std::size_t kPlayerContextOffset = 0x08;

    void noteExtra(int extra);

    static constexpr int kUsesPerBurst = 64;

    static constexpr int kLogIntervalMs = 1000;

    static constexpr std::size_t kItemStackCountOffset = 0x22;

    bool m_repeating = false;
    int m_useButton = -1;

    bool m_inTransaction = false;
    bool m_sneakOnly = true;

    int m_extraSinceLog = 0;
    Clock::time_point m_nextLog{};

    mutable bool m_notSneakingLogged = false;
    mutable bool m_missingSneakingLogged = false;
};

}
