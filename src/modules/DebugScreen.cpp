#include "modules/DebugScreen.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Strings.h"
#include "core/Version.h"
#include "game/ContainerUi.h"
#include "game/DebugScreenWorld.h"
#include "game/GameData.h"
#include "game/GameOptions.h"
#include "game/GameModeIds.h"
#include "game/GameVersion.h"
#include "game/LevelDbIndex.h"
#include "game/ServerPlayerFinder.h"
#include "game/SystemInfo.h"
#include "game/UiProbe.h"
#include "game/UiSound.h"
#include "input/Foreground.h"
#include "modules/DebugKeys.h"
#include "game/GameModeState.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "render/FrameTrace.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi {

namespace {

namespace cui = containerui;
namespace ds = dbgscreen;

void writeBool(int slot, bool value)
{
    if (volatile std::uint8_t* const p = cui::persistentBool(slot)) {
        *p = value ? 1 : 0;
    }
}

ds::ElementState clampState(int value)
{
    if (value <= static_cast<int>(ds::ElementState::Off)) {
        return ds::ElementState::Off;
    }
    if (value >= static_cast<int>(ds::ElementState::Always)) {
        return ds::ElementState::Always;
    }
    return ds::ElementState::InOverlay;
}

using ds::indexOfElement;
constexpr int kGpuElement = indexOfElement("gpu_utilization");
static_assert(kGpuElement >= 0);
constexpr int kMemoryElement = indexOfElement("memory");
static_assert(kMemoryElement >= 0);

constexpr int kPlayerSpeedElement = indexOfElement("player_speed");
constexpr int kChunkSourceStatsElement = indexOfElement("chunk_source_stats");
constexpr int kLookingAtBlockStateElement = indexOfElement("looking_at_block_state");
constexpr int kLookingAtBlockTagsElement = indexOfElement("looking_at_block_tags");

constexpr int kSoundMoodElement = indexOfElement("sound_mood");

constexpr int kSoundCacheElement = indexOfElement("sound_cache");
static_assert(kSoundCacheElement >= 0);
constexpr int kParticleRenderStatsElement = indexOfElement("particle_render_stats");
constexpr int kBiomeElement = indexOfElement("biome");
constexpr int kChunkRenderStatsElement = indexOfElement("chunk_render_stats");
constexpr int kEntityRenderStatsElement = indexOfElement("entity_render_stats");
constexpr int kLookingAtEntityElement = indexOfElement("looking_at_entity");
constexpr int kLookingAtEntityTagsElement = indexOfElement("looking_at_entity_tags");
constexpr int kLookingAtFluidStateElement = indexOfElement("looking_at_fluid_state");
constexpr int kLookingAtFluidTagsElement = indexOfElement("looking_at_fluid_tags");
constexpr int kLocalDifficultyElement = indexOfElement("local_difficulty");
constexpr int kChunkGenerationStatsElement = indexOfElement("chunk_generation_stats");
static_assert(kChunkGenerationStatsElement >= 0);
constexpr int kPlayerPositionElement = indexOfElement("player_position");
static_assert(kLocalDifficultyElement >= 0 && kPlayerPositionElement >= 0);
static_assert(kPlayerSpeedElement >= 0 && kChunkSourceStatsElement >= 0
                  && kLookingAtBlockStateElement >= 0 && kLookingAtBlockTagsElement >= 0
                  && kSoundMoodElement >= 0 && kParticleRenderStatsElement >= 0 && kBiomeElement >= 0
                  && kChunkRenderStatsElement >= 0 && kEntityRenderStatsElement >= 0
                  && kLookingAtEntityElement >= 0 && kLookingAtEntityTagsElement >= 0
                   && kLookingAtFluidStateElement >= 0 && kLookingAtFluidTagsElement >= 0);

constexpr std::uint64_t kSoundFreshMs = 3000;
constexpr std::uint64_t kParticleFreshMs = 1000;

std::atomic<int> g_bindLogs{0};
constexpr int kMaxBindLogs = 4;

void formatVersions(char (&shortName)[32], char (&longName)[32])
{
    shortName[0] = '\0';
    longName[0] = '\0';
    const wchar_t* const running = gameVersion::running();
    if (running == nullptr || running[0] == L'\0') {
        return;
    }
    int parts[4] = {0, 0, 0, 0};
    int count = 0;
    for (const wchar_t* p = running; *p != L'\0' && count < 4; ++p) {
        if (*p >= L'0' && *p <= L'9') {
            parts[count] = parts[count] * 10 + (*p - L'0');
        } else if (*p == L'.') {
            ++count;
        } else {
            break;
        }
    }
    count = count < 3 ? count + 1 : 4;
    if (count >= 3) {
        std::snprintf(shortName, sizeof(shortName), "%d.%d.%d", parts[0], parts[1], parts[2]);
    }
    if (count >= 4) {

        std::snprintf(longName, sizeof(longName), "%d.%d.%d.%02d", parts[0], parts[1], parts[2], parts[3]);
    } else if (count >= 3) {
        std::snprintf(longName, sizeof(longName), "%s", shortName);
    }
}

bool readRenderedActors(const std::int32_t* at, std::int32_t& out)
{
    if (at == nullptr) {
        return false;
    }
    __try {
        out = *at;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                   : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return out >= 0 && out < 100000;
}

void setText(dbgtext::TextValue& out, const char* text)
{
    if (text == nullptr || text[0] == '\0') {
        out.available = false;
        return;
    }
    out.available = true;
    std::snprintf(out.value, sizeof(out.value), "%s", text);
}

}

std::atomic<std::uint64_t> DebugScreen::s_frames{0};
std::atomic<std::uint64_t> DebugScreen::s_serverTicks{0};
std::atomic<std::uint64_t> DebugScreen::s_serverTickQpc{0};
std::atomic<std::uint64_t> DebugScreen::s_packetsSent{0};
std::atomic<std::uint64_t> DebugScreen::s_packetsReceived{0};
std::atomic<std::uint64_t> DebugScreen::s_chunkTicks{0};
std::atomic<bool> DebugScreen::s_chunkTickHookReady{false};
std::atomic<std::uint64_t> DebugScreen::s_startGameEntry{0};
std::atomic<std::uint64_t> DebugScreen::s_versionEntry{0};
std::atomic<char> DebugScreen::s_serverVersion[32]{};
std::atomic<void*> DebugScreen::s_serverLevel{nullptr};
std::atomic<std::uint64_t> DebugScreen::s_lastServerTick{0};
std::atomic<bool> DebugScreen::s_wantSounds{false};
std::atomic<bool> DebugScreen::s_wantParticles{false};
std::atomic<std::uint64_t> DebugScreen::s_soundSampleTick{0};
std::atomic<int> DebugScreen::s_soundStatics{0};
std::atomic<int> DebugScreen::s_soundStreams{0};
std::atomic<int> DebugScreen::s_soundCap{0};
std::atomic<std::uint64_t> DebugScreen::s_soundTick{0};
std::atomic<int> DebugScreen::s_particleTotal{0};
std::atomic<std::uint64_t> DebugScreen::s_particleTick{0};
std::atomic<int> DebugScreen::s_soundLoaded{-1};
std::atomic<long long> DebugScreen::s_soundMemory{-1};
std::atomic<bool> DebugScreen::s_wantChunkRender{false};
std::atomic<int> DebugScreen::s_builderPending{0};
std::atomic<int> DebugScreen::s_builderFree{0};
std::atomic<std::uint64_t> DebugScreen::s_builderTick{0};

DebugScreen& DebugScreen::instance()
{
    static DebugScreen module;
    return module;
}

bool DebugScreen::available() const
{
    return m_definitionRegistered.load(std::memory_order_relaxed);
}

void DebugScreen::onFrameRendered()
{

    s_frames.fetch_add(1, std::memory_order_relaxed);
}

void DebugScreen::onServerTick(void* level, long long qpcTicks)
{

    if (qpcTicks > 0) {
        s_serverTicks.fetch_add(1, std::memory_order_relaxed);
        s_serverTickQpc.fetch_add(static_cast<std::uint64_t>(qpcTicks), std::memory_order_relaxed);
    }
    if (level != nullptr) {
        s_serverLevel.store(level, std::memory_order_relaxed);
    }
    s_lastServerTick.store(GetTickCount64(), std::memory_order_relaxed);
}

void DebugScreen::onPacketSent()
{
    s_packetsSent.fetch_add(1, std::memory_order_relaxed);
}

void DebugScreen::onPacketReceived()
{
    s_packetsReceived.fetch_add(1, std::memory_order_relaxed);
}

void DebugScreen::onStartGame(const char* version)
{
    const std::uint64_t entry = s_startGameEntry.fetch_add(1, std::memory_order_acq_rel) + 1;
    s_versionEntry.store(0, std::memory_order_release);
    std::size_t length = 0;
    if (version != nullptr) {
        while (length < 32 && version[length] != '\0') ++length;
    }
    if (dbgtext::validServerVersion(version, length)) {
        for (std::size_t i = 0; i < 32; ++i) {
            const char c = version[i];
            s_serverVersion[i].store(c, std::memory_order_relaxed);
            if (c == '\0') break;
        }
        s_versionEntry.store(entry, std::memory_order_release);
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            wchar_t wide[32]{};
            for (std::size_t i = 0; i < 31 && version[i] != '\0'; ++i) {
                wide[i] = static_cast<wchar_t>(version[i]);
            }
            log().info(L"DebugScreen: StartGame server version {}", wide);
        }
    } else {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            log().info(L"DebugScreen: StartGame server version was not accepted (field shape mismatch)");
        }
    }
}

