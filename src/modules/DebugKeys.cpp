#include "modules/DebugKeys.h"

#include "config/Config.h"
#include "config/WriteSwitches.h"
#include "core/Logger.h"
#include "game/ClientChat.h"
#include "game/DebugKeysCommands.h"
#include "game/DebugScreenWorld.h"
#include "game/GameModeIds.h"
#include "game/GameModeWheel.h"
#include "game/AdvancedTooltipText.h"
#include "game/ContainerUi.h"
#include "game/GameData.h"
#include "game/GameString.h"
#include "game/UiProbe.h"
#include "hooks/Detours.h"
#include "input/Foreground.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "modules/DebugScreen.h"
#include "game/GameModeState.h"
#include "render/BoxRenderer.h"
#include "render/WorldMesh.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi {

namespace {

constexpr unsigned kVersion = 2u;
constexpr unsigned kPause = 4u;
constexpr unsigned kReload = 8u;
constexpr unsigned kChunks = 32u;
constexpr unsigned kAdvanced = 16u;
constexpr unsigned kHitbox = 64u;
constexpr unsigned kChunkBorder = 128u;
constexpr unsigned kClearChat = 256u;
constexpr unsigned kGamePause = 512u;
constexpr unsigned kGameMode = 1024u;
constexpr unsigned kSpectator = 2048u;

using ClearMessagesFn = void(__fastcall*)(void*);
using ChatCommandFn = bool(__fastcall*)(const std::string*, const void*);

bool runChatCommandRaw(void* ci, ChatCommandFn fn, const std::string* text, bool& handled)
{
    __try {
        void* control = *reinterpret_cast<void**>(static_cast<std::byte*>(ci) + 8);
        if (control == nullptr || *static_cast<unsigned char*>(control) != 1) return false;
        void* reference[3] = {control, nullptr, ci};
        handled = fn(text, reference);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
using GamePauseFn = void(__fastcall*)(void*, bool);

bool clearChatRaw(void* ci, std::int32_t offset, ClearMessagesFn fn)
{
    __try {
        void* gui = *reinterpret_cast<void**>(static_cast<std::byte*>(ci) + offset);
        if (gui == nullptr) return false;
        fn(gui);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool pauseGameRaw(void* ci, GamePauseFn a, GamePauseFn b, bool pause)
{
    __try {
        void* control = *reinterpret_cast<void**>(static_cast<std::byte*>(ci) + 8);
        if (control == nullptr || *static_cast<unsigned char*>(control) != 1) return false;
        alignas(void*) unsigned char model[0x60]{};
        *reinterpret_cast<void**>(model + 0x48) = control;
        *reinterpret_cast<void**>(model + 0x58) = ci;
        a(model, pause);
        b(model, pause);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void debugMessage(std::string_view message)
{
    clientchat::printLocal("§e§l[Debug]:§r " + std::string(message));
}

bool putClipboardRaw(const std::string& utf8)
{
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, nullptr, 0);
    if (chars <= 0) {
        return false;
    }
    const std::size_t bytes = static_cast<std::size_t>(chars) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        return false;
    }
    wchar_t* const buffer = static_cast<wchar_t*>(GlobalLock(memory));
    if (buffer == nullptr) {
        GlobalFree(memory);
        return false;
    }
    const bool converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1,
                                               buffer, chars) == chars;
    GlobalUnlock(memory);
    if (!converted) {
        GlobalFree(memory);
        return false;
    }
    if (!OpenClipboard(nullptr)) {
        GlobalFree(memory);
        return false;
    }
    const bool success = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    CloseClipboard();
    if (!success) {
        GlobalFree(memory);
    }
    return success;
}

bool putClipboard(const std::string& utf8)
{
    if (putClipboardRaw(utf8)) {
        return true;
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        log().warn(L"DebugKeys: clipboard write failed");
    }
    return false;
}

bool setBoolOptionRaw(void* client, std::int32_t optionsSlot, std::int32_t lookupSlot,
                      std::int32_t optionId, void* setterAddress, int want, void*& optionsOut,
                      bool& oldValue, bool& newValue)
{
    __try {
        if (client == nullptr || setterAddress == nullptr) {
            return false;
        }
        auto** clientVtable = *reinterpret_cast<void***>(client);
        using GetOptions = void*(__fastcall*)(void*);
        auto getOptions = reinterpret_cast<GetOptions>(clientVtable[optionsSlot / sizeof(void*)]);
        void* options = getOptions(client);
        if (options == nullptr) {
            return false;
        }
        optionsOut = options;
        auto** optionsVtable = *reinterpret_cast<void***>(options);
        using Lookup = void(__fastcall*)(void*, void**, std::int32_t);
        auto lookup = reinterpret_cast<Lookup>(optionsVtable[lookupSlot / sizeof(void*)]);
        void* result[2]{};
        lookup(options, result, optionId);
        void* const root = result[0];
        if (root == nullptr) {
            return false;
        }
        void* current = root;
        for (;;) {
            void* const holder = *reinterpret_cast<void**>(static_cast<char*>(current) + 8);
            void* const next = *reinterpret_cast<void**>(static_cast<char*>(holder) + 0x238);
            if (next == nullptr) {
                break;
            }
            current = next;
        }
        oldValue = *reinterpret_cast<std::uint8_t*>(static_cast<char*>(current) + 0x10) != 0;
        using SetOption = void(__fastcall*)(void*, bool, bool);
        reinterpret_cast<SetOption>(setterAddress)(root, want < 0 ? !oldValue : want != 0, true);
        current = root;
        for (;;) {
            void* const holder = *reinterpret_cast<void**>(static_cast<char*>(current) + 8);
            void* const next = *reinterpret_cast<void**>(static_cast<char*>(holder) + 0x238);
            if (next == nullptr) {
                break;
            }
            current = next;
        }
        newValue = *reinterpret_cast<std::uint8_t*>(static_cast<char*>(current) + 0x10) != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool durabilityRaw(const void* stack, std::int32_t slot, void* damageValue, int& max, int& damage)
{
    __try {
        if (stack == nullptr || slot <= 0 || damageValue == nullptr) return false;
        auto* const bytes = static_cast<const std::byte*>(stack);
        void* const weak = *reinterpret_cast<void* const*>(bytes + 0x08);
        void* const item = weak != nullptr ? *static_cast<void* const*>(weak) : nullptr;
        if (item == nullptr) return false;
        void** const vt = *static_cast<void***>(item);
        using MaxDamageFn = short(__fastcall*)(const void*);
        using DamageValueFn = short(__fastcall*)(const void*);
        max = reinterpret_cast<MaxDamageFn>(vt[slot / 8])(item);
        damage = max > 0 ? reinterpret_cast<DamageValueFn>(damageValue)(stack) : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool viewPerspectiveRaw(void* client, std::int32_t optionsSlot, void* getter, int& out)
{
    __try {
        if (client == nullptr || getter == nullptr) return false;
        auto** clientVtable = *reinterpret_cast<void***>(client);
        using GetOptions = void*(__fastcall*)(void*);
        void* const options = reinterpret_cast<GetOptions>(clientVtable[optionsSlot / sizeof(void*)])(client);
        if (options == nullptr) return false;
        void** const optionsVtable = *static_cast<void***>(options);
        bool listed = false;
        for (int i = 0; i < 512 && !listed; ++i) listed = optionsVtable[i] == getter;
        if (!listed) return false;
        out = reinterpret_cast<int(__fastcall*)(void*)>(getter)(options);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void warnTooltipOnce()
{
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        log().warn(L"DebugKeys: advanced tooltip data could not be read or written");
    }
}

std::vector<int> loadKey(const nlohmann::json& section, const char* name, int key)
{
    std::vector<int> combo{VK_F3, key};
    if (const auto it = section.find(name); it != section.end() && it->is_array()) {
        combo.clear();
        for (const auto& value : *it) {
            if (value.is_number_integer()) {
                combo.push_back(value.get<int>());
            }
        }
    }
    return combo;
}

}

DebugKeys& DebugKeys::instance()
{
    static DebugKeys module;
    return module;
}

void DebugKeys::onScansReady()
{
    Scanner& scanner = Scanner::instance();
    const std::byte* const site = scanner.address(Target::PauseMenuOnFocusLostSite);
    m_boolOptionSet = scanner.address(Target::BoolOptionSet);
    const std::byte* const maxDamageSite = scanner.address(Target::ItemMaxDamageSlotSite);
    m_damageValue = scanner.address(Target::ItemStackDamageValue);
    if (maxDamageSite != nullptr && memory::isReadable(maxDamageSite + 6, sizeof(m_maxDamageSlot))) {
        std::memcpy(&m_maxDamageSlot, maxDamageSite + 6, sizeof(m_maxDamageSlot));
        if (m_maxDamageSlot <= 0 || m_maxDamageSlot >= 0x1000 || m_maxDamageSlot % 8 != 0) {
            m_maxDamageSlot = 0;
        }
    }
    if (site != nullptr && memory::isReadable(site, 42)) {
        std::memcpy(&m_clientOptionsSlot, site + 10, sizeof(m_clientOptionsSlot));
        std::memcpy(&m_optionLookupSlot, site + 26, sizeof(m_optionLookupSlot));
        std::memcpy(&m_pauseOptionId, site + 38, sizeof(m_pauseOptionId));
    }
    const std::byte* const smooth = scanner.address(Target::SmoothLightingGetter);
    if (smooth != nullptr && memory::isReadable(smooth + 33, sizeof(m_smoothLightingId))) {
        std::memcpy(&m_smoothLightingId, smooth + 33, sizeof(m_smoothLightingId));
    }
    std::byte* const attachPos = scanner.address(Target::ActorAttachPos);
    std::int32_t rotationField = 0;
    if (attachPos != nullptr && memory::isReadable(attachPos + 0x3A6, 7) && attachPos[0x3A6] == std::byte{0x4C}
        && attachPos[0x3A7] == std::byte{0x8B} && attachPos[0x3A8] == std::byte{0x81}) {
        std::memcpy(&rotationField, attachPos + 0x3A9, sizeof(rotationField));
        if (rotationField <= 0 || rotationField > 0x1000 || rotationField % 8 != 0) rotationField = 0;
    }
    debugworld::setActorFunctions(attachPos, scanner.address(Target::ViewVector), rotationField);
    const std::byte* const profanity = scanner.address(Target::ProfanityFilterGetter);
    if (profanity != nullptr && memory::isReadable(profanity + 33, sizeof(m_profanityFilterId))) {
        std::memcpy(&m_profanityFilterId, profanity + 33, sizeof(m_profanityFilterId));
    }
    m_viewPerspective = scanner.address(Target::ViewPerspective);
    m_clearMessages = scanner.address(Target::GuiDataClearMessages);
    const std::byte* const guiSite = scanner.address(Target::GuiDataFieldSite);
    if (guiSite != nullptr && memory::isReadable(guiSite + 3, sizeof(m_guiDataField))) {
        std::memcpy(&m_guiDataField, guiSite + 3, sizeof(m_guiDataField));
        if (m_guiDataField <= 0 || m_guiDataField > 0x10000) m_guiDataField = 0;
    }
    const std::byte* const pauseSite = scanner.address(Target::GamePauseCallSite);
    if (pauseSite != nullptr && memory::isReadable(pauseSite, 0x1c)
        && pauseSite[9] == std::byte{0xE8} && pauseSite[0x17] == std::byte{0xE8}) {
        m_pauseGameA = memory::ripTarget(pauseSite, 10);
        m_pauseGameB = memory::ripTarget(pauseSite, 0x18);
        if (!memory::inGameModule(m_pauseGameA) || !memory::inGameModule(m_pauseGameB)
            || !memory::isExecutable(m_pauseGameA, 1) || !memory::isExecutable(m_pauseGameB, 1)) {
            m_pauseGameA = m_pauseGameB = nullptr;
        }
    }
    const std::byte* const commandSite = scanner.address(Target::ChatCommandRunSite);
    if (commandSite != nullptr && memory::isReadable(commandSite, 0x32) && commandSite[0x2D] == std::byte{0xE8}) {
        m_chatCommand = memory::ripTarget(commandSite, 0x2E);
        if (!memory::inGameModule(m_chatCommand) || !memory::isExecutable(m_chatCommand, 1)) m_chatCommand = nullptr;
    }
}

bool DebugKeys::active()
{
    return DebugScreen::instance().enabled();
}

void DebugKeys::onUpdate()
{
    const bool location = m_locationKey.triggered();
    const bool version = m_versionKey.triggered();
    const bool data = m_dataKey.triggered();
    const bool pause = m_pauseKey.triggered();
    const bool reload = m_reloadKey.triggered();
    const bool advanced = m_advancedKey.triggered();
    const bool chunks = m_chunksKey.triggered();
    const bool hitbox = m_hitboxKey.triggered();
    const bool chunkBorder = m_chunkBorderKey.triggered();
    const bool clearChat = m_clearChatKey.triggered();
    const bool gamePause = m_pauseGameKey.triggered();
    const bool gameMode = m_gameModeKey.triggered();
    const bool spectator = m_spectatorKey.triggered();
    if (!active() || !input::isInGameplay()) {
        return;
    }
    unsigned actions = 0;
    if (version) actions |= kVersion;
    if (pause) actions |= kPause;
    if (reload) actions |= kReload;
    if (chunks) actions |= kChunks;
    if (advanced) actions |= kAdvanced;
    if (hitbox) actions |= kHitbox;
    if (chunkBorder) actions |= kChunkBorder;
    if (clearChat) actions |= kClearChat;
    if (gamePause) actions |= kGamePause;
    if (gameMode) actions |= kGameMode;
    if (spectator) actions |= kSpectator;
    if (actions != 0) {
        m_actions.fetch_or(actions, std::memory_order_release);
    }
    unsigned samples = 0;
    const std::uint64_t now = GetTickCount64();
    if (location) {
        samples |= DebugScreen::kKeyLocation;
        m_locationAt.store(now, std::memory_order_relaxed);
        m_locationSampleSeen.store(DebugScreen::instance().keySampleGeneration(), std::memory_order_relaxed);
    }
    if (data) {
        samples |= DebugScreen::kKeyData;
        m_dataAt.store(now, std::memory_order_relaxed);
        m_dataSampleSeen.store(DebugScreen::instance().keySampleGeneration(), std::memory_order_relaxed);
    }
    if (samples != 0) {
        m_samplePending.fetch_or(samples, std::memory_order_release);
        DebugScreen::instance().requestKeySample(samples);
    }
}

void DebugKeys::onPlayerViewUpdate()
{
    const std::uint64_t viewNow = GetTickCount64();
    if (m_lastViewAt != 0 && viewNow - m_lastViewAt > 1500) {
        m_pausedByKey.store(false, std::memory_order_release);
        m_hitBoxTrack.clear();
    }
    m_lastViewAt = viewNow;
    void* const player = GameData::instance().player();
    if (player != m_lastPlayer) {
        m_hitBoxTrack.clear();
        m_pausedByKey.store(false, std::memory_order_release);
        m_lastPlayer = player;
    }
    m_perspectiveKnown = m_clientOptionsSlot != 0
        && viewPerspectiveRaw(hooks::gameClientInstance(), m_clientOptionsSlot, m_viewPerspective, m_perspective);
    if (m_perspectiveKnown) {
        boxes::noteViewPerspective(m_perspective);
    } else {
        m_perspective = 0;
    }
    m_perspectiveUnknownFrames = m_perspectiveKnown ? 0 : m_perspectiveUnknownFrames + 1;
    if (m_perspectiveUnknownFrames == 300) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            log().warn(L"DebugKeys: the view perspective could not be read; your own hitbox is not drawn and "
                       L"boxes use the first person camera direction");
        }
    }
    if (!active()) {
        if (m_pausedByKey.load(std::memory_order_acquire)) {
            gamemodewheel::closeOurs();
            setGamePaused(false);
        }
        m_hitBoxTrack.clear();
        m_actions.store(0, std::memory_order_relaxed);
        m_selectedMode.store(-1, std::memory_order_relaxed);
        m_samplePending.store(0, std::memory_order_relaxed);
        DebugScreen::instance().takeKeySampleFlags();
        if (m_linesPublished) {
            worldmesh::setDebugLines({});
            m_linesPublished = false;
        }
        return;
    }
    const unsigned actions = m_actions.exchange(0, std::memory_order_acq_rel);
    if ((actions & kVersion) != 0) dumpVersion();
    if ((actions & kPause) != 0) togglePause();
    if ((actions & kReload) != 0) debugMessage("Reloading UI definitions");
    if ((actions & kChunks) != 0) reloadChunks();
    if ((actions & kAdvanced) != 0) {
        const bool shown = !m_advancedShown.load(std::memory_order_relaxed);
        m_advancedShown.store(shown, std::memory_order_release);
        debugMessage(shown ? "Advanced tooltips: shown" : "Advanced tooltips: hidden");
        uiprobe::markSettingsDirty();
    }
    if ((actions & kHitbox) != 0) {
        const bool shown = !m_hitboxesShown.load(std::memory_order_relaxed);
        m_hitboxesShown.store(shown, std::memory_order_release);
        debugMessage(shown ? "Hitboxes: shown" : "Hitboxes: hidden");
        uiprobe::markSettingsDirty();
    }
    if ((actions & kChunkBorder) != 0) {
        const bool shown = !m_chunkBordersShown.load(std::memory_order_relaxed);
        m_chunkBordersShown.store(shown, std::memory_order_release);
        debugMessage(shown ? "Chunk borders: shown" : "Chunk borders: hidden");
        uiprobe::markSettingsDirty();
    }
    if ((actions & kClearChat) != 0) clearChat();
    if ((actions & kGamePause) != 0) {
        if (m_pausedByKey.load(std::memory_order_acquire)) {
            gamemodewheel::closeOurs();
            setGamePaused(false);
        } else {
            pauseWithScreen();
        }
    }
    if (m_pauseWheelClosed.exchange(false, std::memory_order_acq_rel)) setGamePaused(false);
    if ((actions & kGameMode) != 0 && !writes::blocked("DebugScreen:gameModeWheel"))
        gamemodewheel::open(gamemodewheel::Purpose::GameMode);
    if ((actions & kSpectator) != 0) changeMode(GameModeState::instance().spectatorTarget());
    const int selected = m_selectedMode.exchange(-1, std::memory_order_acq_rel);
    if (selected >= 0) changeMode(selected);
    updateDebugLines();

    const unsigned signaled = DebugScreen::instance().takeKeySampleFlags();
    const std::uint64_t generation = DebugScreen::instance().keySampleGeneration();
    unsigned ready = 0;
    if ((signaled & DebugScreen::kKeyLocation) != 0
        && generation > m_locationSampleSeen.load(std::memory_order_relaxed)) ready |= DebugScreen::kKeyLocation;
    if ((signaled & DebugScreen::kKeyData) != 0
        && generation > m_dataSampleSeen.load(std::memory_order_relaxed)) ready |= DebugScreen::kKeyData;
    const unsigned pending = m_samplePending.fetch_and(~ready, std::memory_order_acq_rel) & ready;
    const std::uint64_t now = GetTickCount64();
    if ((pending & DebugScreen::kKeyLocation) != 0
        && now - m_locationAt.load(std::memory_order_relaxed) <= 500) {
        copyLocation();
    }
    if ((pending & DebugScreen::kKeyData) != 0
        && now - m_dataAt.load(std::memory_order_relaxed) <= 500) {
        copyData();
    }
    unsigned expired = 0;
    const unsigned waiting = m_samplePending.load(std::memory_order_acquire);
    if ((waiting & DebugScreen::kKeyLocation) != 0
        && now - m_locationAt.load(std::memory_order_relaxed) > 500) expired |= DebugScreen::kKeyLocation;
    if ((waiting & DebugScreen::kKeyData) != 0
        && now - m_dataAt.load(std::memory_order_relaxed) > 500) expired |= DebugScreen::kKeyData;
    if (expired != 0) m_samplePending.fetch_and(~expired, std::memory_order_release);
}

void DebugKeys::updateDebugLines()
{
    void* const player = GameData::instance().player();
    const bool hitboxes = m_hitboxesShown.load(std::memory_order_acquire);
    const bool borders = m_chunkBordersShown.load(std::memory_order_acquire);
    if (!hitboxes && !borders) {
        if (m_linesPublished) {
            worldmesh::setDebugLines({});
            m_linesPublished = false;
        }
        return;
    }
    std::vector<worldmesh::DebugLine> lines;
    const bool known = m_perspectiveKnown;
    const int perspective = m_perspective;
    if (hitboxes) {
        debuglines::HitBox boxes[512]{};
        const int count = debugworld::collectHitBoxes(boxes, 512, 64.0F, known && perspective != 0);
        const auto shown = m_hitBoxTrack.update(std::span(boxes, count), GetTickCount64());
        for (const auto& box : shown) debuglines::hitBoxLines(box, lines);
    } else {
        m_hitBoxTrack.clear();
    }
    if (borders && player != nullptr) {
        float x = 0, y = 0, z = 0;
        if (GameData::instance().playerBoxFeet(x, y, z)) {
            const auto section = [](float value) {
                return static_cast<int>(std::floor(static_cast<double>(value) / 16.0)) * 16;
            };
            int minY = 0, maxY = 0;
            if (debugworld::worldHeightAt(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(z)), minY, maxY)
                && maxY > minY)
                debuglines::chunkBorderLines(section(x), section(y), section(z), minY, maxY, lines);
        }
    }
    worldmesh::setDebugLines(std::move(lines));
    m_linesPublished = true;
}

void DebugKeys::clearChat()
{
    void* ci = hooks::gameClientInstance();
    if (ci == nullptr || m_guiDataField == 0 || m_clearMessages == nullptr
        || !memory::isReadable(static_cast<std::byte*>(ci) + m_guiDataField, sizeof(void*))) return;
    if (!clearChatRaw(ci, m_guiDataField, reinterpret_cast<ClearMessagesFn>(m_clearMessages))) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) log().warn(L"DebugKeys: GuiData::clearMessages failed");
        return;
    }
    bool changed = false;
    if (m_profanityFilterId == 0 || !flipOptionAndBack(m_profanityFilterId, changed)) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) log().warn(L"DebugKeys: could not refresh the HUD chat (profanity filter option)");
    }
}

void DebugKeys::setGamePaused(bool wanted)
{
    void* ci = hooks::gameClientInstance();
    if (ci == nullptr || m_pauseGameA == nullptr || m_pauseGameB == nullptr
        || !memory::isReadable(static_cast<std::byte*>(ci) + 8, sizeof(void*))) return;
    if (pauseGameRaw(ci, reinterpret_cast<GamePauseFn>(m_pauseGameA),
                     reinterpret_cast<GamePauseFn>(m_pauseGameB), wanted)) {
        m_pausedByKey.store(wanted, std::memory_order_release);
    } else {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) log().warn(L"DebugKeys: pause model control is unavailable");
    }
}

