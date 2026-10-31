#pragma once

#include "memory/Patch.h"
#include "modules/Module.h"

#include <vector>

namespace tsukuyomi {

class OverworldSky : public Module {
public:
    static OverworldSky& instance();

    const wchar_t* name() const override { return L"OverworldSky"; }
    bool available() const override { return m_ready; }

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    void onScansReady() override;
    void shutdown() override;

    bool wantsFogHook() const { return m_ready && enabled(); }
    void adjustFog(const void* settings, void* out) const;
    void adjustFogColor(const void* settings, void* out) const;

protected:
    void onEnabledChanged(bool enabled) override;

private:
    OverworldSky() = default;

    void readWeatherFields();

    std::vector<Patch> m_patches;
    bool m_ready = false;
    bool m_weatherReady = false;
};

}
