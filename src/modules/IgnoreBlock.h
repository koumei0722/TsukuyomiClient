#pragma once

#include "input/Hotkey.h"
#include "modules/Module.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace tsukuyomi {

class IgnoreBlock : public Module {
public:
    static IgnoreBlock& instance();

    const wchar_t* name() const override { return L"IgnoreBlock"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;
    void onScansReady() override;

    bool onBuildBlock(const void* blockPos, bool& handled);
    void onPlayerViewUpdate();

private:
    IgnoreBlock() = default;

    Hotkey m_ignoreKey;
    int m_useButton = -1;
    std::atomic<bool> m_armed{true};
    std::mutex m_lock;
    std::vector<std::array<std::int32_t, 3>> m_pending;
};

}
