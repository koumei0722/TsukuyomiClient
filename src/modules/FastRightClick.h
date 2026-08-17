#pragma once

#include <chrono>
#include <cstddef>

#include "modules/Module.h"

namespace tsukuyomi {

class FastRightClick : public Module {
public:
    static FastRightClick& instance();

    const wchar_t* name() const override { return L"FastRightClick"; }
    bool available() const override;

    void saveConfig(nlohmann::json& section) const override;

    int onUseItem(void* gameMode, void* itemStack);

    int onUseItemTransaction(void* gameMode, void* itemStack);

protected:

    bool persistEnabled() const override { return false; }

private:
    FastRightClick() = default;

    using Clock = std::chrono::steady_clock;

    bool shouldRepeat() const;

    void noteExtra(int extra);

    static constexpr int kUsesPerBurst = 64;

    static constexpr int kLogIntervalMs = 1000;

    static constexpr std::size_t kItemStackCountOffset = 0x22;

    bool m_repeating = false;

    bool m_inTransaction = false;

    int m_extraSinceLog = 0;
    Clock::time_point m_nextLog{};
};

}