void DebugScreen::onChunkTicked()
{
    s_chunkTicks.fetch_add(1, std::memory_order_relaxed);
}

void DebugScreen::onChunkTickHookReady()
{
    s_chunkTickHookReady.store(true, std::memory_order_release);
}

bool DebugScreen::soundSampleDue()
{

    const std::uint64_t now = GetTickCount64();
    std::uint64_t last = s_soundSampleTick.load(std::memory_order_relaxed);
    return now - last >= 1000
           && s_soundSampleTick.compare_exchange_strong(last, now, std::memory_order_relaxed);
}

void DebugScreen::onSounds(int statics, int streams, int cap, int loadedSounds, long long memoryBytes)
{
    s_soundLoaded.store(loadedSounds, std::memory_order_relaxed);
    s_soundMemory.store(memoryBytes, std::memory_order_relaxed);
    s_soundStatics.store(statics, std::memory_order_relaxed);
    s_soundStreams.store(streams, std::memory_order_relaxed);
    s_soundCap.store(cap, std::memory_order_relaxed);
    s_soundTick.store(GetTickCount64(), std::memory_order_release);
}

void DebugScreen::onBuilderStats(int pending, int freeBuffers)
{
    s_builderPending.store(pending, std::memory_order_relaxed);
    s_builderFree.store(freeBuffers, std::memory_order_relaxed);
    s_builderTick.store(GetTickCount64(), std::memory_order_release);
}

void DebugScreen::onParticles(int total)
{
    s_particleTotal.store(total, std::memory_order_relaxed);
    s_particleTick.store(GetTickCount64(), std::memory_order_release);
}

dbgscreen::ElementState DebugScreen::stateOf(int element) const
{
    if (element < 0 || element >= ds::kElementCount) {
        return ds::ElementState::Off;
    }
    return static_cast<ds::ElementState>(m_states[element].load(std::memory_order_relaxed));
}

void DebugScreen::setStateOf(int element, ds::ElementState state)
{
    if (element < 0 || element >= ds::kElementCount) {
        return;
    }
    m_states[element].store(static_cast<int>(state), std::memory_order_relaxed);
}

void DebugScreen::applyProfile(bool performance)
{
    for (int i = 0; i < ds::kElementCount; ++i) {
        const ds::Element& element = ds::kElements[i];
        setStateOf(i, performance ? element.performanceState : element.defaultState);
    }
}

bool DebugScreen::elementShown(int element) const
{
    const ds::ElementState state = stateOf(element);
    if (state == ds::ElementState::Always) {
        return true;
    }
    return state == ds::ElementState::InOverlay && overlayOpen();
}

bool DebugScreen::anyShown() const
{
    for (int i = 0; i < ds::kElementCount; ++i) {
        if (elementShown(i)) {
            return true;
        }
    }
    return false;
}

