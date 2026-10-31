#include "game/OreUiScript.h"

#include "core/Logger.h"
#include "core/Notice.h"
#include "game/OreUiPatch.h"

#include <Windows.h>

#include <libhat/process.hpp>
#include <libhat/scanner.hpp>
#include <libhat/signature.hpp>

#include <atomic>
#include <cstdint>
#include <format>
#include <string>

namespace tsukuyomi::oreuiscript {
namespace {

constexpr char kExecuteScriptSig[] =
    "48 89 74 24 10 48 89 7C 24 20 55 48 8D 6C 24 A9 48 81 EC A0 00 00 00 48 8B F2 48 8B F9 "
    "48 8B 89 28 1E 00 00 BA FF FF 01 00 E8";

constexpr char kAdvanceSig[] =
    "48 8B C4 F2 0F 11 48 10 55 53 56 57 41 54 41 56 41 57 48 8D 68 A1 48 81 EC D0 00 00 00 33 F6 "
    "0F 29 70 B8 89 75 67 48 8B F9 48 8B 89 28 1E 00 00";

constexpr char kModuleName[] = "cohtml.WindowsDesktop.dll";

constexpr char kVanillaKeyGroup[] = "keyboardAndMouse.inputGroup.standard";

using ExecuteScriptFn = void(__fastcall*)(void*, const char*);
using AdvanceFn = std::uintptr_t(__fastcall*)(void*, double);

ExecuteScriptFn g_executeScript = nullptr;
AdvanceFn g_advance = nullptr;

constexpr int kMaxViews = 16;
struct Done {
    void* view;
    void* context;
};
Done g_done[kMaxViews]{};
int g_doneCount = 0;
std::atomic<bool> g_full{false};

std::byte* scanCohtml(const char* signature)
{
    const auto module = hat::process::get_module(kModuleName);
    if (!module.has_value()) {
        return nullptr;
    }
    const auto text = module->get_section_data(".text");
    const auto parsed = hat::parse_signature(signature);
    if (text.empty() || !parsed.has_value()) {
        return nullptr;
    }
    const hat::signature_view view{parsed.value()};
    const auto first = hat::find_pattern(text.data(), text.data() + text.size(), view);
    if (!first.has_result()) {
        return nullptr;
    }
    std::byte* const at = const_cast<std::byte*>(first.get());
    const auto second = hat::find_pattern(at + 1, text.data() + text.size(), view);
    return second.has_result() ? nullptr : at;
}

std::string buildScript()
{
    std::string ids;
    const int count = oreui::ownGroupIdCount();
    for (int i = 0; i < count; ++i) {
        const char* const id = oreui::ownGroupId(i);
        if (id == nullptr) {
            continue;
        }
        if (!ids.empty()) {
            ids += ',';
        }
        ids += '"';
        ids += id;
        ids += '"';
    }
    return std::string("(function(){var src=\"") + kVanillaKeyGroup + "\",ids=[" + ids
        + "];ids.forEach(function(id){if(Object.prototype.hasOwnProperty(id))return;"
          "Object.defineProperty(Object.prototype,id,{configurable:true,enumerable:false,"
          "get:function(){var v=this[src];return typeof v===\"function\"?v:undefined;},"
          "set:function(v){Object.defineProperty(this,id,{value:v,writable:true,configurable:true,enumerable:true});}"
          "});});})();";
}

const std::string& script()
{
    static const std::string text = buildScript();
    return text;
}

void* readContext(void* view) noexcept
{
    __try {
        return *reinterpret_cast<void* const*>(static_cast<const char*>(view) + 0x108);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool executeGuarded(void* view, const char* text) noexcept
{
    __try {
        g_executeScript(view, text);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void runOnce(void* view)
{
    void* const context = readContext(view);
    if (context == nullptr) {
        return;
    }
    for (int i = 0; i < g_doneCount; ++i) {
        if (g_done[i].view == view && g_done[i].context == context) {
            return;
        }
    }
    if (g_doneCount >= kMaxViews) {
        if (!g_full.exchange(true)) {
            log().warn(L"OreUiScript: more than {} Ore UI views; the key-group script is not run on the rest", kMaxViews);
        }
        return;
    }
    g_done[g_doneCount++] = Done{view, context};
    const bool ok = executeGuarded(view, script().c_str());
    static std::atomic<int> told{0};
    if (told.fetch_add(1) < 8) {
        if (ok) {
            log().info(L"OreUiScript: ran the key-group script on view {} (thread {})", view, GetCurrentThreadId());
        } else {
            log().error(L"OreUiScript: the key-group script raised an exception on view {}", view);
        }
    }
}

std::uintptr_t __fastcall detourAdvance(void* view, double now)
{
    if (view != nullptr && g_executeScript != nullptr) {
        runOnce(view);
    }
    return g_advance(view, now);
}

}

void* advanceAddress()
{
    std::byte* const execute = scanCohtml(kExecuteScriptSig);
    std::byte* const advance = scanCohtml(kAdvanceSig);
    if (execute == nullptr || advance == nullptr) {
        notice::failOnce("OreUiScript.scan",
                         std::format(L"OreUiScript: cohtml View::{} was not found in {}; the key rows in the Tsukuyomi "
                                     L"settings tabs are drawn as plain groups",
                                     execute == nullptr ? L"ExecuteScript" : L"Advance", L"cohtml.WindowsDesktop.dll"),
                         "Key rows in the Tsukuyomi settings tabs look broken: the Ore UI script hook is unavailable");
        return nullptr;
    }
    g_executeScript = reinterpret_cast<ExecuteScriptFn>(execute);
    return advance;
}

void* advanceDetour()
{
    return reinterpret_cast<void*>(&detourAdvance);
}

void** advanceOriginal()
{
    return reinterpret_cast<void**>(&g_advance);
}

}
