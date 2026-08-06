#pragma once

#include <vector>

namespace tsukuyomi {

class HookManager {
public:
    static HookManager& instance();

    bool initialize();

    void shutdown();

    bool create(void* target, void* detour, void** original, const wchar_t* name);

    bool applyQueued();

    bool initialized() const { return m_initialized; }

private:
    HookManager() = default;

    bool m_initialized = false;
    std::vector<void*> m_targets;
};

}