void DebugScreen::onScansReady()
{

    DebugKeys::instance().onScansReady();

    if (!cui::bindingsAvailable() || cui::persistentBool(0) == nullptr) {
        log().warn(L"DebugScreen: NOT usable (bindings {} / value storage {})",
                   cui::bindingsAvailable() ? L"ready" : L"missing",
                   cui::persistentBool(0) != nullptr ? L"ready" : L"missing");
        return;
    }

    if (std::byte* const store = Scanner::instance().address(Target::RenderedActorCountStore); store != nullptr) {
        m_renderedActors = static_cast<const std::int32_t*>(memory::ripTarget(store, 5));
    }
    if (std::byte* const condition = Scanner::instance().address(Target::RegionalDifficultyCondition);
        condition != nullptr) {
        std::int32_t slot = 0;
        std::memcpy(&slot, condition + 62, sizeof(slot));
        debugworld::setRegionalDifficultySlot(slot);
    }
    debugworld::setRegionalDifficultyTail(Scanner::instance().address(Target::RegionalDifficultyTail));

    {
        Scanner& scanner = Scanner::instance();
        std::byte* const call = scanner.address(Target::ClimateSampleCall);
        std::byte* const ctor = scanner.address(Target::OverworldGeneratorCtor);
        if (call != nullptr && ctor != nullptr) {
            std::int32_t biomeSource = 0;
            std::memcpy(&biomeSource, ctor + 45, sizeof(biomeSource));
            debugworld::setClimateTargets(memory::ripTarget(call, 25), memory::ripTarget(ctor, 3), biomeSource);
        }
    }

    {
        Scanner& scanner = Scanner::instance();
        std::byte* const loop = scanner.address(Target::PreliminarySurfaceLoop);
        std::byte* const call = scanner.address(Target::PreliminarySurfaceCall);
        std::byte* const flags = scanner.address(Target::BlenderFactoryFlags);
        if (loop != nullptr && call != nullptr && flags != nullptr) {
            const auto readFloat = [loop](std::size_t dispOffset) {
                float value = 0.0f;
                std::memcpy(&value, memory::ripTarget(loop, dispOffset), sizeof(value));
                return value;
            };
            debugworld::SurfaceConstantsIn constants{};
            constexpr std::size_t kFloatAt[9] = {9, 18, 36, 44, 52, 61, 70, 79, 88};
            for (int i = 0; i < 9; ++i) {
                constants.values[i] = readFloat(kFloatAt[i]);
            }
            std::memcpy(constants.table, memory::ripTarget(loop, 28), sizeof(constants.table));
            std::memcpy(&constants.topCell, loop + 1, sizeof(constants.topCell));
            debugworld::SurfaceTargetsIn targets{};
            targets.columnFunction = memory::ripTarget(call, 15);
            std::memcpy(&targets.samplerFromSubObject, call + 3, sizeof(std::int32_t));
            std::memcpy(&targets.factoryFromSubObject, call + 29, sizeof(std::int32_t));
            targets.factoryNoBlendFlag = static_cast<std::int32_t>(static_cast<std::uint8_t>(flags[11]));
            targets.densityGrid = scanner.address(Target::DensityGridEntry);
            debugworld::setSurfaceTargets(targets, constants);
        }
        if (std::byte* const vtable = scanner.address(Target::DBChunkStorageVtable),
            *const insert = scanner.address(Target::DiscardSetInsert);
            vtable != nullptr && insert != nullptr) {
            std::int32_t setSize = 0;
            std::int32_t setHead = 0;
            std::memcpy(&setSize, insert + 13, sizeof(setSize));
            std::memcpy(&setHead, insert + 26, sizeof(setHead));
            debugworld::setDiscardTargets(memory::ripTarget(vtable, 3), setHead, setSize);
        }
    }
    cui::addHudObserver(&DebugScreen::onHudCreated);

    uiprobe::registerDefExtension("hud", "hud_content", "", ds::layoutJson());

    uiprobe::registerDefReplaceArray("hud", "player_position", "bindings",
                                     ds::hideBindingJson("#player_position_visible", ds::kBindHideCoords));
    uiprobe::registerDefReplaceArray("hud", "number_of_days_played", "bindings",
                                     ds::hideBindingJson("#number_of_days_played_visible", ds::kBindHideDays));
    uiprobe::registerDefAppend("hud", "chat_panel", "bindings", ds::chatHideBindingJson());
    uiprobe::registerDefExtension("hud", "hud_content", "", ds::chatJson());
    uiprobe::registerDefExtension("hud", "hud_content", "", ds::pausedJson());

    writeBool(ds::kPauseWheelSlot, true);
    uiprobe::registerDefProperty("persona_emote", "emote_wheel_screen", "force_render_below", "true");
    uiprobe::registerDefAppend("persona_emote", "emote_wheel_screen_content", "bindings", ds::pauseWheelHideJson());

    publishHudOptions();
    m_definitionRegistered.store(true, std::memory_order_relaxed);

    if (!m_enabledWasInConfig) {
        setEnabled(true);
    }
    log().info(L"DebugScreen: ready (JE 26.3 F3; lines appear in worlds entered after injection)");
}

void DebugScreen::shutdown()
{

    DebugKeys::instance().shutdown();
    m_shuttingDown.store(true, std::memory_order_relaxed);
    serverfind::shutdown();
    s_wantSounds.store(false, std::memory_order_relaxed);
    s_wantParticles.store(false, std::memory_order_relaxed);
    hideAllRows();
    publishHudOptions();
    cui::detachPersistentText();
}

void DebugScreen::publishHudOptions()
{
    const bool on = enabled() && !m_shuttingDown.load(std::memory_order_relaxed);
    writeBool(ds::kHideCoordsSlot, on && hideCoordinates());
    writeBool(ds::kHideDaysSlot, on && hideDaysPlayed());
    writeBool(ds::kChatBottomSlot, on && chatBottomLeft());
    writeBool(ds::kGamePausedSlot, on && DebugKeys::instance().pausedByKey());
}

void DebugScreen::onEnabledChanged(bool on)
{
    DebugKeys::instance().onEnabledChanged(on);
    publishHudOptions();
}

void DebugScreen::setHideCoordinates(bool on)
{
    m_hideCoords.store(on, std::memory_order_relaxed);
    publishHudOptions();
}

void DebugScreen::setHideDaysPlayed(bool on)
{
    m_hideDays.store(on, std::memory_order_relaxed);
    publishHudOptions();
}

void DebugScreen::setChatBottomLeft(bool on)
{
    m_chatBottom.store(on, std::memory_order_relaxed);
    publishHudOptions();
}

void DebugScreen::hideAllRows()
{
    for (int slot = 0; slot < ds::kTextSlotCount; ++slot) {
        writeBool(ds::kFirstBoolSlot + slot, false);
        writeBool(ds::kFirstBlankSlot + slot, false);
    }
}

bool DebugScreen::sampleFps(int& outFps)
{
    const std::uint64_t now = GetTickCount64();
    const std::uint64_t frames = s_frames.load(std::memory_order_relaxed);
    if (m_markTick == 0) {
        m_markTick = now;
        m_framesAtMark = frames;
        return false;
    }

    const std::uint64_t elapsed = now - m_markTick;
    if (elapsed >= 1000) {
        m_fps = static_cast<int>((frames - m_framesAtMark) * 1000ULL / elapsed);
        m_markTick = now;
        m_framesAtMark = frames;
    }
    if (m_fps < 0) {
        return false;
    }
    outFps = m_fps;
    return true;
}

