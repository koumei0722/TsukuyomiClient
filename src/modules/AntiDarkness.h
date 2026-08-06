#pragma once

#include "memory/Patch.h"
#include "modules/Module.h"

namespace tsukuyomi {

class AntiDarkness : public Module {
public:
    static AntiDarkness& instance();

    const wchar_t* name() const override { return L"AntiDarkness"; }
    bool available() const override;

    void onScansReady() override;
    void shutdown() override;

protected:
    void onEnabledChanged(bool enabled) override;

private:
    AntiDarkness() = default;

    static constexpr size_t kPatchOffset = 0x0B;
    static constexpr size_t kPatchSize = 4;

    Patch m_patch;
};

}
