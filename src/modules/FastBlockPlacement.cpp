#include "modules/FastBlockPlacement.h"
#include "input/GameButtons.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Strings.h"
#include "game/BlockRegistry.h"
#include "game/BuildOver.h"
#include "game/ContainerUi.h"
#include "game/DebugScreenWorld.h"
#include "game/FlexibleLogic.h"
#include "game/GameData.h"
#include "game/HeldItem.h"
#include "game/LookPlacement.h"
#include "hooks/Detours.h"
#include "input/Foreground.h"
#include "input/GameInput.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "modules/HandRestock.h"
#include "modules/ModuleManager.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>

namespace tsukuyomi {

namespace {

bool samePos(const std::int32_t a[3], const std::int32_t b[3])
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

const wchar_t* const kRestrictionNames[fastplace::kRestrictionCount]{
    L"Plane", L"Face", L"Column", L"Line", L"Layer", L"Diagonal"};
const wchar_t* const kModeKeyLabels[fastplace::kRestrictionCount]{
    L"Plane mode key", L"Face mode key", L"Column mode key", L"Line mode key", L"Layer mode key", L"Diagonal mode key"};
const char* const kModeKeyNames[fastplace::kRestrictionCount]{
    "planeModeKeys", "faceModeKeys", "columnModeKeys", "lineModeKeys", "layerModeKeys", "diagonalModeKeys"};

}

FastBlockPlacement& FastBlockPlacement::instance()
{
    static FastBlockPlacement module;
    return module;
}

const std::array<std::vector<int>, fastplace::kRestrictionCount>& FastBlockPlacement::defaultModeKeys()
{
    static const std::array<std::vector<int>, fastplace::kRestrictionCount> keys{
        std::vector<int>{'Z', '1'}, std::vector<int>{'Z', '2'}, std::vector<int>{'Z', '3'},
        std::vector<int>{'Z', '4'}, std::vector<int>{'Z', '6'}, std::vector<int>{'Z', '5'}};
    return keys;
}

void FastBlockPlacement::onUpdate()
{
    for (int i = 0; i < fastplace::kRestrictionCount; ++i) {
        if (!m_modeKeys[static_cast<std::size_t>(i)].triggered() || !input::isInGameplay()) continue;
        m_restriction.store(i, std::memory_order_relaxed);
        log().info(L"FastBlockPlacement: restriction mode set to {} by its key", kRestrictionNames[i]);
        ModuleManager::instance().postNotice("\xC2\xA7" "b[Tsukuyomi]" "\xC2\xA7" "r Set Placement Restriction mode to \xC2\xA7" "a"
                                             + toUtf8(kRestrictionNames[i]));
    }
}

bool FastBlockPlacement::suppressHotbarSlot(int slot) const
{
    if (slot < 0 || slot > 8) return false;
    const GameButtons& buttons = GameButtons::instance();
    for (const Hotkey& key : m_modeKeys) {
        const std::vector<int> combo = key.combo();
        if (combo.size() < 2 || combo.back() != '1' + slot) continue;
        bool prefixHeld = true;
        for (std::size_t i = 0; i + 1 < combo.size() && prefixHeld; ++i) prefixHeld = buttons.comboKeyHeld(combo[i]);
        if (prefixHeld) return true;
    }
    return false;
}

void FastBlockPlacement::onScansReady()
{
    helditem::resolve();
    buildover::resolve();
    m_useButton = GameButtons::instance().watchButton(gamebuttonlogic::button::buildOrInteract);
}

bool FastBlockPlacement::available() const
{
    const Scanner& scanner = Scanner::instance();
    return scanner.found(Target::BuildBlock) && scanner.found(Target::ServerLevelTick)
        && scanner.found(Target::HitResultAssign) && scanner.found(Target::HitResultPick) && helditem::ready()
        && buildover::ready();
}

MenuItem FastBlockPlacement::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    children.push_back(menu::choice(
        L"Restriction mode",
        std::vector<std::wstring>(std::begin(kRestrictionNames), std::end(kRestrictionNames)),
        [this] { return m_restriction.load(std::memory_order_relaxed); },
        [this](int at) { m_restriction.store(std::clamp(at, 0, fastplace::kRestrictionCount - 1), std::memory_order_relaxed); }));
    children.push_back(menu::text(
        L"Layer height",
        [this] { return std::format(L"{}", m_layerHeight.load(std::memory_order_relaxed)); },
        [this](std::wstring typed) {
            int value = 0;
            if (!parseTypedInt(typed, value)) return;
            m_layerHeight.store(std::clamp(value, fastplace::kMinLayerHeight, fastplace::kMaxLayerHeight),
                                std::memory_order_relaxed);
        }));
    for (int i = 0; i < fastplace::kRestrictionCount; ++i) {
        Hotkey* const key = &m_modeKeys[static_cast<std::size_t>(i)];
        children.push_back(menu::keybind(kModeKeyLabels[i], [key] { return key->combo(); },
            [key](std::vector<int> combo) { key->set(std::move(combo)); }, defaultModeKeys()[static_cast<std::size_t>(i)]));
        bindPad(children.back(), *key);
        children.back().hidden = true;
    }
    children.push_back(menu::toggle(
        L"Remember orientation", [this] { return m_rememberOrientation.load(std::memory_order_relaxed); },
        [this] { m_rememberOrientation.store(!m_rememberOrientation.load(std::memory_order_relaxed), std::memory_order_relaxed); }));

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void FastBlockPlacement::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    m_restriction.store(std::clamp(Config::getInt(section, "restrictionMode", static_cast<int>(fastplace::Restriction::Face)),
                                   0, fastplace::kRestrictionCount - 1),
                        std::memory_order_relaxed);
    m_layerHeight.store(std::clamp(Config::getInt(section, "layerHeight", 1), fastplace::kMinLayerHeight,
                                   fastplace::kMaxLayerHeight),
                        std::memory_order_relaxed);
    m_rememberOrientation.store(Config::getBool(section, "rememberOrientation", true), std::memory_order_relaxed);
    for (int i = 0; i < fastplace::kRestrictionCount; ++i) {
        std::vector<int> combo = defaultModeKeys()[static_cast<std::size_t>(i)];
        if (const auto it = section.find(kModeKeyNames[i]); it != section.end() && it->is_array()) {
            combo.clear();
            for (const auto& value : *it) if (value.is_number_integer()) combo.push_back(value.get<int>());
        }
        m_modeKeys[static_cast<std::size_t>(i)].set(std::move(combo));
    }
}

