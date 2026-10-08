#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "modules/Module.h"

namespace tsukuyomi {

class DeathLogger : public Module {
public:
    static DeathLogger& instance();

    const wchar_t* name() const override { return L"DeathLogger"; }
    bool available() const override { return m_healthAttribute.load(std::memory_order_relaxed) != nullptr; }
    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;
    void onScansReady() override;

    void onPlayerViewUpdate();

protected:
    void onUpdate() override;

private:
    DeathLogger() = default;

    std::string resolveDimension();

    std::atomic<const void*> m_healthAttribute{nullptr};
    std::atomic<bool> m_writeToFile{false};

    unsigned long long m_serial = 0;
    float m_lastHealth = -1.0f;
    bool m_readFailLogged = false;
    bool m_dimensionFailLogged = false;

    std::mutex m_pendingMutex;
    std::vector<std::string> m_pending;
    bool m_writeFailLogged = false;
};

}
