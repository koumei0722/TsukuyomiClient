#include "modules/Waypoints.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Notice.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "game/BlockWrite.h"
#include "game/DebugScreenWorld.h"
#include "game/GameData.h"
#include "game/UiProbe.h"
#include "game/WorldIdentity.h"
#include "game/RejoinLogic.h"
#include "hooks/Detours.h"
#include "input/Foreground.h"
#include "render/WorldMesh.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <format>
#include <fstream>

namespace tsukuyomi {
namespace {
bool playerBlockPos(int& outX, int& outY, int& outZ, float* position = nullptr)
{
    float fx = 0, fy = 0, fz = 0;
    const bool feetOk = GameData::instance().playerFeet(fx, fy, fz);
    float x = 0, y = 0, z = 0;
    bool ok = false;
    if (GameData::instance().hasPlayerView()) {
        const auto view = GameData::instance().playerView();
        x = view.x; y = view.y - GameData::kEyeHeight; z = view.z;
        ok = std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }
    if (feetOk && (!ok || (std::fabs(fx - x) <= 6.0F && std::fabs(fy - y) <= 6.0F && std::fabs(fz - z) <= 6.0F))) {
        x = fx; y = fy; z = fz; ok = true;
    }
    if (!ok || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)
        || std::fabs(x) > 30000000 || std::fabs(y) > 30000000 || std::fabs(z) > 30000000) return false;
    outX = static_cast<int>(std::floor(x)); outY = static_cast<int>(std::floor(y)); outZ = static_cast<int>(std::floor(z));
    if (position) { position[0] = x; position[1] = y; position[2] = z; }
    return true;
}
bool blank(std::wstring_view text)
{
    return std::all_of(text.begin(), text.end(), [](wchar_t c) { return std::iswspace(c) != 0; });
}
}

Waypoints& Waypoints::instance() { static Waypoints module; return module; }
bool Waypoints::available() const { return worldmesh::worldTagsAvailable(); }

void Waypoints::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    std::lock_guard lock(m_mutex);
    std::vector<int> combo;
    if (const auto at = section.find("addKeys"); at != section.end() && at->is_array()) {
        for (const auto& key : *at) if (key.is_number_integer()) combo.push_back(key.get<int>());
    }
    m_addKey.set(std::move(combo));
    m_showDistance = Config::getBool(section, "showDistance", true);
    const float size = Config::getFloat(section, "labelSize", kDefaultLabelSize);
    m_labelSize = std::isfinite(size) ? std::clamp(size, 0.25F, 4.0F) : kDefaultLabelSize;
    ++m_revision;
}
void Waypoints::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    std::lock_guard lock(m_mutex);
    section["addKeys"] = m_addKey.combo(); section["showDistance"] = m_showDistance; section["labelSize"] = m_labelSize;
}
void Waypoints::onScansReady()
{
    if (!available()) { log().warn(L"Waypoints: unavailable (world tags not ready)"); return; }
    log().info(L"Waypoints: ready");
}
MenuItem Waypoints::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem()); children.push_back(toggleKeyItem());
    auto key = menu::keybind(L"Add key", [this] { return m_addKey.combo(); },
        [this](std::vector<int> combo) { m_addKey.set(std::move(combo)); });
    bindPad(key, m_addKey); children.push_back(std::move(key));
    children.push_back(menu::toggle(L"Show distance", [this] { std::lock_guard lock(m_mutex); return m_showDistance; },
        [this] { std::lock_guard lock(m_mutex); m_showDistance = !m_showDistance; ++m_revision; }));
    children.push_back(menu::number(L"Label size", [this] { std::lock_guard lock(m_mutex); return m_labelSize; },
        [this](float size) { if (!std::isfinite(size)) return; std::lock_guard lock(m_mutex); m_labelSize = std::clamp(size, 0.25F, 4.0F); ++m_revision; }, false, 0.25F, 4.0F));
    children.push_back(menu::text(L"Name", [this] { std::lock_guard lock(m_mutex); return toUtf16(m_draft.name); },
        [this](std::wstring text) { if (blank(text)) return; std::lock_guard lock(m_mutex); m_draft.name = toUtf8(text); }, TextInputKind::FreeText));
    children.push_back(menu::text(L"Color", [this] { std::lock_guard lock(m_mutex); return toUtf16(waypoints::formatColor(m_draft.rgb).substr(1)); },
        [this](std::wstring text) { std::uint32_t rgb = 0; if (!waypoints::parseColor(toUtf8(text), rgb)) return; std::lock_guard lock(m_mutex); m_draft.rgb = rgb; }, TextInputKind::FreeText));
    constexpr const wchar_t* labels[]{L"X", L"Y", L"Z"};
    for (int axis = 0; axis < 3; ++axis) {
        children.push_back(menu::text(labels[axis], [this, axis] {
            std::lock_guard lock(m_mutex); return std::to_wstring(axis == 0 ? m_draft.x : axis == 1 ? m_draft.y : m_draft.z);
        }, [this, axis](std::wstring text) {
            int coordinate = 0; if (!waypoints::parseCoordinate(toUtf8(text), coordinate)) return;
            std::lock_guard lock(m_mutex); (axis == 0 ? m_draft.x : axis == 1 ? m_draft.y : m_draft.z) = coordinate;
        }));
    }
    for (std::size_t i = 5; i < children.size(); ++i) { children[i].onPage = true; children[i].pageTab = kPageTab; }
    auto item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); }; item.isOn = [this] { return enabled(); };
    return item;
}
void Waypoints::onEnabledChanged(bool value)
{
    std::lock_guard lock(m_mutex);
    ++m_revision;
    if (!value) { m_openRequested.store(false); worldmesh::setWorldTags({}); }
}
void Waypoints::changedLocked() { m_dirtyAt = GetTickCount64(); ++m_revision; }

