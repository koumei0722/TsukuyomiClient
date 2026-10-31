#pragma once

#include "memory/Patch.h"
#include "modules/Module.h"

#include <cstddef>
#include <vector>

namespace tsukuyomi {

class ShowInvisible : public Module {
public:
    static ShowInvisible& instance();

    const wchar_t* name() const override { return L"ShowInvisible"; }
    bool available() const override;

    bool writeBlocked() const override;
    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    void onScansReady() override;
    void shutdown() override;

    enum class Item {
        Barrier,
        Light,
        StructureVoid,
        Spectator,
        Count,
    };
    static constexpr std::size_t kItemCount = static_cast<std::size_t>(Item::Count);

protected:
    void onEnabledChanged(bool enabled) override;

private:
    ShowInvisible() = default;

    void applyPatches();
    void resolveLayer(std::size_t item);
    void resolveSpectator();
    bool itemFound(std::size_t item) const;

    bool m_show[kItemCount] = {true, true, true, true};
    std::vector<Patch> m_patches[kItemCount];
};

}
