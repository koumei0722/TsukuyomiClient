#include "game/AllowDuplicateKeys.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "hooks/HookManager.h"
#include "memory/Scanner.h"

#include <atomic>
#include <cstdint>

namespace tsukuyomi::allowduplicatekeys {

namespace {

constexpr char kSection[] = "AllowDuplicateKeys";
constexpr int kSkipLogLimit = 20;

using UnassignFn = void(__fastcall*)(void*, std::uintptr_t);
UnassignFn g_unassign = nullptr;
UnassignFn g_unassignOthers = nullptr;
std::atomic<int> g_skipLogs{0};

void __fastcall detourUnassign(void* self, std::uintptr_t keptRow)
{
    (void)self;
    if (g_skipLogs.fetch_add(1, std::memory_order_relaxed) < kSkipLogLimit) {
        log().info(L"AllowDuplicateKeys: kept the other bindings of the key just bound (row {})", keptRow);
    }
}

void __fastcall detourUnassignOthers(void* self, std::uintptr_t keptRow)
{
    (void)self;
    if (g_skipLogs.fetch_add(1, std::memory_order_relaxed) < kSkipLogLimit) {
        log().info(L"AllowDuplicateKeys: kept the other bindings after resetting a binding (row {})", keptRow);
    }
}

}

void install()
{
    nlohmann::json& section = Config::instance().section(kSection);
    Config::keepOnly(section, {"enabled"});
    if (!Config::ensureBool(section, "enabled", true)) {
        log().info(L"AllowDuplicateKeys: disabled in Tsukuyomi.json");
        return;
    }
    void* const target = Scanner::instance().address(Target::KeyBindingUnassignConflicts);
    if (target == nullptr) {
        log().error(L"AllowDuplicateKeys: the function that unassigns duplicate keys was not found");
    } else if (HookManager::instance().create(target, reinterpret_cast<void*>(&detourUnassign),
                                              reinterpret_cast<void**>(&g_unassign), L"KeyBindingUnassignConflicts")) {
        log().info(L"AllowDuplicateKeys: binding a key no longer unassigns the other actions that use it");
    }
    void* const others = Scanner::instance().address(Target::KeyBindingUnassignOthers);
    if (others == nullptr) {
        log().error(L"AllowDuplicateKeys: the function that unassigns duplicate keys after a reset was not found");
    } else if (HookManager::instance().create(others, reinterpret_cast<void*>(&detourUnassignOthers),
                                              reinterpret_cast<void**>(&g_unassignOthers), L"KeyBindingUnassignOthers")) {
        log().info(L"AllowDuplicateKeys: resetting a binding no longer unassigns the other actions that share a key");
    }
}

}
