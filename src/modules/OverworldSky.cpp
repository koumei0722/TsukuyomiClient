#include "modules/OverworldSky.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Notice.h"
#include "game/SkyFogColor.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>

namespace tsukuyomi {

namespace {

constexpr std::size_t kDimensionIdSlot = 0x18 / sizeof(void*);
constexpr std::size_t kFogBrightnessSlot = 0xC0 / sizeof(void*);
constexpr std::size_t kTimeOfDaySlot = 0xF8 / sizeof(void*);
constexpr std::size_t kSunAngleSlot = 0x108 / sizeof(void*);

constexpr int kOverworld = 0;
constexpr int kNether = 1;
constexpr int kTheEnd = 2;

using DimensionIdFn = int* (*)(void* dimension, int* out);
using TimeOfDayFn = float (*)(void* dimension, int time, float alpha);
using SunAngleFn = float* (*)(void* dimension, float* out, float alpha);
using FogBrightnessFn = float* (*)(void* dimension, float* out, const float* color, float brightness);
using BiomeSkyColorFn = float* (*)(float* out, const void* biome, float temperature);

constexpr float kOverworldSkyColor[4] = {120.0f / 255.0f, 167.0f / 255.0f, 1.0f, 1.0f};
constexpr float kOverworldFogColor[4] = {171.0f / 255.0f, 210.0f / 255.0f, 1.0f, 1.0f};
constexpr float kOverworldFogStart = 0.92f;
constexpr float kOverworldFogEnd = 1.0f;
constexpr std::uint32_t kFogRenderDistance = 1;
constexpr std::uint32_t kFogKindAir = 0;
constexpr float kTwoPi = 6.2831855f;

TimeOfDayFn g_baseTimeOfDay = nullptr;
FogBrightnessFn g_overworldFogBrightness = nullptr;
SunAngleFn g_baseSunAngle = nullptr;
BiomeSkyColorFn g_biomeSkyColor = nullptr;
std::atomic<void*> g_dimensionIdFn{nullptr};
std::atomic<int> g_lastDimension{-1};
std::atomic<int> g_lastTime{-1};
std::atomic<float> g_lastAlpha{0.0f};

std::atomic<bool> g_netherEnd{true};

constexpr const wchar_t* kTimeLabels[] = {L"Game", L"Day", L"Noon", L"Sunset", L"Night", L"Midnight", L"Sunrise"};
constexpr int kTimeTicks[] = {-1, 1000, 6000, 12000, 13000, 18000, 23000};
constexpr int kTimeCount = static_cast<int>(std::size(kTimeLabels));
static_assert(std::size(kTimeTicks) == std::size(kTimeLabels));
std::atomic<int> g_time{0};

constexpr const wchar_t* kWeatherLabels[] = {L"Game", L"Clear", L"Rain", L"Thunder"};
constexpr int kWeatherGame = 0;
constexpr int kWeatherClear = 1;
constexpr int kWeatherThunder = 3;
constexpr int kWeatherCount = static_cast<int>(std::size(kWeatherLabels));
std::atomic<int> g_weather{kWeatherGame};

int clampChoice(int value, int count)
{
    return value < 0 || value >= count ? 0 : value;
}

struct WeatherFields {
    std::uint32_t inDimension = 0;
    std::uint8_t dimension = 0;
    std::uint8_t oldRain = 0, rain = 0, oldLightning = 0, lightning = 0;
};
WeatherFields g_weatherFields;

void** vtableOf(void* object)
{
    return *static_cast<void***>(object);
}

bool isNetherOrEnd(int dimension)
{
    return dimension == kNether || dimension == kTheEnd;
}

int dimensionIdOf(void* dimension)
{
    void* const known = g_dimensionIdFn.load(std::memory_order_relaxed);
    if (known == nullptr || vtableOf(dimension)[kDimensionIdSlot] != known) {
        return -1;
    }
    int id = kOverworld;
    reinterpret_cast<DimensionIdFn>(known)(dimension, &id);
    return id;
}

bool drawsAsOverworld(void* dimension)
{
    return g_netherEnd.load(std::memory_order_relaxed) && isNetherOrEnd(dimensionIdOf(dimension));
}

int fixedTime()
{
    return kTimeTicks[clampChoice(g_time.load(std::memory_order_relaxed), kTimeCount)];
}

void applyWeather(void* dimension)
{
    const int mode = clampChoice(g_weather.load(std::memory_order_relaxed), kWeatherCount);
    const WeatherFields& f = g_weatherFields;
    if (mode == kWeatherGame || f.inDimension == 0 || dimensionIdOf(dimension) < 0) {
        return;
    }
    unsigned char* weather = nullptr;
    if (!memory::copyGuarded(static_cast<unsigned char*>(dimension) + f.inDimension, &weather, sizeof(weather))
        || !memory::plausiblePointer(weather)) {
        return;
    }
    void* owner = nullptr;
    if (!memory::copyGuarded(weather + f.dimension, &owner, sizeof(owner)) || owner != dimension) {
        return;
    }
    const float rain = mode == kWeatherClear ? 0.0f : 1.0f;
    const float lightning = mode == kWeatherThunder ? 1.0f : 0.0f;
    memory::writeGuarded(weather + f.oldRain, &rain, sizeof(rain));
    memory::writeGuarded(weather + f.rain, &rain, sizeof(rain));
    memory::writeGuarded(weather + f.oldLightning, &lightning, sizeof(lightning));
    memory::writeGuarded(weather + f.lightning, &lightning, sizeof(lightning));
}

int* dimensionIdAsOverworld(void* dimension, int* out)
{
    void* const original = vtableOf(dimension)[kDimensionIdSlot];
    g_dimensionIdFn.store(original, std::memory_order_relaxed);
    int* const result = reinterpret_cast<DimensionIdFn>(original)(dimension, out);
    if (out != nullptr) {
        g_lastDimension.store(*out, std::memory_order_relaxed);
        if (isNetherOrEnd(*out) && g_netherEnd.load(std::memory_order_relaxed)) {
            *out = kOverworld;
        }
    }
    return result;
}

float timeOfDayAsOverworld(void* dimension, int time, float alpha)
{
    applyWeather(dimension);
    const int fixed = fixedTime();
    if (fixed >= 0) {
        time = fixed;
        alpha = 0.0f;
    }
    g_lastTime.store(time, std::memory_order_relaxed);
    g_lastAlpha.store(alpha, std::memory_order_relaxed);
    if (drawsAsOverworld(dimension)) {
        return g_baseTimeOfDay(dimension, time, alpha);
    }
    return reinterpret_cast<TimeOfDayFn>(vtableOf(dimension)[kTimeOfDaySlot])(dimension, time, alpha);
}

float* sunAngleAsOverworld(void* dimension, float* out, float alpha)
{
    if (!drawsAsOverworld(dimension)) {
        float* const result =
            reinterpret_cast<SunAngleFn>(vtableOf(dimension)[kSunAngleSlot])(dimension, out, alpha);
        const int fixed = fixedTime();
        if (out != nullptr && fixed >= 0 && dimensionIdOf(dimension) == kOverworld) {
            out[0] = g_baseTimeOfDay(dimension, fixed, 0.0f) * kTwoPi;
        }
        return result;
    }
    float* const result = g_baseSunAngle(dimension, out, alpha);
    const int fixed = fixedTime();
    const int time = fixed >= 0 ? fixed : g_lastTime.load(std::memory_order_relaxed);
    const float timeAlpha = fixed >= 0 ? 0.0f : g_lastAlpha.load(std::memory_order_relaxed);
    if (out != nullptr && time >= 0) {
        out[0] = g_baseTimeOfDay(dimension, time, timeAlpha) * kTwoPi;
    }
    return result;
}

float* fogBrightnessAsOverworld(void* dimension, float* out, const float* color, float brightness)
{
    if (drawsAsOverworld(dimension)) {
        return g_overworldFogBrightness(dimension, out, color, brightness);
    }
    return reinterpret_cast<FogBrightnessFn>(vtableOf(dimension)[kFogBrightnessSlot])(dimension, out, color,
                                                                                      brightness);
}

float* biomeSkyColorAsOverworld(float* out, const void* biome, float temperature)
{
    float* const result = g_biomeSkyColor(out, biome, temperature);
    if (out != nullptr && g_netherEnd.load(std::memory_order_relaxed)
        && g_lastDimension.load(std::memory_order_relaxed) == kTheEnd && out[0] == 0.0f
        && out[1] == 0.0f && out[2] == 0.0f) {
        std::memcpy(out, kOverworldSkyColor, sizeof(kOverworldSkyColor));
    }
    return result;
}

struct Calls {
    std::vector<std::byte*> dimensionId;
    std::vector<std::byte*> timeOfDay;
    std::vector<std::byte*> fogBrightness;
    std::vector<std::byte*> sunAngle;
};

bool hasDimensionLoad(const unsigned char* from, const unsigned char* to)
{
    for (const unsigned char* at = from; at + 6 <= to; ++at) {
        if (at[0] != 0x8B || at[1] < 0x80 || at[1] > 0xBF) {
            continue;
        }
        const unsigned char* disp = at + 2;
        if (*disp == 0x24) {
            ++disp;
        }
        if (disp + 4 <= to && disp[0] == 0x88 && disp[1] == 0x09 && disp[2] == 0x00 && disp[3] == 0x00) {
            return true;
        }
    }
    return false;
}

const unsigned char* findIndirectCall(const unsigned char* from, std::size_t within, const unsigned char* end)
{
    for (const unsigned char* at = from; at < from + within && at + 6 <= end; ++at) {
        if (at[0] == 0xFF && at[1] == 0x15) {
            return at;
        }
    }
    return nullptr;
}

Calls collectCalls(std::byte* begin, std::byte* end)
{
    static constexpr unsigned char kLoadVtable[] = {0x48, 0x8B, 0x01, 0x48, 0x8B};
    static constexpr unsigned char kTimeLoad[] = {0x4C, 0x8B, 0x81, 0xF8, 0x00, 0x00, 0x00};
    static constexpr unsigned char kTimeCall[] = {0x4C, 0x89, 0xC0, 0xFF, 0x15};
    const auto* const b = reinterpret_cast<const unsigned char*>(begin);
    const auto* const e = reinterpret_cast<const unsigned char*>(end);
    const auto out = [&](const unsigned char* at) { return begin + (at - b); };
    Calls calls;
    for (const unsigned char* at = b; at + 12 <= e; ++at) {
        if (std::memcmp(at, kLoadVtable, sizeof(kLoadVtable)) == 0) {
            const unsigned char* const slot = at + sizeof(kLoadVtable);
            if (slot[0] == 0x40 && slot[1] == 0x18) {
                const unsigned char* const call = findIndirectCall(slot + 2, 8, e);
                if (call != nullptr && hasDimensionLoad(at > b + 32 ? at - 32 : b, at)) {
                    calls.dimensionId.push_back(out(call));
                }
            } else if (slot[0] == 0x80 && slot[1] == 0xC0 && slot[2] == 0 && slot[3] == 0 && slot[4] == 0) {
                const unsigned char* const call = findIndirectCall(slot + 5, 16, e);
                if (call != nullptr && hasDimensionLoad(at > b + 16 ? at - 16 : b, at)) {
                    calls.fogBrightness.push_back(out(call));
                }
            } else if (slot[0] == 0x80 && slot[1] == 0x08 && slot[2] == 0x01 && slot[3] == 0 && slot[4] == 0) {
                const unsigned char* const call = findIndirectCall(slot + 5, 16, e);
                if (call != nullptr && hasDimensionLoad(at > b + 32 ? at - 32 : b, at)) {
                    calls.sunAngle.push_back(out(call));
                }
            }
        }
        if (std::memcmp(at, kTimeLoad, sizeof(kTimeLoad)) == 0) {
            for (const unsigned char* call = at + sizeof(kTimeLoad);
                 call < at + sizeof(kTimeLoad) + 16 && call + sizeof(kTimeCall) <= e; ++call) {
                if (std::memcmp(call, kTimeCall, sizeof(kTimeCall)) == 0) {
                    calls.timeOfDay.push_back(out(call + 3));
                    break;
                }
            }
        }
    }
    return calls;
}

bool functionRange(const std::byte* at, std::byte*& begin, std::byte*& end)
{
    ULONG64 imageBase = 0;
    const auto* const entry = RtlLookupFunctionEntry(reinterpret_cast<ULONG64>(at), &imageBase, nullptr);
    if (entry == nullptr || imageBase == 0) {
        return false;
    }
    begin = reinterpret_cast<std::byte*>(imageBase + entry->BeginAddress);
    end = reinterpret_cast<std::byte*>(imageBase + entry->EndAddress);
    return memory::isReadable(begin, static_cast<std::size_t>(end - begin));
}

void collectDirectCallees(std::byte* begin, std::byte* end, std::vector<std::byte*>& callees)
{
    for (std::byte* at = begin; at + 5 <= end; ++at) {
        if (*at != std::byte{0xE8}) {
            continue;
        }
        std::int32_t rel = 0;
        std::memcpy(&rel, at + 1, sizeof(rel));
        std::byte* const target = at + 5 + rel;
        if (!memory::inGameModule(target)) {
            continue;
        }
        std::byte* fb = nullptr;
        std::byte* fe = nullptr;
        if (!functionRange(target, fb, fe) || fb != target) {
            continue;
        }
        bool known = false;
        for (std::byte* c : callees) {
            known = known || c == target;
        }
        if (!known) {
            callees.push_back(target);
        }
    }
}

const void* indirectCallCell(const std::byte* call)
{
    std::int32_t disp = 0;
    std::memcpy(&disp, call + 2, sizeof(disp));
    return call + 6 + disp;
}

}

OverworldSky& OverworldSky::instance()
{
    static OverworldSky module;
    return module;
}

void OverworldSky::onScansReady()
{
    Scanner& scanner = Scanner::instance();
    std::byte* const descriptionSite = scanner.address(Target::SkyDescriptionDimensionCall);
    std::byte* const stateSite = scanner.address(Target::SkyStateEndCheckCalls);
    std::byte* const skyColorSite = scanner.address(Target::SkyColorBiomeCall);
    std::byte* const fogBrightnessSite = scanner.address(Target::SkyFogBrightnessCall);
    std::byte* const renderSite = scanner.address(Target::NameTagStageCaller);
    std::byte* const fogDistanceSite = scanner.address(Target::FogColorDistanceClamp);
    g_baseTimeOfDay = reinterpret_cast<TimeOfDayFn>(scanner.address(Target::DimensionTimeOfDayBase));
    g_overworldFogBrightness = reinterpret_cast<FogBrightnessFn>(scanner.address(Target::OverworldFogBrightness));
    g_baseSunAngle = reinterpret_cast<SunAngleFn>(scanner.address(Target::DimensionSunAngleBase));
    if (g_baseSunAngle == nullptr || descriptionSite == nullptr || stateSite == nullptr || skyColorSite == nullptr || fogBrightnessSite == nullptr
        || renderSite == nullptr || fogDistanceSite == nullptr || g_baseTimeOfDay == nullptr
        || g_overworldFogBrightness == nullptr || scanner.address(Target::FogColorFetch) == nullptr) {
        return;
    }

    const auto fail = [](const std::wstring& why) {
        notice::failOnce("OverworldSky.verify", L"OverworldSky: " + why + L"; OverworldSky is off",
                         "OverworldSky is off: the sky code did not match");
    };

    struct Function {
        const wchar_t* name;
        std::byte* site;
        std::size_t dimensionId, timeOfDay, fogBrightness, sunAngle;
        bool redirectSunAngle;
    };
    const Function functions[] = {
        {L"sky state", stateSite, 4, 5, 0, 0, false},
        {L"fog color", fogBrightnessSite, 0, 2, 1, 0, false},
        {L"render", renderSite, 5, 0, 0, 1, true},
        {L"fog distance", fogDistanceSite, 2, 0, 0, 1, false},
    };
    Calls all;
    std::vector<std::byte*> mainFunctions;
    std::vector<std::byte*> callees;
    for (const Function& function : functions) {
        std::byte* begin = nullptr;
        std::byte* end = nullptr;
        if (!functionRange(function.site, begin, end)) {
            fail(std::wstring(L"the ") + function.name + L" function has no unwind entry");
            return;
        }
        mainFunctions.push_back(begin);
        collectDirectCallees(begin, end, callees);
        Calls calls = collectCalls(begin, end);
        if (calls.dimensionId.size() != function.dimensionId || calls.timeOfDay.size() != function.timeOfDay
            || calls.fogBrightness.size() != function.fogBrightness || calls.sunAngle.size() != function.sunAngle) {
            log().warn(L"OverworldSky: {} has {} getDimensionId / {} getTimeOfDay / {} fog brightness / {} sun angle calls "
                       L"(want {} / {} / {} / {})",
                       function.name, calls.dimensionId.size(), calls.timeOfDay.size(), calls.fogBrightness.size(),
                       calls.sunAngle.size(), function.dimensionId, function.timeOfDay, function.fogBrightness,
                       function.sunAngle);
            fail(std::wstring(L"the calls in the ") + function.name + L" function did not match");
            return;
        }
        all.dimensionId.insert(all.dimensionId.end(), calls.dimensionId.begin(), calls.dimensionId.end());
        all.timeOfDay.insert(all.timeOfDay.end(), calls.timeOfDay.begin(), calls.timeOfDay.end());
        all.fogBrightness.insert(all.fogBrightness.end(), calls.fogBrightness.begin(), calls.fogBrightness.end());
        if (function.redirectSunAngle) {
            all.sunAngle.insert(all.sunAngle.end(), calls.sunAngle.begin(), calls.sunAngle.end());
        }
    }
    constexpr std::size_t kHelpers = 8, kHelperTime = 8, kHelperDimensionId = 4;
    std::size_t helpers = 0, helperTime = 0, helperDimensionId = 0;
    for (std::byte* callee : callees) {
        bool isMain = false;
        for (std::byte* main : mainFunctions) {
            isMain = isMain || main == callee;
        }
        std::byte* begin = nullptr;
        std::byte* end = nullptr;
        if (isMain || !functionRange(callee, begin, end)) {
            continue;
        }
        Calls calls = collectCalls(begin, end);
        if (calls.timeOfDay.empty()) {
            continue;
        }
        ++helpers;
        helperTime += calls.timeOfDay.size();
        helperDimensionId += calls.dimensionId.size();
        all.timeOfDay.insert(all.timeOfDay.end(), calls.timeOfDay.begin(), calls.timeOfDay.end());
        all.dimensionId.insert(all.dimensionId.end(), calls.dimensionId.begin(), calls.dimensionId.end());
    }
    if (helpers != kHelpers || helperTime != kHelperTime || helperDimensionId != kHelperDimensionId) {
        log().warn(L"OverworldSky: {} helper functions with {} getTimeOfDay / {} getDimensionId calls (want {} / {} / {})",
                   helpers, helperTime, helperDimensionId, kHelpers, kHelperTime, kHelperDimensionId);
        fail(L"the helper functions of the sky did not match");
        return;
    }
    const auto contains = [&](std::byte* call) {
        for (std::byte* at : all.dimensionId) {
            if (at == call) {
                return true;
            }
        }
        return false;
    };
    if (!contains(stateSite + 21) || !contains(stateSite + 63)) {
        fail(L"the End checks of the sky state were not among the calls");
        return;
    }
    all.dimensionId.push_back(descriptionSite + 21);

    const void* const cell = indirectCallCell(descriptionSite + 21);
    for (const auto* list : {&all.dimensionId, &all.timeOfDay, &all.fogBrightness, &all.sunAngle}) {
        for (std::byte* call : *list) {
            if (call[0] != std::byte{0xFF} || call[1] != std::byte{0x15} || indirectCallCell(call) != cell) {
                fail(L"a sky call is not the usual indirect call");
                return;
            }
        }
    }

    std::vector<Patch> patches;
    const auto redirect = [&](const std::vector<std::byte*>& list, const void* replacement, const char* name) {
        for (std::byte* call : list) {
            patches.push_back(makeIndirectCallPatch(call, replacement, name));
        }
    };
    redirect(all.dimensionId, reinterpret_cast<const void*>(&dimensionIdAsOverworld), "OverworldSky.dimension");
    redirect(all.timeOfDay, reinterpret_cast<const void*>(&timeOfDayAsOverworld), "OverworldSky.timeOfDay");
    redirect(all.sunAngle, reinterpret_cast<const void*>(&sunAngleAsOverworld), "OverworldSky.timeOfDay");
    redirect(all.fogBrightness, reinterpret_cast<const void*>(&fogBrightnessAsOverworld), "OverworldSky.fog");
    void* biomeSkyColor = nullptr;
    patches.push_back(makeDirectCallPatch(skyColorSite + 25, reinterpret_cast<const void*>(&biomeSkyColorAsOverworld),
                                          &biomeSkyColor, "OverworldSky.skyColor"));
    if (biomeSkyColor == nullptr || !memory::inGameModule(biomeSkyColor)) {
        fail(L"the biome sky color call did not point into the game");
        return;
    }
    g_biomeSkyColor = reinterpret_cast<BiomeSkyColorFn>(biomeSkyColor);
    for (const Patch& patch : patches) {
        if (!patch.valid()) {
            log().warn(L"OverworldSky: a call could not be redirected (disabled in hooks.json or no page near the game)");
            return;
        }
    }
    m_patches = std::move(patches);
    m_ready = true;
    readWeatherFields();
    if (enabled()) {
        onEnabledChanged(true);
    }
    log().info(L"OverworldSky: {} sky calls found ({}; weather {})", m_patches.size(), enabled() ? L"on" : L"off",
               m_weatherReady ? L"ready" : L"unavailable");
}

void OverworldSky::readWeatherFields()
{
    Scanner& scanner = Scanner::instance();
    const auto* const tick = reinterpret_cast<const std::uint8_t*>(scanner.address(Target::WeatherTickLevels));
    const auto* const darken = reinterpret_cast<const std::uint8_t*>(scanner.address(Target::SkyDarkenWeatherLoad));
    if (tick == nullptr || darken == nullptr) {
        return;
    }
    WeatherFields fields;
    std::memcpy(&fields.inDimension, darken + 3, sizeof(fields.inDimension));
    fields.dimension = tick[3];
    fields.rain = tick[27];
    fields.lightning = tick[32];
    fields.oldRain = tick[37];
    fields.oldLightning = tick[42];
    if (darken[43] != fields.dimension || fields.inDimension == 0 || fields.inDimension >= 0x10000
        || fields.oldRain + 4 != fields.rain || fields.oldLightning + 4 != fields.lightning
        || fields.rain >= fields.dimension || fields.lightning >= fields.dimension) {
        log().warn(L"OverworldSky: weather fields did not match (dimension +{:#x}, weather +{:#x}/+{:#x}, "
                   L"rain +{:#x}/+{:#x}, lightning +{:#x}/+{:#x})",
                   fields.inDimension, fields.dimension, darken[43], fields.oldRain, fields.rain, fields.oldLightning,
                   fields.lightning);
        notice::failOnce("OverworldSky.weather", L"OverworldSky: the weather fields did not match; Weather is off",
                         "OverworldSky Weather is off: the weather code did not match");
        return;
    }
    g_weatherFields = fields;
    m_weatherReady = true;
}

MenuItem OverworldSky::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    children.push_back(menu::toggle(
        L"Nether and End", [] { return g_netherEnd.load(std::memory_order_relaxed); },
        [] { g_netherEnd.store(!g_netherEnd.load(std::memory_order_relaxed), std::memory_order_relaxed); }));
    children.push_back(menu::choice(
        L"Time", std::vector<std::wstring>(std::begin(kTimeLabels), std::end(kTimeLabels)),
        [] { return clampChoice(g_time.load(std::memory_order_relaxed), kTimeCount); },
        [](int value) { g_time.store(clampChoice(value, kTimeCount), std::memory_order_relaxed); }));
    MenuItem weather = menu::choice(
        L"Weather", std::vector<std::wstring>(std::begin(kWeatherLabels), std::end(kWeatherLabels)),
        [] { return clampChoice(g_weather.load(std::memory_order_relaxed), kWeatherCount); },
        [](int value) { g_weather.store(clampChoice(value, kWeatherCount), std::memory_order_relaxed); });
    weather.available = [this] { return m_weatherReady; };
    children.push_back(std::move(weather));

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void OverworldSky::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    g_netherEnd.store(Config::getBool(section, "netherEnd", true), std::memory_order_relaxed);
    g_time.store(clampChoice(Config::getInt(section, "time", 0), kTimeCount), std::memory_order_relaxed);
    g_weather.store(clampChoice(Config::getInt(section, "weather", kWeatherGame), kWeatherCount),
                    std::memory_order_relaxed);
}