bool Waypoints::writeWorldLocked(const std::string& key, const std::vector<waypoints::Waypoint>& list, bool blocked)
{
    if (blocked) return false;
    const auto dir = paths::dataDir() / "waypoints";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / (key + ".json"), temp = dir / (key + ".json.tmp");
    bool ok = false;
    if (!ec) {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (out) { out << waypoints::toJson(list).dump(2); out.flush(); out.close(); ok = out.good() && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE; }
    }
    if (!ok) notice::failOnce("Waypoints.write", L"Waypoints: could not save waypoints; the original file was kept.", "Could not save waypoints; the original file was preserved.");
    return ok;
}
bool Waypoints::saveWorldLocked()
{
    m_saveAttemptAt = GetTickCount64();
    if (!m_worldKey.empty() && m_dirtyAt && !m_worldWriteBlocked) {
        m_unsaved[m_worldKey] = {m_list, false};
    }
    for (auto it = m_unsaved.begin(); it != m_unsaved.end();) {
        if (writeWorldLocked(it->first, it->second.list, it->second.blocked)) {
            if (it->first == m_worldKey) m_dirtyAt = 0;
            it = m_unsaved.erase(it);
        } else ++it;
    }
    return m_unsaved.empty() && (!m_dirtyAt || !m_worldKey.empty());
}
void Waypoints::leaveWorldLocked()
{
    if (!m_inWorld) return;
    saveWorldLocked();
    m_list.clear(); m_worldKey.clear(); m_dimension.clear(); m_worldIndex.reset();
    m_inWorld = false; m_worldWriteBlocked = false; m_dirtyAt = 0; m_identityResolveAt = 0;
    m_openRequested.store(false); ++m_revision;
    worldmesh::setWorldTags({});
}
void Waypoints::syncWorld()
{
    const auto identity = worldidentity::snapshot();
    const auto now = GetTickCount64();
    bool integrated = GameData::instance().integratedServer();
    if (m_inWorld && identity.entry == m_worldEntry && !m_worldKey.empty()) integrated = m_integrated;
    const bool sameEntry = m_inWorld && identity.entry == m_worldEntry;
    if (!sameEntry) { m_identityResolveAt = now + 2000; m_worldIndex.reset(); }
    if (integrated && !identity.received) m_worldIndex.update(now, identity.entry);
    auto key = sameEntry && !m_worldKey.empty() ? m_worldKey
        : worldidentity::worldKey(identity.received, integrated, identity.levelId, identity.name,
            toUtf8(m_worldIndex.folderName()), integrated ? std::string{} : rejoin::readConnectionUrl(hooks::gameClientInstance()));
    if (!integrated && now < m_identityResolveAt) key.clear();
    if (key.empty() && !integrated && now >= m_identityResolveAt)
        notice::failOnce("Waypoints.unsavedWorld", L"Waypoints: waypoints on this server are not saved (rejoin after loading Tsukuyomi to choose a save location).",
            "Waypoints here are not saved. Rejoin after loading Tsukuyomi to choose a save location.");
    if (sameEntry && key == m_worldKey) return;
    saveWorldLocked();
    auto pending = sameEntry && m_worldKey.empty() ? std::move(m_list) : std::vector<waypoints::Waypoint>{};
    m_list.clear(); m_dirtyAt = 0; m_saveAttemptAt = 0; m_worldWriteBlocked = false;
    m_inWorld = true; m_worldEntry = identity.entry; m_worldKey = key; m_integrated = integrated;
    if (!sameEntry) { m_dimension.clear(); m_lastAddedName.clear(); }
    if (const auto unsaved = m_unsaved.find(key); unsaved != m_unsaved.end()) {
        m_list = unsaved->second.list;
        m_worldWriteBlocked = unsaved->second.blocked;
        m_dirtyAt = now;
    } else if (!key.empty()) {
        const auto path = paths::dataDir() / "waypoints" / (key + ".json");
        std::error_code ec;
        if (std::filesystem::exists(path, ec) || ec) {
            std::ifstream in(path, std::ios::binary);
            const auto json = nlohmann::json::parse(in, nullptr, false);
            if (json.is_discarded() || !waypoints::fromJson(json, m_list)) {
                m_list.clear(); in.close();
                auto bad = path;
                const auto stamp = std::chrono::system_clock::now().time_since_epoch();
                bad += L".bad-" + std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(stamp).count());
                std::filesystem::rename(path, bad, ec); m_worldWriteBlocked = static_cast<bool>(ec);
                notice::failOnce("Waypoints.read", L"Waypoints: cannot read the JSON; the original file was moved aside and the list starts empty.", "Could not read waypoints; the original file was preserved.");
            }
        }
    }
    if (!pending.empty()) { m_list.insert(m_list.end(), pending.begin(), pending.end()); m_dirtyAt = now; }
    ++m_revision;
}

