#include "hooks/HookManager.h"

#include "config/WriteSwitches.h"
#include "core/Logger.h"
#include "core/Perf.h"
#include "core/Strings.h"

#include <MinHook.h>

#include <algorithm>
#include <string>
#include <vector>

namespace tsukuyomi {

namespace {

const wchar_t* statusText(MH_STATUS status)
{
    switch (status) {
    case MH_OK:                       return L"OK";
    case MH_ERROR_ALREADY_INITIALIZED:return L"already initialized";
    case MH_ERROR_NOT_INITIALIZED:    return L"not initialized";
    case MH_ERROR_ALREADY_CREATED:    return L"already created";
    case MH_ERROR_NOT_CREATED:        return L"not created";
    case MH_ERROR_ENABLED:            return L"already enabled";
    case MH_ERROR_DISABLED:           return L"already disabled";
    case MH_ERROR_NOT_EXECUTABLE:     return L"not executable memory";
    case MH_ERROR_UNSUPPORTED_FUNCTION: return L"unsupported instructions";
    case MH_ERROR_MEMORY_ALLOC:       return L"memory allocation failed";
    case MH_ERROR_MEMORY_PROTECT:     return L"memory protection change failed";
    default:                          return L"unknown error";
    }
}

const wchar_t* groupName(HookGroup group)
{
    switch (group) {
    case HookGroup::Always:     return L"always";
    case HookGroup::Ghost:      return L"ghost";
    case HookGroup::Diag:       return L"diag";
    case HookGroup::Fullbright: return L"fullbright";
    case HookGroup::Ability:    return L"ability";
    case HookGroup::Tool:       return L"tool";
    case HookGroup::Fog:        return L"fog";
    case HookGroup::Particles:  return L"particles";
    default:                    return L"?";
    }
}

}

HookManager& HookManager::instance()
{
    static HookManager manager;
    return manager;
}

bool HookManager::initialize()
{
    const std::lock_guard<std::recursive_mutex> guard(m_lock);
    if (m_initialized) {
        return true;
    }

    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        log().error(L"MinHook initialization failed: {}", statusText(status));
        return false;
    }

    m_initialized = true;
    return true;
}

bool HookManager::create(void* target, void* detour, void** original, const wchar_t* name,
                         HookGroup group)
{
    const std::lock_guard<std::recursive_mutex> guard(m_lock);
    if (!m_initialized) {
        log().error(L"Cannot hook {} (MinHook not initialized)", name);
        return false;
    }
    writes::noteUnknown(toUtf8(name));
    if (!writes::allowed(name)) {
        static std::vector<std::wstring> told;
        if (std::find(told.begin(), told.end(), name) == told.end()) {
            told.emplace_back(name);
            log().info(L"{} is turned off in hooks.json; not hooked", name);
        }
        return false;
    }
    if (target == nullptr) {
        log().warn(L"Skipping hook for {} (address not found)", name);
        return false;
    }

    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        log().error(L"Failed to create hook for {}: {}", name, statusText(status));
        return false;
    }

    const bool wantNow = m_groupOn[static_cast<std::size_t>(group)];
    if (wantNow) {
        status = MH_QueueEnableHook(target);
        if (status != MH_OK) {
            log().error(L"Failed to queue hook for {}: {}", name, statusText(status));
            MH_RemoveHook(target);
            return false;
        }
    }

    m_entries.push_back(Entry{target, group});
    if (group == HookGroup::Always) {
        log().info(L"{} prepared", name);
    } else {
        log().info(L"{} prepared ({}{})", name, groupName(group), wantNow ? L", on" : L", off");
    }
    return true;
}

bool HookManager::applyQueued()
{
    const std::lock_guard<std::recursive_mutex> guard(m_lock);
    if (!m_initialized) {
        return false;
    }

    MH_STATUS status = MH_OK;
    {
        const perf::Scope perfScope{perf::Slot::HookApply};
        status = MH_ApplyQueued();
    }
    if (status != MH_OK) {
        log().error(L"Failed to enable hooks: {}", statusText(status));
        return false;
    }

    std::size_t on = 0;
    for (const Entry& entry : m_entries) {
        if (m_groupOn[static_cast<std::size_t>(entry.group)]) {
            ++on;
        }
    }
    log().success(L"{} hooks enabled ({} prepared)", on, m_entries.size());
    return true;
}

bool HookManager::groupEnabled(HookGroup group) const
{
    const std::lock_guard<std::recursive_mutex> guard(m_lock);
    return m_groupOn[static_cast<std::size_t>(group)];
}

bool HookManager::setGroupEnabled(HookGroup group, bool on)
{
    const std::lock_guard<std::recursive_mutex> guard(m_lock);
    if (!m_initialized || group == HookGroup::Always) {
        return false;
    }
    const auto index = static_cast<std::size_t>(group);
    if (m_groupOn[index] == on) {
        return true;
    }
    m_groupOn[index] = on;

    std::size_t queued = 0;
    for (const Entry& entry : m_entries) {
        if (entry.group != group) {
            continue;
        }
        const MH_STATUS status =
            on ? MH_QueueEnableHook(entry.target) : MH_QueueDisableHook(entry.target);
        if (status != MH_OK) {
            log().warn(L"Could not queue a {} hook ({})", groupName(group), statusText(status));
            continue;
        }
        ++queued;
    }
    MH_STATUS status = MH_OK;
    {
        const perf::Scope perfScope{perf::Slot::HookApply};
        status = MH_ApplyQueued();
    }
    if (status != MH_OK) {
        log().error(L"Failed to switch the {} hooks {}: {}", groupName(group),
                    on ? L"on" : L"off", statusText(status));
        return false;
    }
    log().info(L"Hooks: {} {} ({} hooks)", groupName(group), on ? L"on" : L"off", queued);
    return true;
}

void HookManager::shutdown()
{
    const std::lock_guard<std::recursive_mutex> guard(m_lock);
    if (!m_initialized) {
        return;
    }

    if (const MH_STATUS status = MH_DisableHook(MH_ALL_HOOKS); status != MH_OK) {
        log().warn(L"MinHook: disabling all hooks failed: {}", statusText(status));
    }
    for (const Entry& entry : m_entries) {
        MH_RemoveHook(entry.target);
    }
    m_entries.clear();

    const MH_STATUS status = MH_Uninitialize();
    if (status != MH_OK) {
        log().warn(L"MinHook shutdown failed: {}", statusText(status));
    }

    m_initialized = false;
}

}
