#pragma once

#include <atomic>

#include "modules/Module.h"

namespace tsukuyomi {

class FastInventory : public Module {
public:
    static FastInventory& instance();

    const wchar_t* name() const override { return L"FastInventory"; }
    bool available() const override;

    void onScansReady() override;

    void onInventoryOpenSent(void* client);

protected:
    bool persistEnabled() const override { return true; }

private:
    FastInventory() = default;
    std::atomic<bool> m_broken{false};
    std::atomic<bool> m_logged{false};
    std::atomic<bool> m_warned{false};

};

}