void DebugScreen::sampleNetwork()
{
    const std::uint64_t now = GetTickCount64();
    const std::uint64_t ticks = s_serverTicks.load(std::memory_order_relaxed);
    const std::uint64_t qpc = s_serverTickQpc.load(std::memory_order_relaxed);
    const std::uint64_t sent = s_packetsSent.load(std::memory_order_relaxed);
    const std::uint64_t received = s_packetsReceived.load(std::memory_order_relaxed);
    const std::uint64_t chunks = s_chunkTicks.load(std::memory_order_relaxed);
    const std::uint64_t entry = s_startGameEntry.load(std::memory_order_acquire);
    if (m_netMarkTick == 0 || m_networkEntry != entry) {
        m_networkEntry = entry;
        m_netMarkTick = now;
        m_ticksAtMark = ticks;
        m_tickQpcAtMark = qpc;
        m_sentAtMark = sent;
        m_receivedAtMark = received;
        m_chunkTicksAtMark = chunks;
        m_tickingChunks = -1;
        return;
    }
    const std::uint64_t elapsed = now - m_netMarkTick;
    if (elapsed < 1000) {
        return;
    }
    const std::uint64_t tickCount = ticks - m_ticksAtMark;

    m_tickingChunks = tickCount > 0 && s_chunkTickHookReady.load(std::memory_order_acquire)
        ? static_cast<int>((chunks - m_chunkTicksAtMark + tickCount / 2) / tickCount) : -1;
    if (tickCount > 0) {
        static LARGE_INTEGER frequency = [] {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return value;
        }();
        if (frequency.QuadPart > 0) {
            const double ms = static_cast<double>(qpc - m_tickQpcAtMark) * 1000.0
                              / static_cast<double>(frequency.QuadPart)
                              / static_cast<double>(tickCount);
            m_tickMs = m_tickMs < 0.0 ? ms : m_tickMs * 0.8 + ms * 0.2;
        }
    }
    m_txPerSecond = static_cast<int>((sent - m_sentAtMark) * 1000ULL / elapsed);
    m_rxPerSecond = static_cast<int>((received - m_receivedAtMark) * 1000ULL / elapsed);
    m_netMarkTick = now;
    m_ticksAtMark = ticks;
    m_tickQpcAtMark = qpc;
    m_sentAtMark = sent;
    m_receivedAtMark = received;
    m_chunkTicksAtMark = chunks;
}

void DebugScreen::fillVersionInfo(dbgtext::Sample& out) const
{
    char shortName[32]{};
    char longName[32]{};
    formatVersions(shortName, longName);
    setText(out.gameVersion, shortName);
    if (out.gameVersion.available) {
        setText(out.modName, "Tsukuyomi");
        setText(out.launchedVersion, TSUKUYOMI_VERSION);
    }

    const std::uint64_t entry = s_startGameEntry.load(std::memory_order_acquire);
    if (entry != 0 && s_versionEntry.load(std::memory_order_acquire) == entry) {
        char version[32]{};
        for (std::size_t i = 0; i < sizeof(version); ++i) {
            version[i] = s_serverVersion[i].load(std::memory_order_relaxed);
        }
        if (s_startGameEntry.load(std::memory_order_acquire) == entry
            && s_versionEntry.load(std::memory_order_acquire) == entry) {
            setText(out.serverBrand, version);
        }
    }
}

