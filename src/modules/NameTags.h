#pragma once

#include "memory/Patch.h"
#include "modules/Module.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace tsukuyomi {

class NameTags : public Module {
public:
    static NameTags& instance();

    const wchar_t* name() const override { return L"NameTags"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;
    bool writeBlocked() const override;

    void onScansReady() override;
    void shutdown() override;
    void onPlayerViewUpdate();

    using BuildTagFn = void*(__fastcall*)(void* a1, void* out, void* a3, void* a4, void* actor, const void* text,
                                          const float* position, float a8, const void* a9);
    void* onBuildTag(BuildTagFn original, void* a1, void* out, void* a3, void* a4, void* actor, const void* text,
                     const float* position, float a8, const void* a9);
    void setBuildHooked(bool hooked) { m_buildHooked.store(hooked, std::memory_order_release); }

    static constexpr std::size_t kPatchCount = 3;

protected:
    void onEnabledChanged(bool enabled) override;

private:
    NameTags() = default;

    void applyPatches();
    bool alwaysShowActive() const { return enabled() && m_alwaysShow.load(std::memory_order_relaxed); }
    bool platformActive() const { return enabled() && m_showPlatform.load(std::memory_order_relaxed); }
    void samplePlatforms();

    Patch m_patches[kPatchCount];

    std::atomic<bool> m_patchesReady{false};
    std::atomic<bool> m_alwaysShow{true};
    std::atomic<bool> m_showPlatform{true};

    struct Known {
        const void* actor;
        std::int32_t platform;
    };
    std::mutex m_knownMutex;
    std::vector<Known> m_known;
    std::atomic<bool> m_hasKnown{false};
    std::atomic<bool> m_buildHooked{false};
    unsigned long long m_lastSample = 0;
    bool m_sampleFailLogged = false;
    bool m_platformLogged = false;
};

}
