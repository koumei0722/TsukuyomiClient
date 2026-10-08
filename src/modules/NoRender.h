#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "memory/Patch.h"
#include "modules/Module.h"

namespace tsukuyomi {

class NoRender : public Module {
public:
    static NoRender& instance();

    const wchar_t* name() const override { return L"NoRender"; }
    bool available() const override;
    bool writeBlocked() const override;
    void applyWriteBlock() override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    void onScansReady() override;
    void shutdown() override;

    enum class Stage {
        Terrain = 0,
        BlockEntities,
        Entities,
        Items,
        Sky,
        Fog,
        Particles,
        Weather,
        NameTags,
        Shadows,
        Cursor,
        Count,
    };

    static constexpr std::size_t kStageCount = static_cast<std::size_t>(Stage::Count);

    bool fogSuppressed() const
    {
        return enabled() && m_off[static_cast<std::size_t>(Stage::Fog)]
               && !m_stageBlocked[static_cast<std::size_t>(Stage::Fog)];
    }

    bool particlesSuppressed() const
    {
        return enabled() && m_off[static_cast<std::size_t>(Stage::Particles)]
               && !m_stageBlocked[static_cast<std::size_t>(Stage::Particles)];
    }

    void noteStageBlocks();

protected:
    void onEnabledChanged(bool enabled) override;

    enum class OptionSlot {
        Terrain = 0,
        Entities,
        BlockEntities,
        Particles,
        Sky,
        Weather,
        Count,
    };

    static constexpr std::size_t kOptionCount = static_cast<std::size_t>(OptionSlot::Count);

private:
    NoRender() = default;

    void resolveOptions();

    void resolveStages();

    void resolveItems();

    void applyStages();

    bool m_off[kStageCount] = {};
    bool m_stageBlocked[kStageCount] = {};
    std::vector<Patch> m_patches[kStageCount];

    bool m_byOption[kStageCount] = {};
    static bool isOptionStage(int stage);

    int m_found = 0;

    static constexpr const char* kZonePrefix = "Level renderer camera - Render";

    static constexpr std::ptrdiff_t kColdPathSpan = 0x100;

    static constexpr std::ptrdiff_t kMainSpan = 0x200;
};

}