void DebugScreen::fillSample(dbgtext::Sample& out)
{
    {

        const std::uint64_t lastTick = s_lastServerTick.load(std::memory_order_relaxed);
        out.localServer = {true, lastTick != 0 && GetTickCount64() - lastTick < 3000};
    }

    fillVersionInfo(out);

    GameData& data = GameData::instance();
    const bool inWorld = data.msSinceView() < 500;
    if (!inWorld) {
        out.serverBrand.available = false;
    }
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (inWorld && data.playerBoxFeet(x, y, z)) {
        out.x = {true, static_cast<double>(x)};
        out.y = {true, static_cast<double>(y)};
        out.z = {true, static_cast<double>(z)};
    }
    if (inWorld && data.hasPlayerView()) {
        const PlayerView view = data.playerView();
        out.yaw = {true, static_cast<double>(view.yaw)};
        out.pitch = {true, static_cast<double>(view.pitch)};
    }

    int fps = 0;
    if (sampleFps(fps)) {
        out.fps = {true, fps};
    }

    const std::uint64_t now = GetTickCount64();
    if (m_optionsTick == 0 || now - m_optionsTick >= 30000) {
        m_optionsTick = now;
        gameoptions::reload();
    }
    int limit = 0;
    if (gameoptions::getInt("gfx_max_framerate", limit)) {
        out.fpsLimit = {true, limit};
    }

    int texelAa = 0;
    if (gameoptions::getInt("gfx_texel_aa_2", texelAa)) {
        setText(out.filtering, texelAa != 0 ? "Texel AA" : "None");
    }

    int graphicsMode = 0;
    if (gameoptions::getInt("graphics_mode", graphicsMode)) {
        static constexpr const char* kGraphicsModes[] = {"Simple", "Fancy", "Vibrant Visuals", "Ray Traced"};
        if (graphicsMode >= 0
            && graphicsMode < static_cast<int>(sizeof(kGraphicsModes) / sizeof(kGraphicsModes[0]))) {
            setText(out.terrainRendering, kGraphicsModes[graphicsMode]);
        }
    }

    sysinfo::Memory memory{};
    const bool haveMemory = elementShown(kMemoryElement) && sysinfo::memoryUsage(memory)
                            && memory.totalPhysical > 0;
    if (haveMemory) {
        constexpr std::uint64_t kMib = 1024ULL * 1024ULL;
        const int usedMb = static_cast<int>(memory.privateBytes / kMib);
        const int totalMb = static_cast<int>(memory.totalPhysical / kMib);
        const int workingMb = static_cast<int>(memory.workingSet / kMib);
        out.privateMegabytes = {true, usedMb};
        out.totalMegabytes = {true, totalMb};
        out.memoryPercent = {true, totalMb > 0 ? usedMb * 100 / totalMb : 0};
        out.workingSetMegabytes = {true, workingMb};
        out.allocatedPercent = {true, totalMb > 0 ? workingMb * 100 / totalMb : 0};
    }

    char text[192]{};
    static char s_cpuText[192]{};
    static bool s_cpuTried = false;
    char brand[160]{};
    const bool buildCpuText = !s_cpuTried && sysinfo::cpuName(brand, sizeof(brand));
    s_cpuTried = true;
    if (buildCpuText) {

        char collapsed[160]{};
        std::size_t used = 0;
        for (const char* c = brand; *c != '\0' && used + 1 < sizeof(collapsed); ++c) {
            const bool space = *c == ' ' || *c == '\t';
            if (space && (used == 0 || collapsed[used - 1] == ' ')) {
                continue;
            }
            collapsed[used++] = space ? ' ' : *c;
        }
        while (used > 0 && collapsed[used - 1] == ' ') {
            --used;
        }
        collapsed[used] = '\0';
        int threads = 0;
        if (sysinfo::cpuThreads(threads)) {
            std::snprintf(text, sizeof(text), "%dx %s", threads, collapsed);
        } else {
            std::snprintf(text, sizeof(text), "%s", collapsed);
        }
        std::snprintf(s_cpuText, sizeof(s_cpuText), "%s", text);
    }
    if (s_cpuText[0] != '\0') {
        setText(out.cpu, s_cpuText);
    }
    static sysinfo::Display s_display{};
    static bool s_haveDisplay = false;
    static std::uint64_t s_displayTick = 0;
    void* const window = sysinfo::mainWindow();
    if (s_displayTick == 0 || now - s_displayTick >= 1000) {
        s_displayTick = now;
        s_display = sysinfo::Display{};
        s_haveDisplay = sysinfo::displayInfo(window, s_display);
    }
    if (s_haveDisplay) {
        const sysinfo::Display& display = s_display;
        std::snprintf(text, sizeof(text), "%dx%d (%s)", display.width, display.height, display.vendor);
        setText(out.display, text);
        setText(out.graphicsCard, display.adapter);
        if (display.refreshRate > 0.0) {

            out.refreshRate = {true, display.refreshRate};
        } else if (display.refreshHz > 0) {
            out.refreshRate = {true, static_cast<double>(display.refreshHz)};
        }

        const UINT dpi = window != nullptr ? GetDpiForWindow(static_cast<HWND>(window)) : 96;
        std::snprintf(text, sizeof(text), "%dx%d (%.2fx pixel density)", display.width,
                      display.height, dpi > 0 ? static_cast<double>(dpi) / 96.0 : 1.0);
        setText(out.window, text);

        const char* api = nullptr;
        if (frametrace::sawD3D12()) {
            api = "Direct3D 12";
        } else if (GetModuleHandleW(L"d3d12.dll") == nullptr && GetModuleHandleW(L"d3d11.dll") != nullptr) {
            api = "Direct3D 11";
        }
        if (api != nullptr) {
            if (display.driver[0] != '\0') {
                std::snprintf(text, sizeof(text), "%s %s", api, display.driver);
                setText(out.graphicsApi, text);
            } else {
                setText(out.graphicsApi, api);
            }
        }
    }

    if (elementShown(kGpuElement)) {
        if (m_gpuTick == 0 || now - m_gpuTick >= 1000) {
            m_gpuTick = now;
            int percent = 0;
            if (sysinfo::gpuUtilization(percent)) {
                m_gpuPercent = percent;
            }
        }
        if (m_gpuPercent >= 0) {
            out.gpuPercent = {true, m_gpuPercent};
        }
    }

    sampleNetwork();
    const std::uint64_t lastServerTick = s_lastServerTick.load(std::memory_order_relaxed);
    if (lastServerTick != 0 && now - lastServerTick < 3000) {

        setText(out.serverBrand, "Integrated");
        if (m_tickMs >= 0.0) {
            out.millisecondsPerTick = {true, m_tickMs};
        }
    }
    if (m_txPerSecond >= 0) {
        out.packetsUpPerTick = {true, m_txPerSecond};
    }
    if (m_rxPerSecond >= 0) {
        out.packetsDownPerTick = {true, m_rxPerSecond};
    }

    {
        const std::uint64_t at = s_soundTick.load(std::memory_order_acquire);
        if (at != 0 && now - at < kSoundFreshMs) {
            out.staticSounds = {true, s_soundStatics.load(std::memory_order_relaxed)};
            out.staticSoundCap = {true, s_soundCap.load(std::memory_order_relaxed)};
            out.streamingSounds = {true, s_soundStreams.load(std::memory_order_relaxed)};

            const int loaded = s_soundLoaded.load(std::memory_order_relaxed);
            const long long fmodMemory = s_soundMemory.load(std::memory_order_relaxed);
            if (loaded >= 0 && fmodMemory >= 0) {
                out.soundCacheBuffers = {true, loaded};

                out.soundCacheMebibytes = {true, static_cast<int>((fmodMemory + 1024LL * 1024LL - 1) / (1024LL * 1024LL))};
            }
        }
    }

    {
        const std::uint64_t at = s_builderTick.load(std::memory_order_acquire);
        if (at != 0 && now - at < 1000) {
            out.pendingBuild = {true, s_builderPending.load(std::memory_order_relaxed)};
            out.freeBuffers = {true, s_builderFree.load(std::memory_order_relaxed)};
        }
    }

    {
        const std::uint64_t at = s_particleTick.load(std::memory_order_acquire);
        if (at != 0 && now - at < kParticleFreshMs) {
            out.particleTotal = {true, s_particleTotal.load(std::memory_order_relaxed)};
        }
    }

    debugworld::Sample world{};
    if (debugworld::read(world)) {
        if (world.hasTime) {
            out.dayTime = {true, world.dayTime};
        }
        if (world.hasDifficulty) {
            out.difficultyId = {true, world.difficultyId};
        }
        if (world.hasSimulationDistance) {
            out.simulationDistance = {true, world.simulationDistance};
        }
        if (world.hasLight) {
            out.skyLight = {true, world.skyLight};
            out.blockLight = {true, world.blockLight};
        }
        if (world.hasDimension) {
            out.dimensionId = {true, world.dimensionId};
        }

        if (world.hasSpeed) {

            const double dx = world.dx;
            const double dy = world.dy;
            const double dz = world.dz;
            out.speed = {true, std::sqrt(dx * dx + dy * dy + dz * dz)};
        }
        if (world.hasChunkGrid) {

            out.clientWanted = {true, world.chunkSlots};
            out.clientLive = {true, world.chunkLive};
        }

        if (world.hasEntities) {

            out.clientEntities = {true, world.entityCount};
            out.clientEntitySections = {true, world.entitySections};
            if (out.localServer.value && m_tickingChunks >= 0) {
                out.tickingChunks = {true, m_tickingChunks};
            }

            std::int32_t rendered = 0;
            if (readRenderedActors(m_renderedActors, rendered)) {
                out.renderedEntities = {true, rendered};
                out.totalEntities = {true, world.entityCount};
            } else {
                out.renderedEntities = {true, world.entityCount};
            }
        }
        if (world.hasTarget) {
            out.blockX = {true, world.targetX};
            out.blockY = {true, world.targetY};
            out.blockZ = {true, world.targetZ};
            setText(out.blockName, world.targetName);
            const int states = std::min(world.targetStateCount,
                                        static_cast<int>(sizeof(out.blockStates) / sizeof(out.blockStates[0])));
            for (int i = 0; i < states; ++i) {
                setText(out.blockStates[i], world.targetStates[i]);
            }

            const int tags = std::min(world.targetTagCount,
                                      static_cast<int>(sizeof(out.blockTags) / sizeof(out.blockTags[0])));
            for (int i = 0; i < tags; ++i) {
                setText(out.blockTags[i], world.targetTags[i]);
            }
        }
        if (world.hasEntityTarget) {
            setText(out.entityName, world.entityName);

            const int tags = std::min(world.entityTagCount,
                                      static_cast<int>(sizeof(out.entityTags) / sizeof(out.entityTags[0])));
            for (int i = 0; i < tags; ++i) {
                setText(out.entityTags[i], world.entityTags[i]);
            }
        }
        if (world.hasBiome) {

            setText(out.biome, world.biome);
        }
        if (world.hasSections) {

            out.renderedSections = {true, world.sectionsNonEmpty};
            out.totalSections = {true, world.sectionsTotal};

            const int mode = GameModeState::instance().currentMode();
            if (gamemode::isSelectable(mode)) {
                out.spectator = {true, mode == gamemode::kSpectator ? 1 : 0};
            }

            int viewDistance = 0;
            if (gameoptions::getInt("gfx_viewdistance", viewDistance) && viewDistance > 0) {
                out.viewDistance = {true, viewDistance / 16};
            }

        }
        if (world.hasFluid) {
            out.fluidX = {true, world.fluidX};
            out.fluidY = {true, world.fluidY};
            out.fluidZ = {true, world.fluidZ};
            setText(out.fluidName, world.fluidName);
            const int states = std::min(world.fluidStateCount,
                                        static_cast<int>(sizeof(out.fluidStates) / sizeof(out.fluidStates[0])));
            for (int i = 0; i < states; ++i) {
                setText(out.fluidStates[i], world.fluidStates[i]);
            }
            const int tags = std::min(world.fluidTagCount,
                                      static_cast<int>(sizeof(out.fluidTags) / sizeof(out.fluidTags[0])));
            for (int i = 0; i < tags; ++i) {
                setText(out.fluidTags[i], world.fluidTags[i]);
            }
        }

        if (world.hasServerChunks) {

            out.serverChunks = {true, world.serverChunks};
            if (world.hasServerEntities) {
                out.serverEntities = {true, world.serverEntityCount};
                out.serverVisible = {true, world.serverVisibleEntities};
                out.serverSections = {true, world.serverEntitySections};
                out.serverWanted = {true, world.serverChunks};
                if (m_tickingChunks >= 0) out.serverTicking = {true, m_tickingChunks};
                out.serverLoad = {true, world.serverLoadingChunks};
                if (world.hasServerUnload) {
                    out.serverUnload = {true, world.serverUnloadChunks};
                }
            }
        }
        if (world.hasLocalDifficulty) {
            out.localDifficulty = {true, world.localDifficulty};
            if (world.hasLocalDifficultyRaw) {
                out.localDifficultyRaw = {true, world.localDifficultyRaw};
            }
        }
        if (world.hasForcedChunks) {
            out.forcedChunks = {true, world.forcedChunks};
        }
        if (world.hasClimate) {

            out.noiseT = {true, world.climate[0] / 10000.0};
            out.noiseV = {true, world.climate[1] / 10000.0};
            out.noiseC = {true, world.climate[2] / 10000.0};
            out.noiseE = {true, world.climate[3] / 10000.0};
            out.noiseD = {true, world.climate[4] / 10000.0};
            out.noiseW = {true, world.climate[5] / 10000.0};
        }
        if (world.hasSurfaceLevel) {

            out.noisePS = {true, world.surfaceLevelFound ? static_cast<double>(world.surfaceLevel) : 2147483647.0};
        }
        if (world.hasDensity) {
            out.noiseN = {true, static_cast<double>(world.density)};
        }
    }

    const bool hasPosition = out.x.available && out.y.available && out.z.available;
    const bool readDb = m_allowDbRead.load(std::memory_order_relaxed)
                     && GetCurrentThreadId() == m_mainThreadId.load(std::memory_order_relaxed);
    if (readDb && !hasPosition) m_worldIndex.reset();
    if (elementShown(kPlayerPositionElement) && hasPosition && out.dimensionId.available) {
        const int cx = static_cast<int>(std::floor(out.x.value / 16.0));
        const int cz = static_cast<int>(std::floor(out.z.value / 16.0));
        const int subY = static_cast<int>(std::floor(out.y.value / 16.0));
        const auto key = leveldb::subChunkKey(cx, cz, out.dimensionId.value, subY);
        const std::string hex = leveldb::keyHex(key);
        setText(out.subChunkKey, hex.c_str());
        if (readDb) {
            const std::uint64_t entry = s_startGameEntry.load(std::memory_order_acquire);
            const leveldb::UpdateEvent event = m_worldIndex.update(GetTickCount64(), entry);
            if (!m_worldIndex.folderName().empty() && !m_loggedDbFound) {
                log().info(L"DebugScreen: LevelDB world folder {}", m_worldIndex.folderName());
                m_loggedDbFound = true;
            }
            if (event == leveldb::UpdateEvent::Missing && !m_loggedDbMissing) {
                log().info(L"DebugScreen: active LevelDB world not found");
                m_loggedDbMissing = true;
            }
            if (event == leveldb::UpdateEvent::Unreadable && !m_loggedManifestUnreadable) {
                log().warn(L"DebugScreen: LevelDB MANIFEST unreadable");
                m_loggedManifestUnreadable = true;
            }
            if (m_worldIndex.multipleManifestHandlesSeen() && !m_loggedMultipleManifestHandles) {
                log().warn(L"DebugScreen: multiple open LevelDB MANIFEST handles; using the first");
                m_loggedMultipleManifestHandles = true;
            }
            const std::uint64_t number = m_worldIndex.find(key);
            if (number != 0) {
                char filename[32]{};
                std::snprintf(filename, sizeof(filename), "%06llu.ldb", static_cast<unsigned long long>(number));
                setText(out.subChunkFile, filename);
            }
        }
    }

}

