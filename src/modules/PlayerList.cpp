#include "modules/PlayerList.h"

#include "core/Logger.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "game/BlockWrite.h"
#include "game/ContainerUi.h"
#include "game/GameData.h"
#include "game/GameModeIds.h"
#include "game/PlayerListMemory.h"
#include "game/PngWrite.h"
#include "game/UiProbe.h"
#include "input/Foreground.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "memory/Signatures.h"
#include "modules/DebugScreen.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace tsukuyomi {
namespace {
namespace cui = containerui;
namespace pl = playerlist;
constexpr std::size_t kBlockSourceLevel = 0x20;
constexpr std::size_t kSlots[3] = {0x9f8, 0xb58, 0x1f0};
constexpr std::uint32_t kGameTypeComponent = 0x88D3EDDFu;
constexpr std::uint32_t kAttributesComponent = 0xFD3B0613u;
constexpr std::size_t kAttributesSize = 0x50;
constexpr std::size_t kAttributeId = 4;
static_assert(pl::kHeartsModeSlot < cui::kPersistentSlots);
static_assert(pl::kHealthSlot + pl::kMaxPlayers <= cui::kPersistentSlots);
static_assert(pl::kFaceSlot + pl::kMaxPlayers <= cui::kPersistentTextSlots);
constexpr auto kFaceFileAge = std::chrono::hours(24 * 7);

void writeBool(int slot, bool value)
{
    if (auto* p = cui::persistentBool(slot)) *p = value ? 1 : 0;
}
void writeFloat(int slot, float value)
{
    if (auto* p = cui::persistentFloat(slot)) *p = value;
}
void writeText(int slot, const std::string& value)
{
    cui::writePersistentText(slot, value.data(), value.size());
}
bool readPointer(const void* base, std::size_t offset, const void*& out)
{
    return memory::plausiblePointer(base)
        && memory::copyGuarded(static_cast<const char*>(base) + offset, &out, sizeof(out))
        && memory::plausiblePointer(out);
}
LONG accessFilter(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
const void* fetchActor(const void* level, const void* expectedVtable, const void* expectedFn, std::int64_t id)
{
    __try {
        const auto* vtable = *static_cast<const void* const* const*>(level);
        if (vtable != expectedVtable || vtable[kSlots[2] / sizeof(void*)] != expectedFn) return nullptr;
        using Fetch = const void* (__fastcall*)(const void*, std::int64_t, bool);
        return reinterpret_cast<Fetch>(const_cast<void*>(vtable[kSlots[2] / sizeof(void*)]))(level, id, false);
    } __except (accessFilter(GetExceptionCode())) {
        return nullptr;
    }
}
}

PlayerList& PlayerList::instance() { static PlayerList module; return module; }

void PlayerList::prepareFaceDirectory()
{
    std::error_code error;
    const auto dir = paths::dataDir() / "heads";
    std::filesystem::create_directories(dir, error);
    if (error) {
        log().warn(L"PlayerList: cannot create the heads folder; heads are not shown");
        return;
    }
    const auto now = std::filesystem::file_time_type::clock::now();
    int removed = 0;
    for (const auto& item : std::filesystem::directory_iterator(dir, error)) {
        std::error_code itemError;
        const auto written = item.last_write_time(itemError);
        if (!itemError && item.path().extension() == ".png" && now - written > kFaceFileAge) {
            removed += std::filesystem::remove(item.path(), itemError) ? 1 : 0;
        }
    }
    const std::string path = toUtf8(dir.generic_wstring()) + "/0123456789abcdef.png";
    if (path.size() <= static_cast<std::size_t>(cui::kPersistentTextMax)) {
        m_faceDirectory = dir;
    } else {
        log().warn(L"PlayerList: the heads folder path is too long; heads are not shown");
    }
    if (removed > 0) log().info(L"PlayerList: removed {} old head images", removed);
}

std::string PlayerList::facePathFor(const playerlist::Entry& entry)
{
    if (entry.face.empty() || entry.faceSide <= 0 || m_faceDirectory.empty()) return {};
    const auto file = m_faceDirectory / pl::faceFileName(entry.face, entry.faceSide);
    const std::string path = toUtf8(file.generic_wstring());
    if (m_writtenFaces.count(path) == 0) {
        std::error_code error;
        if (!std::filesystem::exists(file, error)) {
            const auto png = png::encodeRgba(entry.face.data(), entry.faceSide, entry.faceSide);
            std::ofstream out(file, std::ios::binary);
            out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
            if (!out) {
                if (!m_faceWriteLogged) {
                    m_faceWriteLogged = true;
                    log().warn(L"PlayerList: cannot write a head image; that head is not shown");
                }
                return {};
            }
        }
        m_writtenFaces.insert(path);
    }
    return path;
}

void PlayerList::onScansReady()
{
    if (!cui::bindingsAvailable() || !cui::floatBindingsAvailable()
        || cui::persistentBool(pl::kRowSlot + pl::kMaxPlayers - 1) == nullptr
        || cui::persistentFloat(pl::kNameAlphaSlot + pl::kMaxPlayers - 1) == nullptr
        || !cui::writePersistentText(pl::kWidthScoreSlot, "", 0)) {
        log().warn(L"PlayerList: NOT usable (HUD bindings or persistent storage missing)");
        return;
    }
    writeFloat(pl::kBoxAlphaSlot, 128.0f / 255.0f);
    writeFloat(pl::kStripeAlphaSlot, 32.0f / 255.0f);
    writeFloat(pl::kGhostAlphaSlot, 0.0f);
    for (int i = 0; i < pl::kMaxPlayers; ++i) writeFloat(pl::kNameAlphaSlot + i, 1.0f);
    if (std::byte* const site = Scanner::instance().address(Target::HealthAttributeLoad)) {
        m_healthAttribute.store(memory::ripTarget(site, 8), std::memory_order_relaxed);
    }
    if (m_healthAttribute.load(std::memory_order_relaxed) == nullptr) {
        log().warn(L"PlayerList: the health attribute was not found; hearts are not shown");
    }
    prepareFaceDirectory();
    cui::addHudObserver(&PlayerList::onHudCreated);
    uiprobe::registerDefExtension("hud", "hud_content", "", pl::layoutJson());
    m_definitionRegistered.store(true, std::memory_order_relaxed);
    if (!m_enabledWasInConfig) setEnabled(true);
    log().info(L"PlayerList: ready (hold Tab; HUD appears in worlds entered after injection)");
}

PlayerList::LevelSlots PlayerList::inspectSlots(const void* vtable)
{
    if (const auto found = m_slots.find(vtable); found != m_slots.end()) return found->second;
    LevelSlots result;
    for (int i = 0; i < 3; ++i) {
        const void* fn = nullptr;
        unsigned char code[8]{};
        std::int32_t disp = 0;
        bool ok = readPointer(vtable, kSlots[i], fn) && memory::inGameModule(fn)
            && memory::copyGuarded(fn, code, sizeof(code)) && code[0] == 0x48 && code[1] == 0x8B
            && code[2] == (i == 2 ? 0x89 : 0x81) && code[7] == (i == 2 ? 0xE9 : 0xC3);
        if (ok && i != 2) {
            std::memcpy(&disp, code + 3, sizeof(disp));
            ok = disp > 0 && disp < 0x10000;
        }
        if (ok) {
            if (i == 0) result.players = disp;
            else if (i == 1) result.scoreboard = disp;
            else result.fetch = fn;
        } else if (!m_badSlotLogged[i]) {
            m_badSlotLogged[i] = true;
            log().warn(L"PlayerList: Level vtable slot +0x{:x} has an unexpected getter; value unavailable", kSlots[i]);
        }
    }
    m_slots.emplace(vtable, result);
    return result;
}

void PlayerList::onPlayerViewUpdate()
{
    if (!m_wantSample.load(std::memory_order_relaxed) || m_shuttingDown.load(std::memory_order_relaxed)) return;
    const auto now = GetTickCount64();
    if (m_lastSample != 0 && now - m_lastSample < 100) return;
    m_lastSample = now;
    Sample sample;
    sample.at = now;
    const void* level = nullptr;
    const void* vtable = nullptr;
    const void* map = nullptr;
    if (readPointer(blockwrite::renderRegion(), kBlockSourceLevel, level) && readPointer(level, 0, vtable)) {
        const LevelSlots slots = inspectSlots(vtable);
        std::vector<pl::mem::RawPlayer> raw(pl::mem::kMaxPlayerEntries);
        const int count = slots.players != 0 && readPointer(level, static_cast<std::size_t>(slots.players), map)
            ? pl::mem::readPlayerMap(map, raw.data(), static_cast<int>(raw.size())) : -1;
        if (count >= 0) {
            sample.readable = true;
            const void* const healthAttribute = m_healthAttribute.load(std::memory_order_relaxed);
            std::uint32_t healthId = 0;
            const bool healthKnown = healthAttribute != nullptr
                && memory::copyGuarded(static_cast<const char*>(healthAttribute) + kAttributeId, &healthId, sizeof(healthId));
            raw.resize(static_cast<std::size_t>(count));
            std::stable_sort(raw.begin(), raw.end(), [](const auto& a, const auto& b) { return a.actorId < b.actorId; });
            std::vector<std::int64_t> ids;
            ids.reserve(raw.size());
            for (const auto& player : raw) ids.push_back(player.actorId);
            std::array<bool, pl::mem::kMaxPlayerEntries> has{};
            std::array<int, pl::mem::kMaxPlayerEntries> scores{};
            const void* scoreboard = nullptr;
            const void* objective = nullptr;
            if (slots.scoreboard != 0 && readPointer(level, static_cast<std::size_t>(slots.scoreboard), scoreboard)) {
                const bool objectiveOk = pl::mem::findListObjective(scoreboard, &objective);
                sample.objective = objectiveOk && objective != nullptr;
                const bool scoresOk = objectiveOk && (!sample.objective
                    || pl::mem::readScores(scoreboard, objective, ids.data(), count, has.data(), scores.data()));
                if (!scoresOk && !m_scoreReadLogged) {
                    m_scoreReadLogged = true;
                    log().warn(L"PlayerList: scoreboard tables unreadable or over the 65536-entry limit; scores left blank");
                }
            }
            sample.entries.reserve(raw.size());
            for (int i = 0; i < count; ++i) {
                const auto& player = raw[i];
                const void* actor = slots.fetch != nullptr ? fetchActor(level, vtable, slots.fetch, player.actorId) : nullptr;
                std::int32_t mode = 0;
                const bool spectator = memory::plausiblePointer(actor)
                    && GameData::copyComponent(actor, kGameTypeComponent, sizeof(mode), &mode)
                    && mode == gamemode::kSpectator;
                pl::Entry entry{std::string(player.name, player.nameLength), spectator, has[i], scores[i]};
                std::uint8_t attributes[kAttributesSize]{};
                float current = 0.0f, maximum = 0.0f;
                if (healthKnown && memory::plausiblePointer(actor)
                    && GameData::copyComponent(actor, kAttributesComponent, sizeof(attributes), attributes)
                    && pl::mem::readAttribute(attributes, healthId, current, maximum)) {
                    entry.health = pl::healthPoints(current);
                }
                std::uint8_t face[pl::mem::kMaxFaceSide * pl::mem::kMaxFaceSide * 4];
                int side = 0;
                if (pl::mem::readFace(player.skin, face, pl::mem::kMaxFaceSide, side) && side > 0) {
                    entry.face.assign(face, face + static_cast<std::size_t>(side) * side * 4);
                    entry.faceSide = side;
                }
                sample.entries.push_back(std::move(entry));
            }
        }
    }
    const int count = static_cast<int>(sample.entries.size());
    const int state = (count << 2) | (sample.objective ? 2 : 0) | (sample.readable ? 1 : 0);
    if (state != m_lastPlayerCount) {
        m_lastPlayerCount = state;
        if (m_sampleLogs < 8) {
            ++m_sampleLogs;
            log().info(L"PlayerList: {} players (objective: {}, readable: {})", count,
                       sample.objective ? L"yes" : L"no", sample.readable ? L"yes" : L"no");
        }
    }
    std::lock_guard lock(m_sampleMutex);
    m_sample = std::move(sample);
}

void PlayerList::hide()
{
    writeBool(pl::kVisibleSlot, false);
    m_visible = false;
}

void PlayerList::publish(bool held, std::uint64_t now)
{
    if (!held || m_shuttingDown.load(std::memory_order_relaxed)) { hide(); return; }
    Sample sample;
    { std::lock_guard lock(m_sampleMutex); sample = m_sample; }
    if (!sample.readable || sample.at == 0 || now - sample.at > 1000) { hide(); return; }
    if (m_visible && now - m_lastPublish < 50) return;
    pl::sortEntries(sample.entries);
    if (sample.entries.size() > pl::kMaxPlayers) sample.entries.resize(pl::kMaxPlayers);
    const int count = static_cast<int>(sample.entries.size());
    if (DebugScreen::localServerRunning() && count <= 1 && !sample.objective) {
        if (!m_aloneLogged) {
            m_aloneLogged = true;
            log().info(L"PlayerList: hidden (local world, {} player, no list objective; same as JE)", count);
        }
        hide();
        return;
    }
    const auto arrangement = pl::arrange(count);
    bool heartsMode = false;
    if (!sample.objective) {
        for (const auto& entry : sample.entries) heartsMode = heartsMode || (!entry.spectator && entry.health >= 0);
    }
    std::string widestName, widestScore;
    int nameWidth = -1, scoreWidth = -1;
    for (const auto& entry : sample.entries) {
        const auto name = pl::displayName(entry);
        if (const int width = pl::estimateWidth(entry.name); width > nameWidth) { nameWidth = width; widestName = name; }
        if (entry.hasScore && !heartsMode) {
            const auto score = pl::scoreText(entry.score);
            if (const int width = pl::estimateWidth(score); width > scoreWidth) { scoreWidth = width; widestScore = score; }
        }
    }
    std::vector<std::string> facePaths(sample.entries.size());
    bool headColumn = false;
    for (std::size_t i = 0; i < sample.entries.size(); ++i) {
        facePaths[i] = facePathFor(sample.entries[i]);
        headColumn = headColumn || !facePaths[i].empty();
    }
    writeText(pl::kWidthNameSlot, widestName);
    writeText(pl::kWidthScoreSlot, widestScore);
    writeBool(pl::kScorePaddingSlot, scoreWidth >= 0);
    writeBool(pl::kHeadColumnSlot, headColumn);
    writeBool(pl::kHeartsModeSlot, heartsMode);
    for (int c = 0; c < pl::kMaxColumns; ++c) {
        const bool shown = pl::playerAt(arrangement, c, 0, count) >= 0;
        writeBool(pl::kColumnSlot + c, shown);
        if (c > 0) writeBool(pl::kGapSlot + c - 1, shown);
        for (int r = 0; r < pl::kMaxRows; ++r) {
            const int slot = c * pl::kMaxRows + r;
            const int index = pl::playerAt(arrangement, c, r, count);
            const pl::Entry* entry = index >= 0 ? &sample.entries[index] : nullptr;
            writeText(pl::kNameSlot + slot, entry != nullptr ? pl::displayName(*entry) : "");
            writeText(pl::kScoreSlot + slot, entry != nullptr && entry->hasScore && !entry->spectator
                ? pl::scoreText(entry->score) : "");
            writeFloat(pl::kNameAlphaSlot + slot, entry != nullptr && entry->spectator ? 144.0f / 255.0f : 1.0f);
            const std::string facePath = index >= 0 ? facePaths[static_cast<std::size_t>(index)] : std::string();
            writeText(pl::kFaceSlot + slot, facePath);
            writeBool(pl::kHeadRowSlot + slot, !facePath.empty());
            writeFloat(pl::kHealthSlot + slot, heartsMode && entry != nullptr && !entry->spectator && entry->health >= 0
                ? static_cast<float>(entry->health + 1) : 0.0f);
            writeBool(pl::kRowSlot + slot, entry != nullptr);
        }
    }
    m_lastPublish = now;
    if (!m_visible && m_shownLogs < 8) {
        ++m_shownLogs;
        log().info(L"PlayerList: shown {} players in {} x {} (scores: {}, hearts: {}, heads: {})", count,
                   arrangement.columns, arrangement.rows, scoreWidth >= 0 ? L"yes" : L"no",
                   heartsMode ? L"yes" : L"no", headColumn ? L"yes" : L"no");
    }
    writeBool(pl::kVisibleSlot, true);
    m_visible = true;
}

void PlayerList::onUpdate()
{
    const bool held = !m_shuttingDown.load(std::memory_order_relaxed) && enabled() && available()
        && !m_listKey.empty() && m_listKey.isDown() && input::isInGameplay();
    m_wantSample.store(held, std::memory_order_relaxed);
    std::lock_guard lock(m_publishMutex);
    publish(held, GetTickCount64());
}

void PlayerList::onHudCreated(void* ctrl)
{
    auto& self = instance();
    std::lock_guard lock(self.m_publishMutex);
    if (!self.available() || self.m_shuttingDown.load(std::memory_order_relaxed)) return;
    { std::lock_guard sampleLock(self.m_sampleMutex); self.m_sample = {}; }
    self.hide();
    for (int slot = pl::kColumnSlot; slot <= pl::kHeartsModeSlot; ++slot) writeBool(slot, false);
    for (int slot = pl::kNameSlot; slot < pl::kFaceSlot + pl::kMaxPlayers; ++slot) writeText(slot, "");
    for (int i = 0; i < pl::kMaxPlayers; ++i) writeFloat(pl::kHealthSlot + i, 0.0f);
    writeFloat(pl::kBoxAlphaSlot, 128.0f / 255.0f);
    writeFloat(pl::kStripeAlphaSlot, 32.0f / 255.0f);
    writeFloat(pl::kGhostAlphaSlot, 0.0f);
    for (int i = 0; i < pl::kMaxPlayers; ++i) writeFloat(pl::kNameAlphaSlot + i, 1.0f);
    int texts = 0, bools = 0, floats = 0;
    bools += cui::bindPersistentBool(ctrl, "#tk_tab_visible", pl::kVisibleSlot) ? 1 : 0;
    bools += cui::bindPersistentBool(ctrl, "#tk_tab_sp", pl::kScorePaddingSlot) ? 1 : 0;
    floats += cui::bindPersistentFloat(ctrl, "#tk_tab_box_alpha", pl::kBoxAlphaSlot) ? 1 : 0;
    floats += cui::bindPersistentFloat(ctrl, "#tk_tab_stripe_alpha", pl::kStripeAlphaSlot) ? 1 : 0;
    floats += cui::bindPersistentFloat(ctrl, "#tk_tab_ghost_alpha", pl::kGhostAlphaSlot) ? 1 : 0;
    texts += cui::bindPersistentText(ctrl, "#tk_tab_wn", pl::kWidthNameSlot) ? 1 : 0;
    texts += cui::bindPersistentText(ctrl, "#tk_tab_ws", pl::kWidthScoreSlot) ? 1 : 0;
    bools += cui::bindPersistentBool(ctrl, "#tk_tab_hc", pl::kHeadColumnSlot) ? 1 : 0;
    bools += cui::bindPersistentBool(ctrl, "#tk_tab_hm", pl::kHeartsModeSlot) ? 1 : 0;
    for (int c = 0; c < pl::kMaxColumns; ++c) {
        const auto suffix = std::to_string(c);
        bools += cui::bindPersistentBool(ctrl, ("#tk_tab_c" + suffix + "v").c_str(), pl::kColumnSlot + c) ? 1 : 0;
        if (c > 0) bools += cui::bindPersistentBool(ctrl, ("#tk_tab_g" + suffix + "v").c_str(), pl::kGapSlot + c - 1) ? 1 : 0;
    }
    char name[32]{};
    for (int i = 0; i < pl::kMaxPlayers; ++i) {
        pl::bindName('n', i, name, sizeof(name)); texts += cui::bindPersistentText(ctrl, name, pl::kNameSlot + i) ? 1 : 0;
        pl::bindName('s', i, name, sizeof(name)); texts += cui::bindPersistentText(ctrl, name, pl::kScoreSlot + i) ? 1 : 0;
        pl::bindName('v', i, name, sizeof(name)); bools += cui::bindPersistentBool(ctrl, name, pl::kRowSlot + i) ? 1 : 0;
        pl::bindName('a', i, name, sizeof(name)); floats += cui::bindPersistentFloat(ctrl, name, pl::kNameAlphaSlot + i) ? 1 : 0;
        pl::bindName('f', i, name, sizeof(name)); texts += cui::bindPersistentText(ctrl, name, pl::kFaceSlot + i) ? 1 : 0;
        pl::bindName('k', i, name, sizeof(name)); bools += cui::bindPersistentBool(ctrl, name, pl::kHeadRowSlot + i) ? 1 : 0;
        pl::bindName('h', i, name, sizeof(name)); floats += cui::bindPersistentFloat(ctrl, name, pl::kHealthSlot + i) ? 1 : 0;
    }
    if (self.m_bindLogs.fetch_add(1, std::memory_order_relaxed) == 0) {
        if (texts == 242 && bools == 171 && floats == 163)
            log().info(L"PlayerList: bound {} text / {} bool / {} float values to the HUD controller", texts, bools, floats);
        else log().warn(L"PlayerList: bound only {}/242 text / {}/171 bool / {}/163 float values", texts, bools, floats);
    }
}

void PlayerList::shutdown()
{
    m_shuttingDown.store(true, std::memory_order_relaxed);
    m_wantSample.store(false, std::memory_order_relaxed);
    std::lock_guard lock(m_publishMutex);
    hide();
    for (int i = 0; i < pl::kMaxPlayers; ++i) writeBool(pl::kRowSlot + i, false);
    cui::detachPersistentText();
}

MenuItem PlayerList::buildMenu()
{
    std::vector<MenuItem> children{enabledItem(), toggleKeyItem()};
    children.push_back(menu::keybind(L"List players key", [this] { return m_listKey.combo(); },
        [this](std::vector<int> combo) { m_listKey.set(std::move(combo)); }, {}));
    bindPad(children.back(), m_listKey);
    auto item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void PlayerList::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    m_enabledWasInConfig = section.contains("enabled");
    std::vector<int> combo{9};
    if (const auto it = section.find("listKeys"); it != section.end() && it->is_array()) {
        combo.clear();
        for (const auto& value : *it) if (value.is_number_integer()) combo.push_back(value.get<int>());
    }
    m_listKey.set(std::move(combo));
}

void PlayerList::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["listKeys"] = m_listKey.combo();
}
}
