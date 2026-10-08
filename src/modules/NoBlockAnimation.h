#pragma once

#include "memory/Patch.h"
#include "modules/Module.h"

namespace tsukuyomi {

class NoBlockAnimation : public Module {
public:
    static NoBlockAnimation& instance();

    const wchar_t* name() const override { return L"NoBlockAnimation"; }
    bool available() const override { return m_patch.valid(); }

    void onScansReady() override;
    void shutdown() override;

protected:
    void onEnabledChanged(bool enabled) override;

private:
    NoBlockAnimation() = default;

    Patch m_patch;
};

}
