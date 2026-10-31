#include "modules/FlexiblePlacement.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Strings.h"
#include "game/BlockRegistry.h"
#include "game/BlockWrite.h"
#include "game/BuildOver.h"
#include "game/ClientChat.h"
#include "game/ContainerUi.h"
#include "game/DebugScreenWorld.h"
#include "game/DebugLines.h"
#include "game/GameData.h"
#include "game/LookPlacement.h"
#include "game/PlacementQuery.h"
#include "hooks/Detours.h"
#include "input/Foreground.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "modules/FastUseItem.h"
#include "modules/HandRestock.h"
#include "render/WorldMesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tsukuyomi {
namespace {
bool blockEmpty(void* gameMode, const std::int32_t pos[3], bool& empty)
{
    const int buildOver = buildover::canBuildOver(gameMode, pos);
    empty = buildOver == 1;
    return buildOver >= 0;
}
bool readHit(std::int32_t pos[3], int& face, float rel[3])
{
    unsigned char hit[hooks::kHitResultBytes]{};
    if (!hooks::readHitResult(hit)) return false;
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
using flexible::facePoint;
const char* directionName(int dir)
{
    constexpr const char* names[]{"down", "up", "north", "south", "west", "east"};
    return dir >= 0 && dir < 6 ? names[dir] : "?";
}
bool solidAt(void* gameMode, const std::int32_t pos[3])
{
    bool empty = false;
    return blockEmpty(gameMode, pos, empty) && !empty;
}
enum class HeldResult { Found, Unsupported, Failed };
HeldResult heldBlock(const std::string& item, placementquery::HeldBlock& held)
{
    if (!placementquery::readHeld(HandRestock::instance().heldStack(), held)) return HeldResult::Failed;
    if (held.type) return HeldResult::Found;
    static std::unordered_map<std::string, const void*> byName;
    static std::uint64_t byNameGeneration = 0;
    if (const auto generation = blockwrite::regionGeneration(); generation != byNameGeneration) {
        byName.clear();
        byNameGeneration = generation;
    }
    auto it = byName.find(item);
    if (it == byName.end()) {
        const void* type = nullptr;
        blocks::Table table;
        if (!blocks::resolve({item}, table)) return HeldResult::Failed;
        if (const auto found = table.find(item); found != table.end() && found->second)
            if (!memory::copyGuarded(static_cast<const std::byte*>(found->second) + 0x68, &type, sizeof(type))) return HeldResult::Failed;
        it = byName.emplace(item, type).first;
    }
    held.type = it->second; held.itemValue = 0;
    return held.type ? HeldResult::Found : HeldResult::Unsupported;
}
std::wstring statesText(const void* block)
{
    char name[blocks::kStateTextBytes]{};
    char states[8][blocks::kStateTextBytes]{};
    const int count = block ? blocks::statesOfBlock(block, states, 8) : 0;
    if (block) blocks::nameOfBlock(block, name, sizeof(name));
    std::string text = name;
    for (int i = 0; i < count; ++i) { text += i == 0 ? " [" : ", "; text += states[i]; }
    if (count > 0) text += "]";
    return toUtf16(text);
}
bool queryScenario(const placementquery::HeldBlock& held, const std::int32_t target[3], const flexible::Scenario& scenario,
                   const void*& out)
{
    out = nullptr;
    lookplace::ScopedLook look(scenario.look);
    if (!look.ok()) return false;
    return placementquery::query(held, GameData::instance().player(), target, static_cast<std::uint8_t>(scenario.face),
        scenario.click, out);
}
void gridPoint(const std::int32_t pos[3], int face, int facing, float h, float v, double out[3])
{
    float rel[3]{0.5F, v, 0.5F};
    if (face < 2) {
        if (face == 0) v = 1 - v;
        switch (facing) {
        case 2: rel[0] = h; rel[2] = 1 - v; break;
        case 3: rel[0] = 1 - h; rel[2] = v; break;
        case 4: rel[0] = 1 - v; rel[2] = 1 - h; break;
        case 5: rel[0] = v; rel[2] = h; break;
        default: break;
        }
    } else if (face < 4) rel[0] = face == 3 ? h : 1 - h;
    else rel[2] = face == 4 ? h : 1 - h;
    const int axis = face < 2 ? 1 : face < 4 ? 2 : 0;
    rel[axis] = (face & 1) ? 1.002F : -0.002F;
    for (int i = 0; i < 3; ++i) out[i] = static_cast<double>(pos[i]) + rel[i];
}
}
FlexiblePlacement& FlexiblePlacement::instance() { static FlexiblePlacement module; return module; }
bool FlexiblePlacement::available() const
{
    const auto& scanner = Scanner::instance();
    return scanner.found(Target::BuildBlock) && scanner.found(Target::PlayerView) && scanner.found(Target::HitResultAssign)
        && buildover::ready();
}
void FlexiblePlacement::onScansReady() { buildover::resolve(); }
flexible::Keys FlexiblePlacement::pressedKeys() const
{
    return {m_rotationKey.isDown(), m_offsetKey.isDown(), m_adjacentKey.isDown(),
        m_accurateInKey.isDown(), m_accurateReverseKey.isDown()};
}
MenuItem FlexiblePlacement::buildMenu()
{
    std::vector<MenuItem> children{enabledItem(), toggleKeyItem()};
    const wchar_t* labels[]{L"Rotation key", L"Offset key", L"Adjacent key", L"Accurate in key", L"Accurate reverse key"};
    Hotkey* keys[]{&m_rotationKey, &m_offsetKey, &m_adjacentKey, &m_accurateInKey, &m_accurateReverseKey};
    for (int i = 0; i < 5; ++i) {
        auto* key = keys[i];
        children.push_back(menu::keybind(labels[i], [key] { return key->combo(); },
            [key](std::vector<int> combo) { key->set(std::move(combo)); }));
        bindPad(children.back(), *key);
    }
    children.push_back(menu::toggle(L"Grid overlay", [this] { return m_gridOverlay.load(); },
        [this] { m_gridOverlay.store(!m_gridOverlay.load()); }));
    auto item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}
void FlexiblePlacement::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    const char* names[]{"rotationKeys", "offsetKeys", "adjacentKeys", "accurateInKeys", "accurateReverseKeys"};
    Hotkey* keys[]{&m_rotationKey, &m_offsetKey, &m_adjacentKey, &m_accurateInKey, &m_accurateReverseKey};
    for (int i = 0; i < 5; ++i) {
        std::vector<int> combo;
        const auto it = section.find(names[i]);
        if (it != section.end() && it->is_array()) {
            for (const auto& value : *it) if (value.is_number_integer()) combo.push_back(value.get<int>());
        } else if (i < 2) combo.push_back(i == 0 ? VK_LMENU : VK_LCONTROL);
        keys[i]->set(std::move(combo));
    }
    m_gridOverlay.store(Config::getBool(section, "gridOverlay", true));
}
void FlexiblePlacement::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["rotationKeys"] = m_rotationKey.combo(); section["offsetKeys"] = m_offsetKey.combo();
    section["adjacentKeys"] = m_adjacentKey.combo(); section["accurateInKeys"] = m_accurateInKey.combo();
    section["accurateReverseKeys"] = m_accurateReverseKey.combo(); section["gridOverlay"] = m_gridOverlay.load();
}
bool FlexiblePlacement::onBuildBlock(void* gameMode, void* blockPos, unsigned char face, unsigned char extra,
                                     bool simTick, bool& handled)
{
    handled = false;
    if (!enabled()) return false;
    const auto keys = pressedKeys();
    if (!keys.rotation && !keys.offset && !keys.adjacent && !keys.accurateIn && !keys.accurateReverse) return false;
    if (containerui::itemName(HandRestock::instance().heldStack()).empty()) return false;
    handled = true;
    if (m_queryFailed.load() || simTick || face > 5 || lookplace::busy() || HandRestock::instance().itemMovePending()) return false;
    std::int32_t pos[3]{}, placePos[3]{}, hitPos[3]{};
    if (!blockPos || !memory::copyGuarded(blockPos, pos, sizeof(pos))) return false;
    float rel[3]{};
    facePoint(face, 0.5F, rel);
    int hitFace = -1;
    float hitRel[3]{};
    if (readHit(hitPos, hitFace, hitRel) && std::equal(pos, pos + 3, hitPos)) std::copy_n(hitRel, 3, rel);
    flexible::Look real;
    if (!lookplace::readLook(real)) return false;
    const auto item = containerui::itemName(HandRestock::instance().heldStack());
    if (item.empty()) return false;
    bool empty = false;
    if (!blockEmpty(gameMode, pos, empty)) return false;
    if (empty) std::copy_n(pos, 3, placePos);
    else flexible::offset(pos, face, placePos);
    const auto plan = flexible::plan(placePos, face, rel, real, keys, item == "minecraft:observer");
    if (!blockEmpty(gameMode, plan.target, empty) || !empty) return false;
    lookplace::Request request;
    request.gameMode = gameMode; request.look = plan.look; request.owner = "FlexiblePlacement";
    const auto result = solve(plan, rel, real, item, request);
    if (result == Solved::Failed) {
        if (!m_queryFailed.exchange(true)) {
            std::lock_guard lock(m_noticeMutex);
            m_notices.emplace_back("[Tsukuyomi] FlexiblePlacement: placement query failed; placement stopped.");
        }
        return false;
    }
    if (result == Solved::Impossible) return false;
    if (result == Solved::Unsupported) {
        std::int32_t neighbor[3]{};
        flexible::offset(plan.target, flexible::opposite(plan.side), neighbor);
        if (!blockEmpty(gameMode, neighbor, empty)) return false;
        if (!empty) {
            std::copy_n(neighbor, 3, request.pos); request.face = static_cast<std::uint8_t>(plan.side);
            facePoint(plan.side, rel[1], request.click);
        } else {
            std::copy_n(plan.target, 3, request.pos); request.face = 1; facePoint(1, 0.5F, request.click);
        }
    }
    if (std::fabs(std::remainder(request.look.yaw - real.yaw, 360.0F)) <= 0.5F)
        return hooks::callOriginalBuildBlock(gameMode, request.pos, request.face, request.click, extra, false);
    const std::array<std::int32_t, 3> target{plan.target[0], plan.target[1], plan.target[2]};
    request.stillValid = [this, target, item, gameMode] {
        bool vacant = false;
        return enabled() && input::isInGameplay() && !HandRestock::instance().itemMovePending()
            && containerui::itemName(HandRestock::instance().heldStack()) == item
            && blockEmpty(gameMode, target.data(), vacant) && vacant;
    };
    lookplace::schedule(std::move(request));
    return false;
}
FlexiblePlacement::Solved FlexiblePlacement::solve(const flexible::Plan& plan, const float rel[3], flexible::Look real,
                                                    const std::string& item, lookplace::Request& request)
{
    placementquery::HeldBlock held;
    const auto kind=heldBlock(item,held);
    if (kind==HeldResult::Failed) return Solved::Failed;
    if (kind==HeldResult::Unsupported) return Solved::Unsupported;
    flexible::Scenario ideal;
    const void* want = nullptr;
    const float clickY = rel[1] >= 1.0F ? 0.25F : rel[1] <= 0.0F ? 0.75F : std::clamp(rel[1], 0.05F, 0.95F);
    ideal.look = plan.look; ideal.face = plan.side;
    flexible::offset(plan.target, flexible::opposite(plan.side), ideal.neighbor);
    facePoint(ideal.face, clickY, ideal.click);
    if (!queryScenario(held, plan.target, ideal, want)) return Solved::Failed;
    if (!want) return Solved::Impossible;
    struct Option { std::int32_t pos[3]{}; std::uint8_t face = 1; float click[3]{}; };
    std::vector<Option> options;
    const bool sneaking = FastUseItem::playerSneaking(request.gameMode);
    const auto add = [&](const std::int32_t neighbor[3], int face, float y) {
        float click[3]{};
        facePoint(face, y, click);
        for (const auto& one : options) if (std::equal(one.pos, one.pos + 3, neighbor) && one.click[1] == click[1]) return;
        if (!solidAt(request.gameMode, neighbor)) return;
        if (!sneaking && blocks::clickUses(request.gameMode, debugworld::worldBlockAt(neighbor), neighbor, face, click) != 0) return;
        Option one;
        std::copy_n(neighbor, 3, one.pos); one.face = static_cast<std::uint8_t>(face); std::copy_n(click, 3, one.click);
        options.push_back(one);
    };
    add(ideal.neighbor, ideal.face, clickY);
    for (const float y : {clickY, clickY < 0.5F ? 0.75F : 0.25F}) {
        for (int side = 0; side < 6; ++side) {
            std::int32_t neighbor[3]{};
            flexible::offset(plan.target, side, neighbor);
            add(neighbor, flexible::opposite(side), y);
        }
    }
    {
        Option air;
        std::copy_n(plan.target, 3, air.pos); air.face = 1; facePoint(1, 0.5F, air.click);
        options.push_back(air);
    }
    std::vector<flexible::Look> looks{real, {real.pitch, ideal.look.yaw}};
    for (int dir = 2; dir < 6; ++dir) looks.push_back({real.pitch, flexible::yawOf(dir)});
    const Option* chosen = nullptr;
    flexible::Look chosenLook;
    for (std::size_t l = 0; l < looks.size() && !chosen; ++l) {
        bool seen = false;
        for (std::size_t k = 0; k < l; ++k) seen = seen || std::fabs(std::remainder(looks[k].yaw - looks[l].yaw, 360.0F)) <= 0.5F;
        if (seen) continue;
        lookplace::ScopedLook look(looks[l]);
        if (!look.ok()) return Solved::Failed;
        for (const auto& one : options) {
            const void* predicted = nullptr;
            if (!placementquery::query(held, GameData::instance().player(), plan.target, one.face, one.click, predicted)) return Solved::Failed;
            if (predicted == want) { chosen = &one; chosenLook = looks[l]; break; }
        }
    }
    static unsigned said = 0;
    if (!chosen) {
        std::string reason;
        if (!solidAt(request.gameMode, ideal.neighbor))
            reason = std::format("needs a solid block on its {} side", directionName(flexible::opposite(ideal.face)));
        else reason = "no placement from here gives that facing";
        std::string block = item;
        if (block.rfind("minecraft:", 0) == 0) block.erase(0, 10);
        notify(std::format("{}:{}", block, reason), std::format("[Tsukuyomi] FlexiblePlacement: {}: {}", block, reason));
        if (said++ < 48) log().info(L"FlexiblePlacement: {} wants {} - not reproducible ({})",
            toUtf16(item), statesText(want), toUtf16(reason));
        return Solved::Impossible;
    }
    std::copy_n(chosen->pos, 3, request.pos); request.face = chosen->face; std::copy_n(chosen->click, 3, request.click);
    request.look = chosenLook;
    if (said++ < 48) log().info(L"FlexiblePlacement: {} wants {} - face {} at {}, {}, {} with yaw {}",
        toUtf16(item), statesText(want), chosen->face, chosen->pos[0], chosen->pos[1], chosen->pos[2], chosenLook.yaw);
    return Solved::Found;
}
void FlexiblePlacement::notify(const std::string& key, const std::string& message)
{
    const auto now = GetTickCount64();
    std::lock_guard lock(m_noticeMutex);
    auto& last = m_noticeAt[key];
    if (last != 0 && now - last < 2000) return;
    last = now;
    m_notices.push_back(message);
}
void FlexiblePlacement::clearLines()
{
    if (m_linesVisible.exchange(false)) {
        worldmesh::setDebugLines(worldmesh::LineSource::FlexiblePlacement, {});
        worldmesh::setDebugFills(worldmesh::LineSource::FlexiblePlacement, {});
    }
}
void FlexiblePlacement::onPlayerViewUpdate()
{
    std::vector<std::string> notices;
    { std::lock_guard lock(m_noticeMutex); notices.swap(m_notices); }
    for (const auto& message : notices) clientchat::printLocal(message);
    if (enabled()) placementquery::initialize();
    const auto keys = pressedKeys();
    std::int32_t pos[3]{};
    int face = -1;
    float rel[3]{};
    flexible::Look look;
    if (!enabled() || !m_gridOverlay.load() || !input::isInGameplay() || (!keys.rotation && !keys.offset && !keys.adjacent)
        || !readHit(pos, face, rel) || !lookplace::readLook(look)) { clearLines(); return; }
    const int facing = flexible::horizontalFromYaw(look.yaw);
    const auto part = flexible::hitPart(face, facing, rel);
    std::vector<worldmesh::DebugLine> lines;
    auto line = [&](float ah, float av, float bh, float bv) {
        worldmesh::DebugLine one{};
        gridPoint(pos, face, facing, ah, av, one.a); gridPoint(pos, face, facing, bh, bv, one.b);
        const float white[]{1, 1, 1, 0.8F};
        std::copy_n(white, 4, one.rgba); one.width = debuglines::kThinWidth;
        lines.push_back(one);
    };
    const float outer[4][2]{{0,0},{1,0},{1,1},{0,1}};
    const float inner[4][2]{{0.25F,0.25F},{0.75F,0.25F},{0.75F,0.75F},{0.25F,0.75F}};
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        line(outer[i][0], outer[i][1], outer[j][0], outer[j][1]);
        line(inner[i][0], inner[i][1], inner[j][0], inner[j][1]);
        line(outer[i][0], outer[i][1], inner[i][0], inner[i][1]);
    }
    const float* region[4]{inner[0], inner[1], inner[2], inner[3]};
    if (part != flexible::HitPart::Center) {
        int edge = 0;
        switch (part) {
        case flexible::HitPart::Right: edge = 1; break;
        case flexible::HitPart::Top: edge = 2; break;
        case flexible::HitPart::Left: edge = 3; break;
        default: break;
        }
        const int next = (edge + 1) % 4;
        region[0] = outer[edge]; region[1] = outer[next]; region[2] = inner[next]; region[3] = inner[edge];
    }
    worldmesh::DebugFill fill{};
    for (int i = 0; i < 4; ++i) gridPoint(pos, face, facing, region[i][0], region[i][1], fill.p[i]);
    const float blue[]{0x30 / 255.0F, 0x30 / 255.0F, 0xF0 / 255.0F, 0xC0 / 255.0F};
    std::copy_n(blue, 4, fill.rgba);
    worldmesh::setDebugFills(worldmesh::LineSource::FlexiblePlacement, {fill});
    worldmesh::setDebugLines(worldmesh::LineSource::FlexiblePlacement, std::move(lines));
    m_linesVisible.store(true);
}
void FlexiblePlacement::onEnabledChanged(bool enabled)
{
    if (!enabled) { lookplace::cancel(); clearLines(); }
    else {
        static bool told = false;
        if (!told) { told = true; log().info(L"FlexiblePlacement: up/down facing of pistons, dispensers, observers and barrels follows the real player position (the server ignores a spoofed one); spoofed yaw may be visible for one tick"); }
    }
}
void FlexiblePlacement::shutdown() { lookplace::cancel(); clearLines(); }
}