void OverworldSky::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["netherEnd"] = g_netherEnd.load(std::memory_order_relaxed);
    section["time"] = clampChoice(g_time.load(std::memory_order_relaxed), kTimeCount);
    section["weather"] = clampChoice(g_weather.load(std::memory_order_relaxed), kWeatherCount);
}

void OverworldSky::adjustFog(const void* settings, void* out) const
{
    if (!m_ready || !enabled() || settings == nullptr || out == nullptr || !g_netherEnd.load(std::memory_order_relaxed)
        || !isNetherOrEnd(g_lastDimension.load(std::memory_order_relaxed))) {
        return;
    }
    std::uint32_t kind = 0;
    if (!memory::copyGuarded(static_cast<const unsigned char*>(settings) + 8, &kind, sizeof(kind))) return;
    auto* const bytes = static_cast<unsigned char*>(out);
    unsigned char present = 0;
    if (kind != kFogKindAir || !memory::copyGuarded(bytes + 0x1c, &present, sizeof(present)) || present != 1) {
        return;
    }
    memory::writeGuarded(bytes, kOverworldFogColor, sizeof(kOverworldFogColor));
    memory::writeGuarded(bytes + 0x10, &kOverworldFogStart, sizeof(kOverworldFogStart));
    memory::writeGuarded(bytes + 0x14, &kOverworldFogEnd, sizeof(kOverworldFogEnd));
    memory::writeGuarded(bytes + 0x18, &kFogRenderDistance, sizeof(kFogRenderDistance));
}

void OverworldSky::adjustFogColor(const void* settings, void* out) const
{
    if (settings == nullptr || out == nullptr) return;
    const bool active = m_ready && enabled() && g_netherEnd.load(std::memory_order_relaxed);
    if (!active) return;
    std::uint32_t kind = 0;
    unsigned char present = 0;
    if (!memory::copyGuarded(static_cast<const unsigned char*>(settings) + 8, &kind, sizeof(kind))
        || !memory::copyGuarded(static_cast<const unsigned char*>(out) + 0xc, &present, sizeof(present))) return;
    float rgb[3]{};
    if (skyfog::overworldColor(active, g_lastDimension.load(std::memory_order_relaxed), kind, present == 1, rgb)) {
        memory::writeGuarded(out, rgb, sizeof(rgb));
    }
}

void OverworldSky::onEnabledChanged(bool enabled)
{
    if (!m_ready) {
        return;
    }
    std::vector<Patch*> all;
    all.reserve(m_patches.size());
    for (Patch& patch : m_patches) {
        all.push_back(&patch);
    }
    Patch::setAll(all, enabled);
}

void OverworldSky::shutdown()
{
    for (Patch& patch : m_patches) {
        patch.restore();
    }
}

}
