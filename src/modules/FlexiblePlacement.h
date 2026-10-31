#pragma once

#include <Windows.h>

#include "game/FlexibleLogic.h"
#include "game/LookPlacement.h"
#include "modules/Module.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tsukuyomi {
class FlexiblePlacement : public Module {
public:
    static FlexiblePlacement& instance();
    const wchar_t* name() const override { return L"FlexiblePlacement"; }
    bool available() const override;
    void onScansReady() override;
    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;
    void shutdown() override;
    bool onBuildBlock(void* gameMode, void* blockPos, unsigned char face, unsigned char extra,
                      bool simTick, bool& handled);
    void onPlayerViewUpdate();
protected:
    void onEnabledChanged(bool enabled) override;
private:
    FlexiblePlacement() = default;
    flexible::Keys pressedKeys() const;
    enum class Solved { Found, Impossible, Unsupported, Failed };
    Solved solve(const flexible::Plan& plan, const float rel[3], flexible::Look real, const std::string& item,
                 lookplace::Request& request);
    void notify(const std::string& key, const std::string& message);
    std::mutex m_noticeMutex;
    std::vector<std::string> m_notices;
    std::unordered_map<std::string, std::uint64_t> m_noticeAt;
    Hotkey m_rotationKey{{VK_LMENU}}, m_offsetKey{{VK_LCONTROL}};
    Hotkey m_adjacentKey, m_accurateInKey, m_accurateReverseKey;
    std::atomic<bool> m_gridOverlay{true};
    std::atomic<bool> m_linesVisible{false};
    void clearLines();
    std::atomic<bool> m_queryFailed{false};
};
}