void DebugScreen::publish()
{
    if (!m_definitionRegistered.load(std::memory_order_relaxed)) {
        return;
    }

    publishHudOptions();
    if (m_shuttingDown.load(std::memory_order_relaxed) || !enabled()) {
        s_wantSounds.store(false, std::memory_order_relaxed);
        s_wantParticles.store(false, std::memory_order_relaxed);
        s_wantChunkRender.store(false, std::memory_order_relaxed);
        hideAllRows();
        return;
    }

    s_wantSounds.store(elementShown(kSoundMoodElement) || elementShown(kSoundCacheElement), std::memory_order_relaxed);
    s_wantParticles.store(elementShown(kParticleRenderStatsElement), std::memory_order_relaxed);
    s_wantChunkRender.store(elementShown(kChunkRenderStatsElement), std::memory_order_relaxed);

    dbgtext::Sample sample{};

    if (anyShown()) {
        fillSample(sample);
    } else {

        int fps = 0;
        sampleFps(fps);
        sampleNetwork();

    }

    int count = 0;
    for (int i = 0; i < ds::kElementCount; ++i) {
        if (!elementShown(i)) {
            continue;
        }
        const ds::Element& element = ds::kElements[i];
        const int rows = dbgtext::formatElement(element.id, sample, m_lines[i], ds::kMaxRowsPerElement);

        const bool groupPresent =
            element.port == ds::Port::Group
            && (i != kChunkGenerationStatsElement || (sample.localServer.available && sample.localServer.value));
        m_items[count++] = ds::Contribution{i, rows, groupPresent};
    }
    ds::arrange(m_items, count, m_arranged);

    for (int side = 0; side < 2; ++side) {
        const ds::Column column = side == 0 ? ds::Column::Left : ds::Column::Right;
        for (int row = 0; row < ds::kRowsPerColumn; ++row) {
            const int textSlot = ds::textSlotOf(column, row);
            const int boolSlot = ds::boolSlotOf(column, row);
            const int blankSlot = ds::blankSlotOf(column, row);
            if (textSlot < 0 || boolSlot < 0 || blankSlot < 0) {
                continue;
            }
            if (row >= m_arranged.rows[side]) {
                writeBool(boolSlot, false);
                writeBool(blankSlot, false);
                continue;
            }
            const ds::LineRef line = m_arranged.line[side][row];
            const char* text = nullptr;
            if (line.contribution >= 0 && line.contribution < count) {
                text = m_lines[m_items[line.contribution].element][line.row];
            }
            if (text == nullptr) {

                cui::writePersistentText(textSlot, " ", 1);
                writeBool(boolSlot, false);
                writeBool(blankSlot, true);
                continue;
            }

            constexpr std::size_t kSlotChars = cui::kPersistentTextBytes - 16 - 1;
            char escaped[kSlotChars + 1]{};
            std::size_t length = 0;
            for (const char* c = text; *c != '\0'; ++c) {
                const std::size_t need = *c == '%' ? 2 : 1;
                if (length + need > kSlotChars) {
                    break;
                }
                escaped[length++] = *c;
                if (*c == '%') {
                    escaped[length++] = '%';
                }
            }
            cui::writePersistentText(textSlot, escaped, length);
            writeBool(blankSlot, false);
            writeBool(boolSlot, true);
        }
    }

    static std::atomic<bool> toldDropped{false};
    if (m_arranged.dropped[0] + m_arranged.dropped[1] > 0 && !toldDropped.exchange(true)) {
        log().warn(L"DebugScreen: {} line(s) did not fit in the {} rows per column", m_arranged.dropped[0] + m_arranged.dropped[1],
                   ds::kRowsPerColumn);
    }
}