void FastBlockPlacement::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section.erase("mode");
    section.erase("distance");
    section.erase("placementsPerTick");
    section.erase("itemListType");
    section.erase("itemBlacklist");
    section.erase("itemWhitelist");
    section["restrictionMode"] = m_restriction.load(std::memory_order_relaxed);
    section["layerHeight"] = m_layerHeight.load(std::memory_order_relaxed);
    section["rememberOrientation"] = m_rememberOrientation.load(std::memory_order_relaxed);
    for (int i = 0; i < fastplace::kRestrictionCount; ++i) section[kModeKeyNames[i]] = m_modeKeys[static_cast<std::size_t>(i)].combo();
}

void FastBlockPlacement::onEnabledChanged(bool enabled)
{
    if (!enabled) {
        m_resetRequested.store(true, std::memory_order_release);
        m_holdId.fetch_add(1, std::memory_order_acq_rel);
    }
}

void FastBlockPlacement::applyReset()
{
    if (m_resetRequested.exchange(false, std::memory_order_acq_rel) && m_hold.active) {
        endHold(L"turned off");
    }
}

bool FastBlockPlacement::handReady(void* gameMode) const
{
    bool empty = false;
    return helditem::mainHandEmpty(gameMode, empty) && !empty;
}

bool FastBlockPlacement::readAim(std::int32_t pos[3], int& face, float rel[3])
{
    unsigned char hit[hooks::kHitResultBytes]{};
    void* const live = hooks::hitResultAddress();
    if (live == nullptr || !memory::copyGuarded(live, hit, sizeof(hit))) return false;
    int kind = -1;
    std::memcpy(&kind, hit + 0x18, sizeof(kind));
    face = hit[0x1c];
    if (kind != 0 || face < 0 || face > 5) return false;
    std::memcpy(pos, hit + 0x20, sizeof(std::int32_t) * 3);
    float point[3]{};
    std::memcpy(point, hit + 0x2c, sizeof(point));
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(point[i])) return false;
        rel[i] = std::clamp(point[i] - static_cast<float>(pos[i]), 0.0F, 1.0F);
    }
    return true;
}

bool FastBlockPlacement::placementCell(void* gameMode, const std::int32_t pos[3], int face, std::int32_t out[3])
{
    const int buildOver = buildover::canBuildOver(gameMode, pos);
    if (buildOver < 0) return false;
    if (buildOver == 1) std::copy_n(pos, 3, out);
    else flexible::offset(pos, face, out);
    return true;
}

bool FastBlockPlacement::vacant(void* gameMode, const std::int32_t pos[3])
{
    return buildover::canBuildOver(gameMode, pos) == 1;
}