void DebugKeys::pauseWithScreen()
{
    if (writes::blocked("DebugScreen:gameModeWheel") || !gamemodewheel::open(gamemodewheel::Purpose::Pause)) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) log().warn(L"DebugKeys: F3+Esc could not open its screen; the game is not paused");
        debugMessage("Could not pause the game");
        return;
    }
    m_pauseWheelClosed.store(false, std::memory_order_release);
    setGamePaused(true);
    if (!m_pausedByKey.load(std::memory_order_acquire)) gamemodewheel::closeOurs();
}

void DebugKeys::changeMode(int mode)
{
    if (!gamemode::isSelectable(mode) || mode == GameModeState::instance().currentMode()) return;
    void* const ci = hooks::gameClientInstance();
    const std::string text = gamemode::command(mode);
    bool handled = false;
    if (ci == nullptr || m_chatCommand == nullptr
        || !runChatCommandRaw(ci, reinterpret_cast<ChatCommandFn>(m_chatCommand), &text, handled)) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) log().warn(L"DebugKeys: could not run the game mode command");
    }
}

void DebugKeys::onItemHoverText(const void* stack, void* out)
{
    if (!m_advancedShown.load(std::memory_order_acquire) || m_stopped.load(std::memory_order_acquire) || !active()) return;
    if (stack == nullptr || out == nullptr || m_maxDamageSlot == 0 || m_damageValue == nullptr
        || !gamestring::available()) {
        warnTooltipOnce();
        return;
    }
    std::string original;
    if (!gamestring::read(out, original)) {
        warnTooltipOnce();
        return;
    }
    const std::string id = containerui::itemName(stack);
    if (id.empty()) {
        warnTooltipOnce();
        return;
    }
    int max = 0;
    int damage = 0;
    if (!durabilityRaw(stack, m_maxDamageSlot, m_damageValue, max, damage)) {
        warnTooltipOnce();
        return;
    }
    std::int32_t color = 0;
    const std::int32_t dye = containerui::nbtInt(stack, "customColor", color)
                                 ? static_cast<std::int32_t>(static_cast<std::uint32_t>(color) & 0xFFFFFFu) : -1;
    original += advancedtooltip::suffix(id, max, damage, dye, containerui::nbtTagCount(stack));
    if (!gamestring::assign(out, original)) warnTooltipOnce();
}

