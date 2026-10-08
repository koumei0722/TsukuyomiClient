#pragma once

#include <atomic>

#include "modules/Module.h"

namespace tsukuyomi {

class OffhandSlot : public Module {
public:
    static OffhandSlot& instance();

    const wchar_t* name() const override { return L"OffhandSlot"; }
    bool available() const override { return m_definitionRegistered.load(std::memory_order_relaxed); }

    void onScansReady() override;
    void shutdown() override;

    static void* hudManager();
    static const void* offhandStackFor(const void* collectionName, int index);

protected:
    void onUpdate() override;
    void onEnabledChanged(bool enabled) override;

private:
    OffhandSlot() = default;

    void publish();
    static bool offhandHasItem();
    bool wanted() const;

    static void onHudCreated(void* ctrl);

    std::atomic<bool> m_definitionRegistered{false};
    std::atomic<bool> m_shuttingDown{false};
};

}