void DebugScreen::onPlayerViewUpdate()
{
    const unsigned keyRequests = m_keySampleRequests.exchange(0, std::memory_order_acq_rel);
    if (m_shuttingDown.load(std::memory_order_relaxed)) {
        return;
    }

    if (keyRequests == 0 && (!enabled() || !anyShown())) {
        return;
    }
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!GameData::instance().playerBoxFeet(x, y, z)) {

        if (keyRequests != 0) {
            m_keySampleRequests.fetch_or(keyRequests, std::memory_order_release);
        }
        return;
    }
    const int feetX = static_cast<int>(std::floor(x));
    const int feetY = static_cast<int>(std::floor(y));
    const int feetZ = static_cast<int>(std::floor(z));

    debugworld::Wanted wanted{};
    wanted.target =
        elementShown(kLookingAtBlockStateElement) || elementShown(kLookingAtBlockTagsElement)
        || (keyRequests & kKeyData) != 0;
    wanted.chunks = elementShown(kChunkSourceStatsElement);
    wanted.speed = elementShown(kPlayerSpeedElement);
    wanted.entities = elementShown(kChunkSourceStatsElement) || elementShown(kEntityRenderStatsElement);
    wanted.entityTarget = elementShown(kLookingAtEntityElement) || (keyRequests & kKeyData) != 0;
    wanted.entityTags = elementShown(kLookingAtEntityTagsElement);

    wanted.serverChunks = elementShown(kChunkSourceStatsElement);
    wanted.localDifficulty = elementShown(kLocalDifficultyElement);
    wanted.forcedChunks = elementShown(kPlayerPositionElement);
    wanted.climate = elementShown(kChunkGenerationStatsElement);
    const std::uint64_t lastTick = s_lastServerTick.load(std::memory_order_relaxed);
    wanted.localServer = lastTick != 0 && GetTickCount64() - lastTick < 3000;

    if (wanted.localServer
        && (wanted.serverChunks || wanted.localDifficulty || wanted.forcedChunks || wanted.entityTags
            || wanted.climate)) {
        serverfind::requestIfMissing(true);
    }
    wanted.biome = elementShown(kBiomeElement);
    wanted.sections = elementShown(kChunkRenderStatsElement);
    wanted.blockTags = elementShown(kLookingAtBlockTagsElement);
    wanted.fluid = elementShown(kLookingAtFluidStateElement) || elementShown(kLookingAtFluidTagsElement);
    if (debugworld::collect(feetX, feetY, feetZ, wanted, keyRequests != 0) && keyRequests != 0) {
        m_keySampleGeneration.fetch_add(1, std::memory_order_release);
        m_keySampleReady.fetch_or(keyRequests, std::memory_order_release);
    }
}