bool FastBlockPlacement::usedByClick(void* gameMode, const std::int32_t pos[3], int face, const float click[3], bool sneaking)
{
    if (sneaking) return false;
    const void* block = debugworld::worldBlockAt(pos);
    if (block == nullptr) return false;
    const int used = blocks::clickUses(gameMode, block, pos, face, click);
    if (used < 0) {
        static int said = 0;
        if (said++ < 4) log().warn(L"FastBlockPlacement: could not ask the game whether a click uses {}, {}, {}; not clicking it", pos[0], pos[1], pos[2]);
    }
    return used != 0;
}

bool FastBlockPlacement::onBuildBlock(void* gameMode, void* blockPos, unsigned char face,
                                      unsigned char extra, bool simTick)
{
    applyReset();
    if (!enabled()) {
        return hooks::callBuildBlock(gameMode, blockPos, face, extra, simTick);
    }

    if (m_hold.active) {
        return false;
    }

    std::int32_t pos[3]{};
    if (face > 5 || !memory::copyGuarded(blockPos, pos, sizeof(pos))) {
        return hooks::callBuildBlock(gameMode, blockPos, face, extra, simTick);
    }
    const int buildOver = buildover::canBuildOver(gameMode, pos);
    const bool result = hooks::callBuildBlock(gameMode, blockPos, face, extra, simTick);
    if (result && buildOver >= 0) {
        startHold(gameMode, pos, face, buildOver == 1);
    } else if (result && m_holdLogs < kMaxHoldLogs) {
        ++m_holdLogs;
        log().warn(L"FastBlockPlacement: could not ask whether {}, {}, {} can be built over; not starting", pos[0], pos[1], pos[2]);
    }
    return result;
}

void FastBlockPlacement::startHold(void* gameMode, const std::int32_t pos[3], int face, bool buildOver)
{
    m_hold = Hold{};
    m_hold.active = true;
    m_hold.gameMode = gameMode;
    if (buildOver) std::copy_n(pos, 3, m_hold.first);
    else flexible::offset(pos, face, m_hold.first);
    m_hold.sideFirst = face;
    std::copy_n(m_hold.first, 3, m_hold.last);

    std::int32_t aim[3]{};
    int aimFace = -1;
    float rel[3]{};
    if (!readAim(aim, aimFace, rel) || !samePos(aim, pos) || aimFace != face) flexible::facePoint(face, 0.5F, rel);
    for (int i = 0; i < 3; ++i) {
        m_hold.clickInFirst[i] = std::clamp(static_cast<float>(pos[i] - m_hold.first[i]) + rel[i], 0.0F, 1.0F);
    }
    flexible::Look look;
    m_hold.horizontalFirst = flexible::horizontalFromYaw(lookplace::readLook(look) ? look.yaw
                                                                                    : GameData::instance().playerView().yaw);
    m_holdId.fetch_add(1, std::memory_order_acq_rel);
    if (m_holdLogs < kMaxHoldLogs) {
        ++m_holdLogs;
        log().info(L"FastBlockPlacement: first ({}, {}, {}) face {} facing {} ({}, layer height {})", m_hold.first[0],
                   m_hold.first[1], m_hold.first[2], face, m_hold.horizontalFirst,
                   kRestrictionNames[m_restriction.load(std::memory_order_relaxed)], m_layerHeight.load(std::memory_order_relaxed));
    }
}

void FastBlockPlacement::endHold(const wchar_t* why)
{
    if (m_hold.active && (m_hold.placed > 0 || m_hold.scheduled > 0) && m_holdLogs < kMaxHoldLogs) {
        ++m_holdLogs;
        log().info(L"FastBlockPlacement: {} after {} placed ({} at most in a tick), {} rotated", why, m_hold.placed,
                   m_maxPerTick, m_hold.scheduled);
    }
    m_hold = Hold{};
    m_budget = 0;
    m_maxPerTick = 0;
    m_holdId.fetch_add(1, std::memory_order_acq_rel);
}

void FastBlockPlacement::onClientTick()
{
    m_gameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    applyReset();
    m_budget = 0;
    if (!m_hold.active) return;
    if (!GameButtons::instance().buttonHeld(m_useButton)) {
        endHold(L"released");
        return;
    }
    if (!enabled() || !input::isInGameplay()) return;
    m_budget = kPlacementsPerTick;
}

bool FastBlockPlacement::wantsPick() const
{
    return m_budget > 0 && m_hold.active && GetCurrentThreadId() == m_gameThread.load(std::memory_order_relaxed);
}