void Waypoints::onUpdate()
{
    if (m_addKey.triggered() && input::isInGameplay() && enabled()) m_openRequested.store(true);
    std::lock_guard lock(m_mutex);
    if (m_stopping) return;
    if (GameData::instance().msSinceView() > 2000 && blockwrite::renderRegion() == nullptr) leaveWorldLocked();
    const auto now = GetTickCount64();
    if (((m_dirtyAt && now - m_dirtyAt >= 1000) || !m_unsaved.empty()) && now - m_saveAttemptAt >= 1000) saveWorldLocked();
}
void Waypoints::onPlayerViewUpdate()
{
    bool open = false;
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping) return;
        syncWorld();
        int x = 0, y = 0, z = 0;
        const bool hasPosition = playerBlockPos(x, y, z, m_playerPos);
        int minY = 0, maxY = 0;
        if (hasPosition && debugworld::worldHeightAt(x, z, minY, maxY)) {
            const auto dimension = waypoints::dimensionName(minY, maxY);
            if (m_dimension != dimension) { m_dimension = dimension; ++m_revision; }
        }
        if (m_openRequested.exchange(false) && enabled() && hasPosition) {
            m_draft.x = x; m_draft.y = y; m_draft.z = z;
            if (m_draft.name.empty() || m_draft.name == m_lastAddedName) m_draft.name = waypoints::defaultName(m_list);
            open = true;
        }
        publishTagsLocked();
    }
    if (open && !uiprobe::openWaypointPage()) {
        static bool told = false;
        if (!told) { told = true; log().warn(L"Waypoints: the page could not be opened (not ready or throttled)"); }
    }
}
std::vector<std::size_t> Waypoints::dimensionRowsLocked() const
{
    std::vector<std::size_t> rows;
    for (std::size_t i = 0; i < m_list.size(); ++i) if (m_list[i].dimension == m_dimension) rows.push_back(i);
    return rows;
}
void Waypoints::publishTagsLocked()
{
    if (m_publishedRevision == m_revision) return;
    std::vector<worldmesh::WorldTag> tags;
    if (enabled() && m_inWorld && !m_dimension.empty()) for (auto i : dimensionRowsLocked()) {
        const auto& w = m_list[i]; if (!w.visible) continue;
        worldmesh::WorldTag label;
        label.pos[0] = w.x + 0.5; label.pos[1] = w.y + 0.5; label.pos[2] = w.z + 0.5;
        label.text = w.name; std::fill(std::begin(label.textRgba), std::end(label.textRgba), 1.0F);
        label.backRgba[3] = 0.25F; label.size = m_labelSize; label.lift = 6.0F; label.appendDistance = m_showDistance;
        tags.push_back(label);
        label.text.clear(); std::fill(std::begin(label.textRgba), std::end(label.textRgba), 0.0F);
        label.backRgba[0] = static_cast<float>((w.rgb >> 16) & 255) / 255.0F;
        label.backRgba[1] = static_cast<float>((w.rgb >> 8) & 255) / 255.0F;
        label.backRgba[2] = static_cast<float>(w.rgb & 255) / 255.0F;
        label.backRgba[3] = 1.0F; label.minHalfWidth = 4; label.lift = 0; label.appendDistance = false;
        tags.push_back(std::move(label));
    }
    worldmesh::setWorldTags(std::move(tags)); m_publishedRevision = m_revision;
}
void Waypoints::pageAction(int op)
{
    std::lock_guard lock(m_mutex);
    if (m_stopping || !m_inWorld) return;
    if (op == 0 && !m_dimension.empty()) {
        auto w = m_draft;
        if (w.name.empty()) w.name = waypoints::defaultName(m_list);
        w.dimension = m_dimension; w.visible = true; m_lastAddedName = w.name; m_list.push_back(std::move(w));
        m_draft.name = waypoints::defaultName(m_list); changedLocked();
    } else if (op == 1) {
        playerBlockPos(m_draft.x, m_draft.y, m_draft.z);
    } else if (op >= 100 && op < 116) {
        std::size_t count = 0; const auto* colors = waypoints::palette(count); m_draft.rgb = colors[op - 100].rgb;
    } else if (op >= 1000) {
        const bool remove = op >= 2000;
        const auto n = static_cast<std::size_t>(op - (remove ? 2000 : 1000));
        const auto rows = dimensionRowsLocked();
        if (n < rows.size()) {
            if (remove) m_list.erase(m_list.begin() + static_cast<std::ptrdiff_t>(rows[n]));
            else m_list[rows[n]].visible = !m_list[rows[n]].visible;
            changedLocked();
        }
    }
    publishTagsLocked();
}
std::vector<Waypoints::PageRow> Waypoints::pageRows()
{
    std::lock_guard lock(m_mutex);
    std::vector<PageRow> rows;
    for (auto i : dimensionRowsLocked()) {
        const auto& w = m_list[i];
        const double dx = w.x + 0.5 - m_playerPos[0], dy = w.y + 0.5 - m_playerPos[1], dz = w.z + 0.5 - m_playerPos[2];
        rows.push_back({std::format("{}  ({}, {}, {})  {:.0f}m", w.name, w.x, w.y, w.z, std::round(std::sqrt(dx * dx + dy * dy + dz * dz))), w.visible});
    }
    return rows;
}
std::uint32_t Waypoints::draftColor() { std::lock_guard lock(m_mutex); return m_draft.rgb; }
std::string Waypoints::currentDimension() { std::lock_guard lock(m_mutex); return m_dimension; }
bool Waypoints::hasWorld() { std::lock_guard lock(m_mutex); return !m_worldKey.empty(); }
void Waypoints::shutdown()
{
    std::lock_guard lock(m_mutex);
    m_stopping = true; saveWorldLocked(); worldmesh::setWorldTags({});
}
}
