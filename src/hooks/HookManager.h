#pragma once

#include <array>
#include <mutex>
#include <vector>

namespace tsukuyomi {

enum class HookGroup : int {
    Always = 0,
    Ghost,
    Diag,
    Fullbright,
    Ability,
    Tool,
    Fog,
    Particles,
    Count
};

class HookManager {
public:
    static HookManager& instance();

    bool initialize();

    void shutdown();

    bool create(void* target, void* detour, void** original, const wchar_t* name,
                HookGroup group = HookGroup::Always);

    bool applyQueued();

    bool setGroupEnabled(HookGroup group, bool on);
    bool groupEnabled(HookGroup group) const;

    std::size_t groupSize(HookGroup group) const;

    bool initialized() const { return m_initialized; }

private:
    HookManager() = default;

    struct Entry {
        void* target = nullptr;
        HookGroup group = HookGroup::Always;
    };

    bool m_initialized = false;
    std::vector<Entry> m_entries;
    std::array<bool, static_cast<std::size_t>(HookGroup::Count)> m_groupOn{true};
    mutable std::recursive_mutex m_lock;
};

}
