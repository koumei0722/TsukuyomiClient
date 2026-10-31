#include "game/CustomGuiScale.h"

#include "config/Config.h"
#include "config/WriteSwitches.h"
#include "core/Logger.h"
#include "game/CustomGuiScaleLogic.h"
#include "game/UiProbe.h"
#include "hooks/Detours.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace tsukuyomi::customguiscale {

ComputeFn originalCompute = nullptr;
OptionListFn originalList = nullptr;
OptionGetFn originalGet = nullptr;
OptionSetFn originalSet = nullptr;

namespace {
constexpr char kSection[] = "CustomGuiScale";
using IndexFn = int(__fastcall*)(void*, const float*);
IndexFn g_default = nullptr;
IndexFn g_maximum = nullptr;
const float* g_table = nullptr;
void** g_providerVtable = nullptr;
std::atomic<bool> g_ready{false};
std::atomic<bool> g_custom{false};
std::atomic<bool> g_stopping{false};
std::atomic<bool> g_refresh{false};
std::atomic<bool> g_restored{false};
std::atomic<int> g_percent{100};
std::atomic<float> g_base{0};
std::atomic<float> g_max{0};
std::atomic<float> g_min{0};
ULONGLONG g_lastRefresh = 0;

bool readScaleBounds(void* self, const float* a2, const float* a3, const float* a4,
                     float& base, float& maximum, float& minimum)
{
    __try {
        const float size[2] = {a2[0] * a3[0] * a4[2], a2[1] * a3[1] * a4[3]};
        const int d = g_default(self, size);
        const int mx = g_maximum(self, size);
        if (mx < 0 || mx > 7) return false;
        base = g_table[defaultIndex(d, mx)];
        maximum = g_table[mx];
        minimum = g_table[0];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void range(int& lo, int& hi)
{
    percentRange(g_base.load(), g_max.load(), g_min.load(), lo, hi);
}

std::atomic<bool> g_configDirty{false};

void rememberCustom(bool value)
{
    if (g_custom.exchange(value) != value) {
        g_configDirty.store(true);
    }
}
}

bool active() { return g_ready.load() && !g_stopping.load(); }
bool isCustom() { return active() && g_custom.load(); }

bool prepareHooks()
{
    auto& section = Config::instance().section(kSection);
    Config::keepOnly(section, {"enabled", "custom", "percent"});
    const bool enabled = Config::ensureBool(section, "enabled", true);
    g_custom.store(Config::ensureBool(section, "custom", false));
    const float saved = Config::getFloat(section, "percent", 100);
    const int percent = std::isfinite(saved)
        && static_cast<double>(saved) >= (std::numeric_limits<int>::min)()
        && static_cast<double>(saved) <= (std::numeric_limits<int>::max)()
        ? static_cast<int>(saved) : 100;
    g_percent.store(percent);
    if (!section.contains("percent") || !section["percent"].is_number() || !std::isfinite(saved))
        section["percent"] = percent;
    if (!enabled || writes::blocked("CustomGuiScale")) return false;

    const auto* const site = Scanner::instance().address(Target::GuiScaleCompute);
    const auto* const provider = Scanner::instance().address(Target::GuiScaleProviderVtableSite);
    if (site == nullptr || provider == nullptr || !memory::isReadable(site, 0xF8)
        || !memory::isReadable(provider, 0x2A)) return false;
    g_default = reinterpret_cast<IndexFn>(memory::ripTarget(site, 0x83));
    g_maximum = reinterpret_cast<IndexFn>(memory::ripTarget(site, 0x90));
    g_table = static_cast<const float*>(memory::ripTarget(site, 0xA6));
    g_providerVtable = static_cast<void**>(memory::ripTarget(provider, 0x15));
    if (!memory::isExecutable(reinterpret_cast<const void*>(g_default), 1)
        || !memory::isExecutable(reinterpret_cast<const void*>(g_maximum), 1)
        || !memory::isReadable(g_table, 8 * sizeof(float))
        || !memory::isReadable(g_providerVtable, 6 * sizeof(void*))) return false;
    const void* const refresh = Scanner::instance().address(Target::ClientRefreshScreenSize);
    return refresh != nullptr && memory::isExecutable(refresh, 1);
}

void* providerFunction(std::size_t slot)
{
    if (slot < 3 || slot > 5 || !memory::isReadable(g_providerVtable, 6 * sizeof(void*))) return nullptr;
    void* const fn = g_providerVtable[slot];
    return memory::inGameModule(fn) && memory::isExecutable(fn, 1) ? fn : nullptr;
}

void setHooksReady(bool ready) { g_ready.store(ready); }

float __fastcall detourCompute(void* self, const float* a2, const float* a3, const float* a4)
{
    const float vanilla = originalCompute(self, a2, a3, a4);
    if (!active()) return vanilla;
    float base = 0, maximum = 0, minimum = 0;
    if (!readScaleBounds(self, a2, a3, a4, base, maximum, minimum)
        || !std::isfinite(base) || base <= 0 || !std::isfinite(maximum)
        || !std::isfinite(minimum) || minimum <= 0 || maximum < minimum) return vanilla;
    const bool changed = g_base.exchange(base) != base || g_max.load() != maximum || g_min.load() != minimum;
    g_max.store(maximum);
    g_min.store(minimum);
    if (changed) uiprobe::requestGuiScalePublish();
    if (!isCustom()) return vanilla;
    int lo = 0, hi = 0;
    percentRange(base, maximum, minimum, lo, hi);
    return customScale(base, maximum, minimum, clampPercent(g_percent.load(), lo, hi));
}

OptionSpan* __fastcall detourList(void* self, OptionSpan* out)
{
    OptionSpan* const result = originalList(self, out);
    if (active() && result != nullptr && memory::isWritable(result, sizeof(*result)))
        uiprobe::appendCustomGuiScaleOption(result->count, result->first);
    return result;
}

int __fastcall detourGet(void* self)
{
    return isCustom() ? kCustomValue : originalGet(self);
}

void __fastcall detourSet(void* self, int value)
{
    if (!active()) {
        originalSet(self, value);
        return;
    }
    rememberCustom(value == kCustomValue);
    if (value != kCustomValue) originalSet(self, value);
    requestRefresh();
    uiprobe::markSettingsDirty();
    uiprobe::requestGuiScalePublish();
}

bool isCustomComponentId(const std::string& id)
{
    return id == "video.ui_scale_modifier" || id == "accessibility.ui_scale_modifier";
}

MenuItem sliderItem()
{
    auto item = menu::number(L"Custom scale", [] {
        int lo = 0, hi = 0;
        range(lo, hi);
        return static_cast<float>(clampPercent(g_percent.load(), lo, hi));
    }, [](float value) {
        if (!isCustom() || !std::isfinite(value)) return;
        int lo = 0, hi = 0;
        range(lo, hi);
        const int percent = static_cast<int>(std::clamp(static_cast<double>(std::round(value)),
                                                        static_cast<double>(lo), static_cast<double>(hi)));
        if (g_percent.exchange(percent) == percent) return;
        g_configDirty.store(true);
        requestRefresh();
        uiprobe::markSettingsDirty();
        uiprobe::requestGuiScalePublish();
    }, true, 50, 200);
    item.available = [] { return isCustom(); };
    item.numberRange = [](float& lo, float& hi) {
        int lower = 0, upper = 0;
        range(lower, upper);
        lo = static_cast<float>(lower);
        hi = static_cast<float>(upper);
        return true;
    };
    return item;
}

void requestRefresh() { g_refresh.store(true); }

void update()
{
    if (!g_configDirty.exchange(false)) return;
    auto& section = Config::instance().section(kSection);
    section["custom"] = g_custom.load();
    section["percent"] = g_percent.load();
    uiprobe::markSettingsDirty();
}

void start()
{
    if (!g_ready.load()) return;
    if (!writes::allowed("RenderCurrentFrame"))
        log().warn(L"CustomGuiScale: RenderCurrentFrame is disabled; scale refresh waits for a window resize");
    if (g_custom.load()) requestRefresh();
}

void onFrame()
{
    if (!g_ready.load() || !g_refresh.load()) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastRefresh < 100) return;
    void* const client = hooks::gameClientInstance();
    if (client == nullptr) return;
    if (!g_refresh.exchange(false)) return;
    const bool stopping = g_stopping.load();
    if (!hooks::callClientRefreshScreenSize(client)) {
        requestRefresh();
        return;
    }
    g_lastRefresh = now;
    if (stopping) g_restored.store(true);
}

void shutdown()
{
    if (!g_ready.load()) return;
    g_stopping.store(true);
    if (!g_custom.load()) return;
    g_restored.store(false);
    requestRefresh();
    const ULONGLONG deadline = GetTickCount64() + 1000;
    while (!g_restored.load() && GetTickCount64() < deadline) Sleep(1);
    if (!g_restored.load())
        log().warn(L"CustomGuiScale: unload refresh did not run within 1 s; vanilla scale returns on window resize");
}

}
