#include "hooks/HookManager.h"

#include "core/Logger.h"

#include <MinHook.h>

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

}

HookManager& HookManager::instance()
{
    static HookManager manager;
    return manager;
}

bool HookManager::initialize()
{
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

bool HookManager::create(void* target, void* detour, void** original, const wchar_t* name)
{
    if (!m_initialized) {
        log().error(L"Cannot hook {} (MinHook not initialized)", name);
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

    status = MH_QueueEnableHook(target);
    if (status != MH_OK) {
        log().error(L"Failed to queue hook for {}: {}", name, statusText(status));
        MH_RemoveHook(target);
        return false;
    }

    m_targets.push_back(target);
    log().info(L"{} prepared", name);
    return true;
}

bool HookManager::applyQueued()
{
    if (!m_initialized) {
        return false;
    }

    const MH_STATUS status = MH_ApplyQueued();
    if (status != MH_OK) {
        log().error(L"Failed to enable hooks: {}", statusText(status));
        return false;
    }

    log().success(L"{} hooks enabled", m_targets.size());
    return true;
}

void HookManager::shutdown()
{
    if (!m_initialized) {
        return;
    }

    for (void* target : m_targets) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }
    m_targets.clear();

    const MH_STATUS status = MH_Uninitialize();
    if (status != MH_OK) {
        log().warn(L"MinHook shutdown failed: {}", statusText(status));
    }

    m_initialized = false;
}

}