void DebugScreen::onUpdate()
{

    DebugKeys::instance().onUpdate();

    if (m_overlayKey.releasedAlone() && input::isInGameplay()) {
        const bool open = !m_overlayOpen.load(std::memory_order_relaxed);
        m_overlayOpen.store(open, std::memory_order_relaxed);

        UiSound::instance().request();
    }

    const std::uint64_t now = GetTickCount64();
    if (now - m_lastPublishTick >= 50) {
        m_lastPublishTick = now;
        m_mainThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
        m_allowDbRead.store(true, std::memory_order_relaxed);
        publish();
        m_allowDbRead.store(false, std::memory_order_relaxed);
    }

    publishHudOptions();
}

void DebugScreen::onHudCreated(void* ctrl)
{
    DebugScreen& self = instance();
    if (!self.m_definitionRegistered.load(std::memory_order_relaxed)) {
        return;
    }

    self.publish();

    int boundText = 0;
    int boundBool = 0;
    char name[24]{};
    for (int side = 0; side < 2; ++side) {
        const ds::Column column = side == 0 ? ds::Column::Left : ds::Column::Right;
        for (int row = 0; row < ds::kRowsPerColumn; ++row) {
            ds::textBindName(column, row, name, sizeof(name));
            boundText += cui::bindPersistentText(ctrl, name, ds::textSlotOf(column, row)) ? 1 : 0;
            ds::visibleBindName(column, row, name, sizeof(name));
            boundBool += cui::bindPersistentBool(ctrl, name, ds::boolSlotOf(column, row)) ? 1 : 0;
            ds::blankBindName(column, row, name, sizeof(name));
            boundBool += cui::bindPersistentBool(ctrl, name, ds::blankSlotOf(column, row)) ? 1 : 0;
        }
    }

    boundBool += cui::bindPersistentBool(ctrl, ds::kBindHideCoords, ds::kHideCoordsSlot) ? 1 : 0;
    boundBool += cui::bindPersistentBool(ctrl, ds::kBindHideDays, ds::kHideDaysSlot) ? 1 : 0;
    boundBool += cui::bindPersistentBool(ctrl, ds::kBindChatBottom, ds::kChatBottomSlot) ? 1 : 0;
    boundBool += cui::bindPersistentBool(ctrl, ds::kBindGamePaused, ds::kGamePausedSlot) ? 1 : 0;
    constexpr int kWanted = ds::kTextSlotCount;
    if (g_bindLogs.fetch_add(1, std::memory_order_relaxed) < kMaxBindLogs) {
        if (boundText == kWanted && boundBool == 2 * kWanted + 4) {
            log().info(L"DebugScreen: bound {} text / {} bool values to the HUD controller", boundText,
                       boundBool);
        } else {
            log().warn(L"DebugScreen: bound only {} text / {} bool of {} rows to the HUD controller",
                       boundText, boundBool, kWanted);
        }
    }
}

MenuItem DebugScreen::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(menu::back());
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());

    for (int profile = 0; profile < 2; ++profile) {
        const bool performance = profile == 1;
        MenuItem item = menu::action(performance ? L"Apply Performance profile" : L"Apply Default profile",
                                     [this, performance] {
                                         applyProfile(performance);
                                         uiprobe::markSettingsDirty();
                                     });
        item.value = [] { return std::wstring(L"Apply"); };
        children.push_back(std::move(item));
    }

    for (int i = 0; i < ds::kElementCount; ++i) {
        children.push_back(menu::choice(
            ds::kElements[i].label, {L"OFF", L"In Overlay", L"Always"},
            [this, i] { return static_cast<int>(stateOf(i)); },
            [this, i](int value) { setStateOf(i, clampState(value)); }));
    }

    children.push_back(menu::toggle(L"Hide vanilla coordinates", [this] { return hideCoordinates(); },
                                    [this] { setHideCoordinates(!hideCoordinates()); }));
    children.push_back(menu::toggle(L"Hide vanilla days played", [this] { return hideDaysPlayed(); },
                                    [this] { setHideDaysPlayed(!hideDaysPlayed()); }));
    children.push_back(menu::toggle(L"Chat at bottom left", [this] { return chatBottomLeft(); },
                                    [this] { setChatBottomLeft(!chatBottomLeft()); }));

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void DebugScreen::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    m_enabledWasInConfig = section.contains("enabled");

    std::vector<int> combo{kDefaultOverlayKey};
    if (const auto it = section.find("overlayKey"); it != section.end() && it->is_array()) {
        combo.clear();
        for (const auto& value : *it) {
            if (value.is_number_integer()) {
                combo.push_back(value.get<int>());
            }
        }
    }
    m_overlayKey.set(std::move(combo));

    const auto readFlag = [&section](const char* key) {
        const auto it = section.find(key);
        return it != section.end() && it->is_boolean() && it->get<bool>();
    };
    m_hideCoords.store(readFlag("hideCoordinates"), std::memory_order_relaxed);
    m_hideDays.store(readFlag("hideDaysPlayed"), std::memory_order_relaxed);
    m_chatBottom.store(readFlag("chatBottomLeft"), std::memory_order_relaxed);

    nlohmann::json keys = section;
    if (nlohmann::json& legacy = Config::instance().section("DebugKeys"); legacy.is_object()) {
        for (auto it = legacy.begin(); it != legacy.end(); ++it) {
            const std::string& key = it.key();
            if (key.size() > 4 && key.ends_with("Keys") && !keys.contains(key)) keys[key] = it.value();
        }
    }
    Config::instance().eraseSection("DebugKeys");
    DebugKeys::instance().loadKeys(keys);

    applyProfile(false);
    if (const auto it = section.find("text"); it != section.end() && it->is_object()) {
        for (int i = 0; i < ds::kElementCount; ++i) {
            auto value = it->find(ds::kElements[i].id);
            if (value != it->end() && value->is_number_integer()) {
                setStateOf(i, clampState(value->get<int>()));
            }
        }
    }
}

void DebugScreen::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["overlayKey"] = m_overlayKey.combo();
    section["hideCoordinates"] = hideCoordinates();
    section["hideDaysPlayed"] = hideDaysPlayed();
    section["chatBottomLeft"] = chatBottomLeft();
    DebugKeys::instance().saveKeys(section);

    nlohmann::json text = nlohmann::json::object();
    for (int i = 0; i < ds::kElementCount; ++i) {
        text[ds::kElements[i].id] = static_cast<int>(stateOf(i));
    }
    section["text"] = std::move(text);
}

}