void DebugKeys::copyLocation()
{
    debugworld::Sample world{};
    if (!debugworld::read(world) || !world.hasDimension) return;
    GameData& game = GameData::instance();
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (!game.playerBoxFeet(x, y, z) || !game.hasPlayerView()) return;
    const PlayerView view = game.playerView();
    const std::string command = debugkeys::locationCommand(world.dimensionId, x, y, z,
                                                             view.yaw, view.pitch);
    if (!command.empty() && putClipboard(command)) {
        debugMessage("Copied location to clipboard");
    }
}

void DebugKeys::dumpVersion()
{
    dbgtext::Sample sample{};
    DebugScreen::instance().fillVersionInfo(sample);
    debugMessage("Client version info:");
    if (sample.gameVersion.available) debugMessage("Minecraft: " + std::string(sample.gameVersion.value));
    if (sample.modName.available && sample.launchedVersion.available) {
        debugMessage(std::string(sample.modName.value) + ": " + sample.launchedVersion.value);
    }
    if (sample.serverBrand.available) debugMessage("Server: " + std::string(sample.serverBrand.value));
}

void DebugKeys::copyData()
{
    debugworld::Sample world{};
    if (!debugworld::read(world)) return;
    std::string command;
    const char* message = nullptr;
    if (world.hasTarget) {
        std::array<std::string_view, debugworld::kMaxStates> states{};
        const int count = std::clamp(world.targetStateCount, 0, debugworld::kMaxStates);
        for (int i = 0; i < count; ++i) states[i] = world.targetStates[i];
        command = debugkeys::setblockCommand(world.targetX, world.targetY, world.targetZ,
                                             world.targetName, std::span(states.data(), count));
        message = "Copied client-side block data to clipboard";
    } else if (world.hasEntityTarget && world.hasEntityHit) {
        command = debugkeys::summonCommand(world.entityName, world.entityHitX, world.entityHitY,
                                           world.entityHitZ);
        message = "Copied client-side entity data to clipboard";
    }
    if (!command.empty() && putClipboard(command)) debugMessage(message);
}