void FastBlockPlacement::onPicked()
{
    applyReset();
    int placed = 0;
    while (m_budget > 0 && m_hold.active && enabled() && input::isInGameplay()) {
        if (!placeOnce()) break;
        --m_budget;
        ++placed;
        if (m_budget > 0 && !hooks::repickHitResult()) break;
    }
    m_budget = 0;
    if (placed > m_maxPerTick) m_maxPerTick = placed;
}

bool FastBlockPlacement::placeOnce()
{
    Hold& hold = m_hold;
    void* const gameMode = GameData::instance().gameMode();
    if (gameMode == nullptr || gameMode != hold.gameMode) {
        endHold(L"the game mode changed");
        return false;
    }
    if (lookplace::busy() || HandRestock::instance().itemMovePending()) return false;
    if (!handReady(gameMode)) return false;
    const std::string item = containerui::itemName(HandRestock::instance().heldStack());
    if (item.empty()) return false;

    std::int32_t pos[3]{};
    int face = -1;
    float rel[3]{};
    if (!readAim(pos, face, rel)) return false;
    std::int32_t cell[3]{};
    if (!placementCell(gameMode, pos, face, cell)) return false;
    if (samePos(cell, hold.last) || !vacant(gameMode, cell)) return false;
    if (!fastplace::allowed(static_cast<fastplace::Restriction>(m_restriction.load(std::memory_order_relaxed)),
                            m_layerHeight.load(std::memory_order_relaxed), cell, face, hold.first, hold.sideFirst)) {
        return false;
    }

    const bool sneaking = input::sneakHeldWithin(input::kSneakHoldGraceMs);
    std::int32_t support[3]{pos[0], pos[1], pos[2]};
    int supportFace = face;
    float click[3]{rel[0], rel[1], rel[2]};
    const bool remember = m_rememberOrientation.load(std::memory_order_relaxed);
    if (remember && !samePos(cell, pos)) {
        std::int32_t against[3]{};
        flexible::offset(cell, flexible::opposite(hold.sideFirst), against);
        float againstClick[3]{};
        fastplace::supportClick(hold.clickInFirst, hold.sideFirst, againstClick);
        if (samePos(against, pos)
            || (buildover::canBuildOver(gameMode, against) == 0
                && !usedByClick(gameMode, against, hold.sideFirst, againstClick, sneaking))) {
            std::copy_n(against, 3, support);
            supportFace = hold.sideFirst;
            std::copy_n(againstClick, 3, click);
        }
    }
    if (usedByClick(gameMode, support, supportFace, click, sneaking)) {
        if (!hold.saidUsed && m_holdLogs < kMaxHoldLogs) {
            ++m_holdLogs;
            log().info(L"FastBlockPlacement: not clicking {}, {}, {} (the game would use it; sneak to place against it)",
                       support[0], support[1], support[2]);
        }
        hold.saidUsed = true;
        return false;
    }

    flexible::Look real;
    if (remember && lookplace::readLook(real) && flexible::horizontalFromYaw(real.yaw) != hold.horizontalFirst) {
        lookplace::Request request;
        request.gameMode = gameMode;
        std::copy_n(support, 3, request.pos);
        request.face = static_cast<std::uint8_t>(supportFace);
        std::copy_n(click, 3, request.click);
        request.look = flexible::Look{real.pitch, flexible::yawOf(hold.horizontalFirst)};
        request.owner = "FastBlockPlacement";
        const std::array<std::int32_t, 3> target{cell[0], cell[1], cell[2]};
        const std::uint64_t id = m_holdId.load(std::memory_order_acquire);
        request.stillValid = [this, target, id, item] {
            void* const now = GameData::instance().gameMode();
            return enabled() && m_holdId.load(std::memory_order_acquire) == id && input::isInGameplay()
                && !HandRestock::instance().itemMovePending()
                && containerui::itemName(HandRestock::instance().heldStack()) == item && now != nullptr
                && vacant(now, target.data());
        };
        if (lookplace::schedule(std::move(request))) {
            ++hold.scheduled;
        } else if (!hold.saidRotation && m_holdLogs < kMaxHoldLogs) {
            ++m_holdLogs;
            log().warn(L"FastBlockPlacement: could not schedule the rotated placement at {}, {}, {}", cell[0], cell[1], cell[2]);
            hold.saidRotation = true;
        }
        return false;
    }

    if (!hooks::callBuildBlockAimedWithoutSwing(gameMode, support, static_cast<unsigned char>(supportFace), click, 0, false)) {
        return false;
    }
    std::copy_n(cell, 3, hold.last);
    ++hold.placed;
    return true;
}

}