void DebugKeys::togglePause()
{
    if (m_clientOptionsSlot == 0 || m_optionLookupSlot == 0 || m_pauseOptionId == 0
        || m_boolOptionSet == nullptr) return;
    void* options = nullptr;
    bool before = false, after = false;
    if (!setBoolOptionRaw(hooks::gameClientInstance(), m_clientOptionsSlot, m_optionLookupSlot,
                          m_pauseOptionId, m_boolOptionSet, -1, options, before, after)) return;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        log().info(L"DebugKeys: pause option Options={} read={} wrote={}", options, before, after);
    }
    if (before != after) {
        debugMessage(after ? "Pause menu on focus lost: enabled"
                           : "Pause menu on focus lost: disabled");
    }
}

void DebugKeys::applyReloadKey()
{
    m_reloadKey.set(active() ? m_reloadCombo : std::vector<int>{});
}

void DebugKeys::reloadChunks()
{
    bool changed = false;
    if (m_smoothLightingId != 0 && flipOptionAndBack(m_smoothLightingId, changed) && changed)
        debugMessage("Reloading all chunks");
}

bool DebugKeys::flipOptionAndBack(std::int32_t optionId, bool& changed)
{
    changed = false;
    if (m_clientOptionsSlot == 0 || m_optionLookupSlot == 0 || m_boolOptionSet == nullptr) return false;
    void* options = nullptr;
    bool original = false, flipped = false, restored = false, again = false;
    if (!setBoolOptionRaw(hooks::gameClientInstance(), m_clientOptionsSlot, m_optionLookupSlot, optionId,
                          m_boolOptionSet, -1, options, original, flipped)) return false;
    const bool back = setBoolOptionRaw(hooks::gameClientInstance(), m_clientOptionsSlot, m_optionLookupSlot, optionId,
                                       m_boolOptionSet, original ? 1 : 0, options, again, restored);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 4 || !back || restored != original) {
        log().info(L"DebugKeys: option {:#x} {} -> {} -> {} ({})", optionId, original, flipped, restored,
                   back && restored == original ? L"restored" : L"NOT restored");
    }
    changed = flipped != original;
    return back && restored == original;
}

void DebugKeys::onEnabledChanged(bool on)
{
    applyReloadKey();
    if (!on) {
        m_actions.store(0, std::memory_order_relaxed);
        m_samplePending.store(0, std::memory_order_relaxed);
        m_selectedMode.store(-1, std::memory_order_relaxed);
        worldmesh::setDebugLines({});
    }
}

void DebugKeys::shutdown()
{
    if (m_pausedByKey.load(std::memory_order_acquire)) {
        m_actions.fetch_or(kGamePause, std::memory_order_release);
        for (int i = 0; i < 50 && m_pausedByKey.load(std::memory_order_acquire); ++i) Sleep(10);
        if (m_pausedByKey.load(std::memory_order_acquire)) log().warn(L"DebugKeys: game is still paused at unload");
    }
    worldmesh::setDebugLines({});
    m_selectedMode.store(-1, std::memory_order_relaxed);
    m_stopped.store(true, std::memory_order_release);
    m_actions.store(0, std::memory_order_relaxed);
    m_samplePending.store(0, std::memory_order_relaxed);
}

void DebugKeys::loadKeys(const nlohmann::json& section)
{
    m_locationKey.set(loadKey(section, "locationKeys", 'C'));
    m_versionKey.set(loadKey(section, "versionKeys", 'V'));
    m_dataKey.set(loadKey(section, "dataKeys", 'I'));
    m_pauseKey.set(loadKey(section, "pauseKeys", 'P'));
    m_advancedKey.set(loadKey(section, "advancedKeys", 'H'));
    m_chunksKey.set(loadKey(section, "chunksKeys", 'A'));
    m_hitboxKey.set(loadKey(section, "hitboxKeys", 'B'));
    m_chunkBorderKey.set(loadKey(section, "chunkBorderKeys", 'G'));
    m_clearChatKey.set(loadKey(section, "clearChatKeys", 'D'));
    m_pauseGameKey.set(loadKey(section, "pauseGameKeys", VK_ESCAPE));
    m_gameModeKey.set(loadKey(section, "gameModeKeys", VK_F4));
    m_spectatorKey.set(loadKey(section, "spectatorKeys", 'N'));
    m_reloadCombo = loadKey(section, "reloadKeys", 'T');
    applyReloadKey();
    const auto readFlag = [&section](const char* key) {
        const auto it = section.find(key);
        return it != section.end() && it->is_boolean() && it->get<bool>();
    };
    m_advancedShown.store(readFlag("advancedTooltips"), std::memory_order_release);
    m_hitboxesShown.store(readFlag("showHitboxes"), std::memory_order_release);
    m_chunkBordersShown.store(readFlag("showChunkBorders"), std::memory_order_release);
}

void DebugKeys::saveKeys(nlohmann::json& section) const
{
    section["locationKeys"] = m_locationKey.combo();
    section["versionKeys"] = m_versionKey.combo();
    section["dataKeys"] = m_dataKey.combo();
    section["pauseKeys"] = m_pauseKey.combo();
    section["advancedKeys"] = m_advancedKey.combo();
    section["chunksKeys"] = m_chunksKey.combo();
    section["hitboxKeys"] = m_hitboxKey.combo();
    section["chunkBorderKeys"] = m_chunkBorderKey.combo();
    section["clearChatKeys"] = m_clearChatKey.combo();
    section["pauseGameKeys"] = m_pauseGameKey.combo();
    section["gameModeKeys"] = m_gameModeKey.combo();
    section["spectatorKeys"] = m_spectatorKey.combo();
    section["reloadKeys"] = m_reloadCombo;
    section["advancedTooltips"] = m_advancedShown.load(std::memory_order_acquire);
    section["showHitboxes"] = m_hitboxesShown.load(std::memory_order_acquire);
    section["showChunkBorders"] = m_chunkBordersShown.load(std::memory_order_acquire);
}

}
