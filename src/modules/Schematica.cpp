#include "modules/Schematica.h"
#include "modules/MaterialCollector.h"

#include "config/WriteSwitches.h"
#include "core/Notice.h"

#include "render/BoxRenderer.h"

#include <algorithm>
#include <map>
#include <limits>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <format>
#include <string>
#include <utility>

#include <Windows.h>

#include <commdlg.h>
#include <objbase.h>

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Notice.h"
#include "modules/HandRestock.h"
#include "modules/AutoTool.h"
#include "modules/FastUseItem.h"
#include "modules/FreeCamera.h"
#include "modules/ToggleSneakSprint.h"
#include "input/GameInput.h"
#include <optional>
#include "core/Paths.h"
#include "core/Strings.h"
#include "game/BlockRegistry.h"
#include "game/ContainerUi.h"
#include "game/BlockWrite.h"
#include "game/BuildOver.h"
#include "game/StructureWrite.h"
#include "hooks/Detours.h"
#include "game/SchematicWorld.h"
#include "game/EasyPlaceLogic.h"
#include "game/LookPlacement.h"
#include "input/GameButtons.h"
#include "game/PlacementQuery.h"
#include "memory/Memory.h"
#include "game/GameOptions.h"
#include "game/GameData.h"
#include "game/GameCommand.h"
#include "game/GameModeState.h"
#include "game/GameModeIds.h"
#include "game/ChatCommand.h"
#include "game/Rotation.h"
#include "game/WorldIdentity.h"
#include "game/RejoinLogic.h"
#include "game/DebugLines.h"
#include "game/ClientChat.h"
#include "render/WorldMesh.h"
#include <tuple>
#include <set>
#include <chrono>
#include "input/Keys.h"
#include "game/StackCount.h"
#include "game/UiProbe.h"
#include "game/SettingsCommand.h"
#include "input/Foreground.h"

namespace tsukuyomi {

namespace {
bool playerBlockPos(int& outX, int& outY, int& outZ);
}

Schematica& Schematica::instance()
{
    static Schematica module;
    return module;
}

void Schematica::scanFiles()
{
    const auto dir = paths::schematicsDir();
    std::map<std::wstring,std::wstring> names;
    std::error_code ec;
    if (!dir.empty()) {
        for (std::filesystem::directory_iterator it(dir,ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc) || fileEc) continue;
            auto ext = it->path().extension().wstring();
            std::transform(ext.begin(),ext.end(),ext.begin(),[](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
            if (ext == L".mcstructure") names.insert_or_assign(it->path().stem().wstring(),it->path().filename().wstring());
            else if (ext == L".litematic") names.emplace(it->path().stem().wstring(),it->path().filename().wstring());
        }
    }
    std::lock_guard lock(m_filesMutex);
    std::vector<std::unique_ptr<SchematicFile>> next;
    m_fileStems.clear(); m_fileNames.clear();
    for (const auto& [name,fileName] : names) {
        auto old = std::find_if(m_files.begin(),m_files.end(),[&](const auto& f) { return f && f->name == name; });
        auto file = old != m_files.end() ? std::move(*old) : std::make_unique<SchematicFile>();
        if (file->generation == 0) file->generation = ++m_nextFileGeneration;
        file->name = name; file->fileName = fileName;
        m_fileStems.push_back(name); m_fileNames.push_back(fileName);
        next.push_back(std::move(file));
    }
    m_files = std::move(next);
    m_outlinesDirty.store(true);
}

void Schematica::refreshFiles() { scanFiles(); }
std::size_t Schematica::fileCount() const { std::lock_guard lock(m_filesMutex); return m_files.size(); }
std::wstring Schematica::fileName(std::size_t at) const
{
    std::lock_guard lock(m_filesMutex);
    return at < m_files.size() ? m_files[at]->name : std::wstring{};
}
std::size_t Schematica::placementCount() const { std::lock_guard lock(m_filesMutex); return m_placements.size(); }
std::uint64_t Schematica::placementId(std::size_t at) const
{
    std::lock_guard lock(m_filesMutex);
    return at < m_placements.size() ? m_placements[at]->spec.id : 0;
}
bool Schematica::placementSpec(std::uint64_t id, schematic::PlacementSpec& out) const
{
    std::lock_guard lock(m_filesMutex);
    for (const auto& p : m_placements) if (p->spec.id == id) { out = p->spec; return true; }
    return false;
}
void Schematica::placementChanged(unsigned purpose)
{
    ++m_placementsRevision;
    m_placementsDirtyAt = GetTickCount64();
    m_outlinesDirty.store(true);
    if (purpose & kPrepReload) m_reloadPending.store(true);
    if (purpose & kPrepRedraw) m_posChangedAt.store(GetTickCount64());
}
bool Schematica::rejectLocked(const schematic::PlacementSpec& spec)
{
    if (!spec.locked) return false;
    const auto now = GetTickCount64();
    auto last = m_lockedNoticeAt.load();
    if ((last == 0 || now-last >= 1000) && m_lockedNoticeAt.compare_exchange_strong(last,now)) {
        log().warn(L"Schematica: placement {} is locked",spec.name);
        m_lockedNoticePending.store(true);
    }
    return true;
}
bool Schematica::mutatePlacement(std::uint64_t id, bool geometry, unsigned purpose,
    const std::function<void(schematic::PlacementSpec&)>& change)
{
    std::lock_guard lock(m_filesMutex);
    if (m_shuttingDown.load()) return false;
    for (const auto& p : m_placements) if (p->spec.id == id) {
        if (geometry && rejectLocked(p->spec)) return false;
        const auto before = p->spec;
        change(p->spec);
        if (before != p->spec) placementChanged(purpose);
        return true;
    }
    return false;
}
bool Schematica::setPlacementName(std::uint64_t id, std::wstring name)
{ return mutatePlacement(id,false,0,[&](auto& p) { p.name=std::move(name); }); }
bool Schematica::setPlacementEnabled(std::uint64_t id, bool value)
{ return mutatePlacement(id,false,kPrepReload,[&](auto& p) { p.enabled=value; }); }
bool Schematica::setPlacementRender(std::uint64_t id, bool value)
{ return mutatePlacement(id,false,kPrepReload,[&](auto& p) { p.render=value; }); }
bool Schematica::setPlacementLocked(std::uint64_t id, bool value)
{ return mutatePlacement(id,false,0,[&](auto& p) { p.locked=value; }); }
bool Schematica::setPlacementPos(std::uint64_t id, int axis, int value)
{
    if (axis<0 || axis>2) return false;
    return mutatePlacement(id,true,kPrepRedraw,[&](auto& p) {
        if (axis==0) p.x=std::clamp(value,kMinXZ,kMaxXZ);
        if (axis==1) p.y=std::clamp(value,kMinY,kMaxY);
        if (axis==2) p.z=std::clamp(value,kMinXZ,kMaxXZ);
    });
}
bool Schematica::setPlacementRotation(std::uint64_t id, int quarters)
{ return mutatePlacement(id,true,kPrepReload,[&](auto& p) { p.rotation=rotation::normalize(quarters); }); }
bool Schematica::setPlacementMirror(std::uint64_t id, schematic::Mirror mirror)
{
    if (mirror<schematic::Mirror::None || mirror>schematic::Mirror::Z) return false;
    return mutatePlacement(id,true,kPrepReload,[&](auto& p) { p.mirror=mirror; });
}
bool Schematica::cyclePlacementColor(std::uint64_t id)
{ return mutatePlacement(id,false,0,[](auto& p) { p.color=(p.color+1)%8; }); }
bool Schematica::setPlacementGrid(std::uint64_t id, const schematic::GridSpec& grid)
{
    for (int a=0;a<3;++a) if (grid.size[a]<0 || grid.repeatNeg[a]<0 || grid.repeatPos[a]<0) return false;
    return mutatePlacement(id,true,kPrepRedraw,[&](auto& p) { p.grid=grid; });
}
int Schematica::unusedColor() const
{
    bool used[8]{};
    for (const auto& p : m_placements) used[p->spec.color]=true;
    for (int i=0;i<8;++i) if (!used[i]) return i;
    return 0;
}
bool Schematica::loadFileAsPlacement(std::size_t at)
{
    std::lock_guard lock(m_filesMutex);
    if (!m_inWorld.load() || at>=m_files.size()) return false;
    m_loadFileGeneration.store(m_files[at]->generation);
    return true;
}
bool Schematica::movePlacementToPlayer(std::uint64_t id)
{
    std::lock_guard lock(m_filesMutex);
    for (const auto& p : m_placements) if (p->spec.id==id) {
        if (rejectLocked(p->spec)) return false;
        m_moveToPlayerId.store(id); return true;
    }
    return false;
}
bool Schematica::duplicatePlacement(std::uint64_t id)
{
    std::lock_guard lock(m_filesMutex);
    for (const auto& p : m_placements) if (p->spec.id==id) {
        int width=0;
        for (const auto& f : m_files) if (f->name==p->spec.schematic) {
            const auto b=schematic::placementBox(p->spec,f->sizeX.load(),f->sizeY.load(),f->sizeZ.load());
            width=b.x1-b.x0; break;
        }
        if (width<=0 || static_cast<std::int64_t>(p->spec.x)+width>kMaxXZ || m_nextId==UINT64_MAX) return false;
        auto copy=std::make_unique<PlacementEntry>(); copy->spec=p->spec;
        copy->spec.id=m_nextId++; copy->spec.x+=width; copy->spec.color=unusedColor();
        m_selectedId.store(copy->spec.id); m_placements.push_back(std::move(copy));
        placementChanged(kPrepReload); return true;
    }
    return false;
}
bool Schematica::removePlacement(std::uint64_t id)
{
    std::lock_guard lock(m_filesMutex);
    const auto p=std::find_if(m_placements.begin(),m_placements.end(),[&](const auto& one) { return one->spec.id==id; });
    if (p==m_placements.end()) return false;
    m_placements.erase(p);
    if (m_selectedId.load()==id) m_selectedId.store(m_placements.empty()?0:m_placements.front()->spec.id);
    placementChanged(kPrepReload); return true;
}
bool Schematica::selectPlacement(std::uint64_t id)
{
    std::lock_guard lock(m_filesMutex);
    if (id!=0 && std::none_of(m_placements.begin(),m_placements.end(),[&](const auto& p) { return p->spec.id==id; })) return false;
    if (m_selectedId.exchange(id)!=id) placementChanged(0);
    return true;
}

int Schematica::clampLayerValue(int axis, int value)
{
    return (axis == 1) ? std::clamp(value, -kMaxLayerOffsetY, kMaxLayerOffsetY)
                       : std::clamp(value, kMinXZ, kMaxXZ);
}

bool Schematica::layerOrigin(int& x, int& y, int& z, std::wstring* name) const
{
    schematic::PlacementSpec p;
    const bool ok=placementSpec(selectedPlacement(),p);
    x=p.x; y=p.y; z=p.z;
    if (name) *name=p.name;
    return ok;
}

int Schematica::layerOriginOf(int axis) const
{
    int x = 0;
    int y = 0;
    int z = 0;
    layerOrigin(x, y, z, nullptr);
    return (axis == 0) ? x : ((axis == 2) ? z : y);
}

void Schematica::layerRangeOffsets(bool& on, int& axis, int& lo, int& hi) const
{
    axis = std::clamp(m_layerAxis.load(std::memory_order_relaxed), 0, 2);
    const int mode = m_layerMode.load(std::memory_order_relaxed);
    const int value = m_layerValue.load(std::memory_order_relaxed);
    constexpr int kFar = 1 << 26;
    on = true;
    switch (mode) {
    case kLayerSingle:
        lo = value;
        hi = value;
        break;
    case kLayerBelow:
        lo = -kFar;
        hi = value;
        break;
    case kLayerAbove:
        lo = value;
        hi = kFar;
        break;
    case kLayerRange:
        lo = m_layerMin.load(std::memory_order_relaxed);
        hi = m_layerMax.load(std::memory_order_relaxed);
        if (lo > hi) {
            std::swap(lo, hi);
        }
        break;
    default:
        on = false;
        lo = 0;
        hi = 0;
        break;
    }
}

void Schematica::layerRange(bool& on, int& axis, int& lo, int& hi) const
{
    layerRangeOffsets(on, axis, lo, hi);
    if (!on) {
        lo = 0;
        hi = 0;
        return;
    }
    const long long origin = layerOriginOf(axis);
    const long long world[2] = {static_cast<long long>(lo) + origin,
                                static_cast<long long>(hi) + origin};
    const long long low = (axis == 1) ? kMinY : kMinXZ;
    const long long high = (axis == 1) ? kMaxY : kMaxXZ;
    lo = static_cast<int>(std::clamp(world[0], low, high));
    hi = static_cast<int>(std::clamp(world[1], low, high));
}

namespace {
bool playerBlockPos(int& outX, int& outY, int& outZ)
{
    float fx = 0.0F;
    float fy = 0.0F;
    float fz = 0.0F;
    const bool feetOk = GameData::instance().playerFeet(fx, fy, fz);
    bool viewOk = false;
    float vx = 0.0F;
    float vy = 0.0F;
    float vz = 0.0F;
    if (GameData::instance().hasPlayerView()) {
        const PlayerView view = GameData::instance().playerView();
        vx = view.x;
        vy = view.y - GameData::kEyeHeight;
        vz = view.z;
        viewOk = std::isfinite(vx) && std::isfinite(vy) && std::isfinite(vz);
    }
    float x = vx;
    float y = vy;
    float z = vz;
    bool ok = viewOk;
    if (feetOk) {
        constexpr float kNear = 6.0F;
        const bool close = viewOk && std::fabs(fx - vx) <= kNear && std::fabs(fy - vy) <= kNear
                           && std::fabs(fz - vz) <= kNear;
        if (close || !viewOk) {
            x = fx;
            y = fy;
            z = fz;
            ok = true;
        }
    }
    outX = static_cast<int>(std::floor(x));
    outY = static_cast<int>(std::floor(y));
    outZ = static_cast<int>(std::floor(z));
    return ok;
}

}

static int layerOfPlayer(int axis, bool& ok)
{
    int x = 0;
    int y = 0;
    int z = 0;
    ok = playerBlockPos(x, y, z);
    return (axis == 0) ? x : ((axis == 2) ? z : y);
}

void Schematica::setLayerMode(int mode)
{
    const int want = std::clamp(mode, 0, kLayerModeCount - 1);
    if (m_layerMode.load(std::memory_order_relaxed) == want) {
        return;
    }
    if (want == kLayerRange && m_layerMin.load() == 0 && m_layerMax.load() == 0) {
        m_layerMin.store(m_layerValue.load());
        m_layerMax.store(m_layerValue.load());
    }
    m_layerMode.store(want, std::memory_order_relaxed);
}

void Schematica::setLayerAxis(int axis)
{
    const int want = std::clamp(axis, 0, 2);
    if (m_layerAxis.load(std::memory_order_relaxed) == want) {
        return;
    }
    m_layerAxis.store(want, std::memory_order_relaxed);
    m_layerValue.store(clampLayerValue(want, m_layerValue.load()));
    m_layerMin.store(clampLayerValue(want, m_layerMin.load()));
    m_layerMax.store(clampLayerValue(want, m_layerMax.load()));
}

void Schematica::stepLayer(int delta)
{
    if (delta == 0) {
        return;
    }
    const int axis = std::clamp(m_layerAxis.load(std::memory_order_relaxed), 0, 2);
    const int mode = m_layerMode.load(std::memory_order_relaxed);
    switch (mode) {
    case kLayerAll: {
        bool ok = false;
        const int here = layerOfPlayer(axis, ok) - layerOriginOf(axis);
        m_layerValue.store(clampLayerValue(axis, ok ? here : m_layerValue.load()),
                           std::memory_order_relaxed);
        m_layerMode.store(kLayerSingle, std::memory_order_relaxed);
        break;
    }
    case kLayerRange: {
        const int lo = m_layerMin.load(std::memory_order_relaxed);
        const int hi = m_layerMax.load(std::memory_order_relaxed);
        m_layerMin.store(clampLayerValue(axis, lo + delta), std::memory_order_relaxed);
        m_layerMax.store(clampLayerValue(axis, hi + delta), std::memory_order_relaxed);
        break;
    }
    default:
        m_layerValue.store(clampLayerValue(axis, m_layerValue.load() + delta),
                           std::memory_order_relaxed);
        break;
    }
}

void Schematica::setLayerHere()
{
    const int axis = std::clamp(m_layerAxis.load(std::memory_order_relaxed), 0, 2);
    bool ok = false;
    const int world = layerOfPlayer(axis, ok);
    if (!ok) {
        log().warn(L"Schematica: could not read the player position, so the layer is not moved");
        return;
    }
    const int here = world - layerOriginOf(axis);
    if (m_layerMode.load(std::memory_order_relaxed) == kLayerRange) {
        const int lo = m_layerMin.load(std::memory_order_relaxed);
        const int hi = m_layerMax.load(std::memory_order_relaxed);
        const int width = std::abs(hi - lo);
        m_layerMin.store(clampLayerValue(axis, here), std::memory_order_relaxed);
        m_layerMax.store(clampLayerValue(axis, here + width), std::memory_order_relaxed);
    } else {
        m_layerValue.store(clampLayerValue(axis, here), std::memory_order_relaxed);
        if (m_layerMode.load(std::memory_order_relaxed) == kLayerAll) {
            m_layerMode.store(kLayerSingle, std::memory_order_relaxed);
        }
    }
}

void Schematica::applyLayerFilter()
{
    const auto typedAt = m_layerTypedAt.load(std::memory_order_relaxed);
    if (typedAt != 0 && GetTickCount64() - typedAt < kLayerTypeSettleMs) return;
    bool on = false;
    int axis = 1, lo = 0, hi = 0;
    layerRange(on, axis, lo, hi);
    if (m_layerFilter.on != on || m_layerFilter.axis != axis || m_layerFilter.lo != lo || m_layerFilter.hi != hi) {
        m_layerFilter = {on, axis, lo, hi};
        ++m_layerGeneration;
    }
}

bool Schematica::deleteArmed() const
{
    const unsigned long long armed = m_deleteArmedAt.load(std::memory_order_relaxed);
    return armed != 0 && GetTickCount64() - armed < kDeleteArmMs;
}

bool Schematica::fileInfo(std::size_t at, int& sizeX, int& sizeY, int& sizeZ,
                               std::size_t& solid) const
{
    sizeX = 0;
    sizeY = 0;
    sizeZ = 0;
    solid = 0;
    std::lock_guard<std::mutex> guard(m_filesMutex);
    if (at >= m_files.size()) {
        return false;
    }
    const SchematicFile& one = *m_files[at];
    sizeX = one.sizeX.load(std::memory_order_relaxed);
    sizeY = one.sizeY.load(std::memory_order_relaxed);
    sizeZ = one.sizeZ.load(std::memory_order_relaxed);
    solid = one.solidCount.load(std::memory_order_relaxed);
    return sizeX > 0 && sizeY > 0 && sizeZ > 0;
}

std::vector<Schematica::MaterialRow> Schematica::placementMaterials(std::uint64_t id,
                                                                   std::size_t limit,
                                                                   std::size_t* kinds, bool unfiltered) const
{
    if (kinds != nullptr) *kinds = 0;
    std::lock_guard guard(m_filesMutex);
    if (m_slotItemsDone != m_slotItems.size()) return {};
    std::map<std::string, MaterialRow> byName;
    std::map<int, std::map<std::uint16_t, std::size_t>> missing;
    schematicworld::missingByOwner(missing);
    const bool all = materialScopeAll();
    const auto multiplier = static_cast<std::size_t>(materialMultiplier());
    std::set<std::uint64_t> included;
    const auto add = [&](std::size_t slot, std::size_t count, bool total) {
        if (slot >= m_slotItems.size()) return;
        for (const auto& item : m_slotItems[slot]) {
            auto& row = byName[item.name];
            row.name = toUtf16(item.name);
            row.noItem = row.noItem || item.noItem;
            if (total) row.count += count * item.count * multiplier;
            else row.missing += count * item.count * multiplier;
        }
    };
    for (const auto& placed : m_placements) {
        const auto& spec = placed->spec;
        if (all ? !spec.enabled : spec.id != id) continue;
        const auto base = m_materialBases.find(spec.id);
        if (base == m_materialBases.end()) continue;
        const auto file = std::find_if(m_files.begin(), m_files.end(), [&](const auto& f) { return f->name == spec.schematic; });
        if (file == m_files.end()) continue;
        included.insert(spec.id);
        std::size_t copies = 1;
        if (spec.grid.enabled) for (int axis = 0; axis < 3; ++axis)
            copies *= 1 + static_cast<std::size_t>(spec.grid.repeatNeg[axis]) + static_cast<std::size_t>(spec.grid.repeatPos[axis]);
        for (std::size_t entry = 0; entry < (*file)->paletteCounts.size(); ++entry)
            if ((*file)->paletteCounts[entry] != 0) add(base->second + entry, (*file)->paletteCounts[entry] * copies, true);
    }
    for (std::size_t owner = 0; owner < m_materialOwners.size(); ++owner) {
        const auto placement = m_materialOwners[owner].first;
        if (!included.contains(placement)) continue;
        const auto found = missing.find(static_cast<int>(owner));
        if (found == missing.end()) continue;
        for (const auto& [entry, count] : found->second) add(m_materialOwners[owner].second + entry, count, false);
    }
    std::vector<MaterialRow> out;
    for (auto& [name, row] : byName) {
        if (m_ignoredMaterials.contains(name)) continue;
        row.missingKnown = schematicworld::active();
        row.availableKnown = m_availableKnown;
        if (const auto at = m_availableByName.find(name); at != m_availableByName.end()) row.available = at->second;
        if (!unfiltered && materialListType() == kMaterialMissing && row.missingKnown && row.missing == 0) continue;
        if (!unfiltered && materialHideAvailable() && row.availableKnown && row.missingKnown && row.available >= row.missing) continue;
        if (const auto at = m_iconIds.find(name); at != m_iconIds.end()) { row.iconIdAux = at->second; row.hasIcon = true; }
        if (const auto at = m_stackSizes.find(name); at != m_stackSizes.end()) row.stackSize = at->second;
        out.push_back(std::move(row));
    }
    const int column = materialSort();
    const bool descending = materialSortDescending();
    std::sort(out.begin(), out.end(), [column, descending](const auto& a, const auto& b) {
        if (column == 0) return descending ? a.name > b.name : a.name < b.name;
        const auto left = column == 1 ? a.count : column == 2 ? a.missing : a.available;
        const auto right = column == 1 ? b.count : column == 2 ? b.missing : b.available;
        return left != right ? (descending ? left > right : left < right) : a.name < b.name;
    });
    if (kinds != nullptr) *kinds = out.size();
    if (out.size() > limit) out.resize(limit);
    return out;
}

bool Schematica::materialAutoCollect() const { return MaterialCollector::instance().enabled(); }
void Schematica::toggleMaterialAutoCollect() { MaterialCollector::instance().toggle(); }
void Schematica::queueChat(std::string message)
{
    std::lock_guard lock(m_placeNoticeMutex);
    m_placeMessages.push_back(std::move(message));
}

void Schematica::setMaterialMultiplier(int value)
{
    if (m_materialMultiplier.exchange(std::clamp(value, 1, 999)) != std::clamp(value, 1, 999)) ++m_materialRevision;
}
void Schematica::setMaterialSort(int column)
{
    column = std::clamp(column, 0, 3);
    if (m_materialSort.exchange(column) == column) m_materialSortDescending.store(!m_materialSortDescending.load());
    else m_materialSortDescending.store(column != 0);
}
std::pair<std::size_t, std::size_t> Schematica::materialItemProgress() const
{
    std::lock_guard guard(m_filesMutex);
    return {m_slotItemsDone, m_slotItems.size()};
}

void Schematica::refreshAvailableCounts()
{
    const auto now = GetTickCount64();
    const bool asked = m_materialRefreshWanted.exchange(false);
    if (!asked && now - m_availableAt < 1000) return;
    m_availableAt = now;
    std::map<std::string, std::size_t> counted;
    bool contentsKnown = true;
    const bool ok = HandRestock::instance().forEachInventoryStack([&](const void* stack, int count) {
        const auto name = containerui::itemName(stack);
        if (name.empty()) { contentsKnown = false; return; }
        counted[name] += static_cast<std::size_t>(count);
        if (name.find("shulker_box") == std::string::npos) return;
        const auto* tag = containerui::userDataOf(stack);
        if (tag == nullptr) return;
        std::vector<containerui::NbtItem> contents;
        if (!containerui::nbtItemsOfTag(tag, contents)) { contentsKnown = false; return; }
        for (const auto& item : contents) if (!item.name.empty() && item.count > 0)
            counted[item.name] += static_cast<std::size_t>(item.count) * static_cast<std::size_t>(count);
    });
    std::lock_guard guard(m_filesMutex);
    m_availableByName = std::move(counted);
    m_availableKnown = ok && contentsKnown;
    if (asked) ++m_materialRevision;
}

void Schematica::resolveMaterialItems()
{
    std::shared_ptr<const std::vector<const void*>> palette;
    { std::lock_guard guard(m_mutex); palette = m_paletteBlocks; }
    if (!palette) return;
    std::lock_guard guard(m_filesMutex);
    if (palette->size() != m_slotSources.size() || m_slotItemsDone == m_slotItems.size()) return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
    do {
        const auto slot = m_slotItemsDone;
        const auto* block = (*palette)[slot];
        const auto& entry = m_slotSources[slot];
        materialrules::States states, gameStates;
        for (const auto& state : entry.states) states[state.name] = state.normalized();
        if (block != nullptr) {
            std::vector<std::array<char, blocks::kStateTextBytes>> lines(16);
            int rows = 0;
            for (;;) {
                rows = blocks::statesOfBlock(block, reinterpret_cast<char (*)[blocks::kStateTextBytes]>(lines.data()), static_cast<int>(lines.size()));
                if (static_cast<std::size_t>(rows) < lines.size()) break;
                lines.resize(lines.size() * 2);
            }
            std::vector<std::string> text;
            for (int row = 0; row < rows; ++row) text.emplace_back(lines[static_cast<std::size_t>(row)].data());
            gameStates = materialrules::parseStates(text);
            for (const auto& [name, value] : gameStates) states[name] = value;
        }
        pickitem::Result picked;
        if (block != nullptr && !materialrules::isLiquid(entry.name)) {
            const auto [at, inserted] = m_pickedItems.try_emplace(block);
            if (inserted) at->second = pickitem::ofBlock(block);
            picked = at->second;
            if (!picked.item.name.empty()) {
                if (!m_pickWorked && m_pickEmptySince != 0)
                    log().info(L"Schematica: picked items became available {} ms after the first empty answer", GetTickCount64() - m_pickEmptySince);
                m_pickWorked = true;
            } else if (!m_pickWorked && !materialrules::itemsFor(entry.name, states, {}).empty()) {
                constexpr std::uint64_t kPickWarmupMs = 3000;
                const auto now = GetTickCount64();
                if (m_pickEmptySince == 0) m_pickEmptySince = now;
                if (now - m_pickEmptySince < kPickWarmupMs) { m_pickedItems.erase(at); return; }
                static bool said = false;
                if (!said) { said = true; log().warn(L"Schematica: the game still picks no item for {} after {} ms; treating it as having no item", toUtf16(entry.name), now - m_pickEmptySince); }
            }
        }
        m_slotItems[slot] = materialrules::itemsFor(entry.name, states, picked.item);
        if (block) {
            PlacementInfo info;
            info.block = entry.name;
            info.states = std::move(gameStates);
            if (!m_slotItems[slot].empty() && !m_slotItems[slot].front().noItem) info.item = m_slotItems[slot].front().name;
            m_placementInfo[block] = std::move(info);
        }
        for (const auto& item : m_slotItems[slot]) {
            int size = 0;
            if (!m_stackSizes.contains(item.name) && blocks::maxStackSizeOf(item.name.c_str(), size)) m_stackSizes[item.name] = size;
            if (picked.hasIcon && item.name == picked.item.name) m_iconIds[item.name] = picked.iconIdAux;
            else if (!m_iconIds.contains(item.name)) {
                std::int32_t icon = 0;
                if (block != nullptr && blocks::blockItemIdAux(block, icon)) {
                    m_iconIds[item.name] = icon;
                    static unsigned said = 0;
                    if (said++ < 16) log().warn(L"Schematica: material {} uses the block icon because the picked item icon is unavailable", toUtf16(item.name));
                }
            }
        }
        ++m_slotItemsDone;
    } while (m_slotItemsDone < m_slotItems.size() && std::chrono::steady_clock::now() < deadline);
    if (m_slotItemsDone == m_slotItems.size()) ++m_materialRevision;
}

void Schematica::cycleMaterialListType()
{
    const int next =
        (m_materialListType.load(std::memory_order_relaxed) + 1) % kMaterialListTypeCount;
    m_materialListType.store(next, std::memory_order_relaxed);
}

void Schematica::toggleMaterialIgnored(const std::wstring& name)
{
    const std::string key = toUtf8(name);
    if (key.empty()) {
        return;
    }
    std::lock_guard<std::mutex> guard(m_filesMutex);
    if (const auto found = m_ignoredMaterials.find(key); found != m_ignoredMaterials.end()) {
        m_ignoredMaterials.erase(found);
        log().info(L"Schematica: material {} is shown again", name);
        return;
    }
    m_ignoredMaterials.insert(key);
    log().info(L"Schematica: material {} is ignored", name);
}

void Schematica::clearIgnoredMaterials()
{
    std::lock_guard<std::mutex> guard(m_filesMutex);
    if (m_ignoredMaterials.empty()) {
        return;
    }
    log().info(L"Schematica: cleared {} ignored material(s)", m_ignoredMaterials.size());
    m_ignoredMaterials.clear();
}

std::size_t Schematica::ignoredMaterialCount() const
{
    std::lock_guard<std::mutex> guard(m_filesMutex);
    return m_ignoredMaterials.size();
}

bool Schematica::writeMaterialList(std::wstring& outPath)
{
    const auto [done, total] = materialItemProgress();
    if (done != total) return false;
    schematic::PlacementSpec spec;
    const auto id=selectedPlacement();
    if (!materialScopeAll() && !placementSpec(id,spec)) return false;
    const auto name=materialScopeAll() ? std::wstring(L"All placements") : spec.name;
    std::size_t kinds=0;
    const auto rows=placementMaterials(id,(std::numeric_limits<std::size_t>::max)(),&kinds);

    std::error_code ec;
    const std::filesystem::path dir = paths::schematicsDir().parent_path() / "material-lists";
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        log().warn(L"Schematica: could not make {} ({})", dir.wstring(), toUtf16(ec.message()));
        return false;
    }
    std::wstring safe = name;
    for (wchar_t& ch : safe) {
        if (std::wcschr(L"\\/:*?\"<>|", ch) != nullptr) {
            ch = L'_';
        }
    }
    const std::filesystem::path path = dir / (safe + L".txt");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        log().warn(L"Schematica: could not write {}", path.wstring());
        return false;
    }
    out << "Material list for " << toUtf8(name) << "\r\n";
    out << "Item\tTotal\tMissing\tAvailable\r\n";
    for (const MaterialRow& row : rows) {
        out << toUtf8(row.name) << (row.noItem ? " (no item)" : "") << '\t' << row.count << '\t';
        if (row.missingKnown) {
            out << row.missing;
        } else {
            out << '-';
        }
        out << '\t';
        if (row.availableKnown) {
            out << row.available;
        } else {
            out << '-';
        }
        out << "\r\n";
    }
    out.close();
    outPath = path.wstring();
    log().info(L"Schematica: wrote the material list to {}", outPath);
    return true;
}

void Schematica::setVerifierScopeAll(bool all)
{
    if (m_verifierAll.exchange(all) == all) return;
    clearVerifierSelection(); refreshVerifier();
}
void Schematica::refreshVerifier() { m_verifierRefresh.store(true); }
void Schematica::selectVerifierKind(schematic::DiffKind kind)
{
    if (schematic::colorOfDiffKind(kind) == schematic::DiffColor::None) return;
    std::lock_guard lock(m_verifierMutex);
    m_verifierSelection = {};
    m_verifierSelection.active = true; m_verifierSelection.kind = kind;
    m_verifierSelection.scopeOwner = m_verifierOwner;
}
bool Schematica::selectVerifierPair(std::size_t at, std::uint64_t serial)
{
    const auto report = verifierReport();
    if (!report || report->serial != serial || at >= report->pairs.size()) return false;
    const auto& pair = report->pairs[at];
    std::lock_guard lock(m_verifierMutex);
    if (report->scopeOwner != m_verifierOwner) return false;
    m_verifierSelection = {true, pair.kind, true, pair.wantEntry, pair.real, m_verifierOwner};
    return true;
}
bool Schematica::ignoreVerifierPair(std::size_t at, std::uint64_t serial)
{
    const auto report = verifierReport();
    if (!report || report->serial != serial || at >= report->pairs.size()) return false;
    const auto& pair = report->pairs[at];
    std::lock_guard lock(m_verifierMutex);
    const schematicworld::VerifierIgnored ignored{pair.kind, pair.wantName, pair.realName};
    if (std::find(m_verifierIgnored.begin(), m_verifierIgnored.end(), ignored) == m_verifierIgnored.end())
        m_verifierIgnored.push_back(ignored);
    m_verifierIgnoredPending = true;
    if (m_verifierSelection.pair && m_verifierSelection.kind == pair.kind
        && m_verifierSelection.wantEntry == pair.wantEntry && m_verifierSelection.real == pair.real) {
        m_verifierSelection.pair = false; m_verifierSelection.wantEntry = 0xFFFF; m_verifierSelection.real = nullptr;
    }
    refreshVerifier();
    return true;
}
void Schematica::clearVerifierSelection()
{
    std::lock_guard lock(m_verifierMutex); m_verifierSelection = {};
}
void Schematica::clearVerifierIgnored()
{
    std::lock_guard lock(m_verifierMutex);
    m_verifierIgnored.clear(); m_verifierIgnoredPending = true; refreshVerifier();
}
std::size_t Schematica::verifierIgnoredCount() const
{
    std::lock_guard lock(m_verifierMutex); return m_verifierIgnored.size();
}
schematicworld::VerifierSelection Schematica::verifierSelection() const
{
    std::lock_guard lock(m_verifierMutex); return m_verifierSelection;
}
std::shared_ptr<const schematicworld::VerifierReport> Schematica::verifierReport() const
{
    return schematicworld::verifierReport();
}
void Schematica::syncVerifier()
{
    int owner = -1;
    if (!m_verifierAll.load()) {
        owner = -2;
        std::lock_guard lock(m_mutex);
        const auto id = selectedPlacement();
        for (std::size_t i = 0; i < m_materialOwners.size(); ++i)
            if (id != 0 && m_materialOwners[i].first == id) { owner = static_cast<int>(i); break; }
    }
    const auto generation = schematicworld::indexGeneration();
    std::lock_guard lock(m_verifierMutex);
    if (m_verifierOwner != owner || m_verifierIndexGeneration != generation) {
        m_verifierOwner = owner; m_verifierIndexGeneration = generation;
        m_verifierSelection = {}; m_verifierRefresh.store(true);
    }
    if (m_verifierIgnoredPending) {
        schematicworld::setVerifierIgnored(m_verifierIgnored); m_verifierIgnoredPending = false;
    }
    auto selection = m_verifierSelection;
    selection.scopeOwner = owner;
    schematicworld::setVerifierSelection(selection);
    if (m_verifierRefresh.exchange(false)) schematicworld::requestVerifier(owner);
}

bool Schematica::deleteFile(std::size_t at)
{
    std::wstring fileName;
    {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        if (at >= m_files.size()) {
            return false;
        }
        fileName = m_files[at]->fileName;

    }
    std::error_code ec;
    const std::filesystem::path path = paths::schematicsDir() / fileName;
    const bool gone = std::filesystem::remove(path, ec);
    if (!gone || ec) {
        log().warn(L"Schematica: could not delete {} ({})",
                   fileName,
                   toUtf16(ec.message()));
        return false;
    }
    log().info(L"Schematica: deleted {} from schematics", fileName);
    scanFiles();
    m_reloadPending.store(true, std::memory_order_relaxed);
    return true;
}

std::vector<Schematica::PrepEntry> Schematica::snapshotVisible(bool& needLoad)
{
    needLoad=false;
    std::vector<PrepEntry> out;
    const auto dir=paths::schematicsDir();
    std::lock_guard lock(m_filesMutex);
    for (std::size_t owner=0;owner<m_placements.size();++owner) {
        const auto& p=m_placements[owner]->spec;
        if (!p.enabled) continue;
        const auto f=std::find_if(m_files.begin(),m_files.end(),[&](const auto& one) { return one->name==p.schematic; });
        if (f==m_files.end()) continue;
        const auto& file=**f;
        if (!p.render && file.ready) continue;
        PrepEntry e;
        e.placementId=p.id; e.owner=static_cast<int>(owner); e.spec=p;
        e.generation=file.generation; e.path=dir/file.fileName;
        e.x=p.x; e.y=p.y; e.z=p.z; e.rotation=p.rotation; e.mirror=static_cast<int>(p.mirror);
        e.loadOnly=!p.render; e.data=file.loaded;
        needLoad=needLoad || !e.data;
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<schematic::Placement> Schematica::placementsOf(const std::vector<PrepEntry>& entries, const schematic::Box& range)
{
    std::vector<schematic::Placement> out;
    for (const auto& e : entries) {
        if (!e.data || e.loadOnly) continue;
        schematic::Placement p;
        p.data=e.data; p.placementId=e.placementId; p.owner=e.owner;
        p.x=e.x; p.y=e.y; p.z=e.z; p.rotation=e.rotation; p.mirror=e.mirror;
        if (e.spec.grid.enabled) {
            p.copies=schematic::copiesInBox(e.spec.grid,schematic::placementBox(e.spec,e.data->sizeX,e.data->sizeY,e.data->sizeZ),
                {p.x,p.y,p.z},range);
            if (p.copies.empty()) continue;
        }
        out.push_back(std::move(p));
    }
    schematic::assignPaletteBases(out);
    return out;
}

namespace {

bool samePalette(const std::vector<schematic::Placement>& a, const std::vector<schematic::Placement>& b)
{
    using Key=std::tuple<std::size_t,const void*,int,int>;
    const auto keys=[](const auto& live) {
        std::set<Key> out;
        for(const auto& p:live)out.emplace(p.paletteBase,p.data.get(),p.rotation&3,p.mirror);
        return out;
    };
    return keys(a)==keys(b);
}

}

void Schematica::requestPrepare(unsigned purposes)
{
    m_prepWant |= purposes;
    if (!m_prepBusy) {
        startPrepare();
    }
    if (m_paletteStale) {
        resolvePalette(m_live);
        finishPrepared(0);
    }
}

void Schematica::pollPrepare()
{
    if (m_prepBusy) {
        std::unique_ptr<PrepareJob> done;
        {
            std::lock_guard<std::mutex> guard(m_workMutex);
            done = std::move(m_workDone);
        }
        if (done) {
            m_prepBusy = false;
            installPrepared(std::move(done));
        }
    }
    if (!m_prepBusy && m_prepWant != 0) {
        startPrepare();
    }
}

void Schematica::startPrepare()
{
    const unsigned purposes = m_prepWant;
    if (purposes == 0 || m_prepBusy) {
        return;
    }
    m_prepWant = 0;
    bool needLoad = false;
    std::vector<PrepEntry> entries = snapshotVisible(needLoad);
    m_prepHasVisible = !entries.empty();

    if (!needLoad) {
        std::vector<schematic::Placement> placements = placementsOf(entries,m_copyRange);
        const std::vector<schematic::LayoutKey> key = schematic::layoutOf(placements);
        const bool unchanged = key == m_cellSet->key
                               && (key.empty() ? m_cellSet->empty() : !m_cellSet->empty());
        if (unchanged) {
            const bool paletteSame = samePalette(placements, m_live);
            m_live = std::move(placements);
            if ((purposes & kPrepReload) != 0 || m_paletteStale || !paletteSame) {
                resolvePalette(m_live);
            }
            if ((purposes & (kPrepReload | kPrepRedraw)) != 0) {
                if (m_prepareLogCount++ < 32) log().info(L"Schematica: {} copy(ies) / {} cell(s) (unchanged, reused)",
                           m_live.size(),
                           m_cellSet->size());
            }
            finishPrepared(purposes);
            return;
        }
    }

    auto job = std::make_unique<PrepareJob>();
    job->purposes = purposes;
    { std::lock_guard lock(m_filesMutex); job->revision=m_placementsRevision; }
    job->range=m_copyRange;
    job->entries = std::move(entries);
    job->prevKey = m_cellSet->key;
    job->prevHasCells = !m_cellSet->empty();
    job->startedAt = GetTickCount64();
    std::size_t toRead = 0;
    for (const PrepEntry& one : job->entries) {
        if (!one.data) {
            ++toRead;
        }
    }
    if (!ensureWorker()) {
        return;
    }
    if (m_prepareLogCount++ < 32) log().info(L"Schematica: preparing {} copy(ies) in the background ({} file(s) to read)",
               job->entries.size(),
               toRead);
    m_prepBusy = true;
    postJob(std::move(job));
}

void Schematica::runPrepareJob(PrepareJob& job)
try {
    const unsigned long long began = GetTickCount64();
    std::map<std::filesystem::path,schematic::Loaded> loadedFiles;
    for (PrepEntry& one : job.entries) {
        if (one.data) {
            continue;
        }
        if (m_workCancel.load(std::memory_order_relaxed)) {
            job.cancelled = true;
            return;
        }
        const auto previous=loadedFiles.find(one.path);
        if (previous!=loadedFiles.end()) { one.data=previous->second.data; continue; }
        one.result = schematic::loadBlueprint(one.path);
        loadedFiles.emplace(one.path,one.result);
        one.loadedNow = true;
        if (one.result.ok()) {
            one.data = one.result.data;
        }
    }
    const unsigned long long loaded = GetTickCount64();
    job.loadMs = loaded - began;

    job.placements = placementsOf(job.entries,job.range);
    job.key = schematic::layoutOf(job.placements);
    const bool unchanged = job.key == job.prevKey
                           && (job.key.empty() ? !job.prevHasCells : job.prevHasCells);
    if (!unchanged) {
        auto cells = std::make_unique<schematic::CellSet>();
        if (!schematic::buildCells(job.placements, *cells, &m_workCancel)) {
            job.why = cells->why;
            job.cancelled = true;
            return;
        }
        job.cells = std::move(cells);
    }
    job.cellsMs = GetTickCount64() - loaded;
}

catch (const std::bad_alloc&) {
    job.why = "out of memory";
    job.cancelled = true;
}

void Schematica::installPrepared(std::unique_ptr<PrepareJob> job)
{
    if (job && job->why) log().warn(L"Schematica: could not prepare cells ({})", toUtf16(job->why));
    if (!job || job->cancelled || m_shuttingDown.load(std::memory_order_relaxed)) {
        return;
    }

    struct LoadNote {
        std::wstring name;
        const schematic::Loaded* result = nullptr;
        std::size_t paletteSize = 0;
    };
    std::vector<LoadNote> notes;
    {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        for (const PrepEntry& one : job->entries) {
            const auto found = std::find_if(m_files.begin(), m_files.end(),
                [&one](const auto& bp) { return bp->name == one.spec.schematic; });
            if (found == m_files.end() || (*found)->generation != one.generation) {
                m_prepWant |= job->purposes | kPrepReload;
                return;
            }
        }
        for (PrepEntry& one : job->entries) {
            if (!one.loadedNow) {
                continue;
            }
            notes.push_back(LoadNote{one.spec.schematic, &one.result,
                                     one.result.data ? one.result.data->palette.size() : 0});
            SchematicFile* found = nullptr;
            for (const auto& bp : m_files) {
                if (bp->name == one.spec.schematic) {
                    found = bp.get();
                    break;
                }
            }
            if (found == nullptr) {
                continue;
            }
            if (!one.result.ok()) {
                if (!found->ready) {
                    found->loaded.reset();
                }
                continue;
            }
            found->sizeX.store(one.result.sizeX, std::memory_order_relaxed);
            found->sizeY.store(one.result.sizeY, std::memory_order_relaxed);
            found->sizeZ.store(one.result.sizeZ, std::memory_order_relaxed);
            found->solidCount.store(one.result.solid, std::memory_order_relaxed);
            found->paletteCounts = one.result.paletteCounts;
            if (!found->ready) {
                found->loaded = one.data;
                found->ready = true;
            }
        }
    }
    m_outlinesDirty.store(true);
    { std::lock_guard lock(m_filesMutex);
      if (job->revision!=m_placementsRevision) { m_prepWant |= kPrepReload; return; } }
    for (const LoadNote& one : notes) {
        const schematic::Loaded& got = *one.result;
        if (!got.ok()) {
            if (got.requiredMemory != 0) {
                log().warn(L"Schematica: memory for {}: required {} MB, available {} MB", one.name,
                           got.requiredMemory / (1024 * 1024), got.availableMemory / (1024 * 1024));
            }
            if (got.sizeX > 0 && got.sizeY > 0 && got.sizeZ > 0) {
                const std::size_t volume = static_cast<std::size_t>(got.sizeX)
                                           * static_cast<std::size_t>(got.sizeY)
                                           * static_cast<std::size_t>(got.sizeZ);
                log().warn(L"Schematica: could not read {} ({}) - {}x{}x{} = {} cells", one.name,
                           toUtf16(got.why != nullptr ? got.why : "?"), got.sizeX, got.sizeY,
                           got.sizeZ, volume);
            } else {
                log().warn(L"Schematica: could not read {} ({})", one.name,
                           toUtf16(got.why != nullptr ? got.why : "?"));
            }
            continue;
        }
        log().info(L"Schematica: loaded {} ({}x{}x{}, {} palette entries, {} solid blocks)",
                   one.name, got.sizeX, got.sizeY, got.sizeZ, one.paletteSize, got.solid);
        if (!got.note.empty()) log().warn(L"Schematica: {}: {}", one.name, toUtf16(got.note));
    }

    bool needLoad = false;
    std::vector<PrepEntry> nowEntries = snapshotVisible(needLoad);
    std::vector<schematic::Placement> placements = placementsOf(nowEntries,m_copyRange);
    const std::vector<schematic::LayoutKey> key = schematic::layoutOf(placements);
    if (key != job->key) {
        if (m_prepareLogCount++ < 32) log().info(L"Schematica: the layout changed while it was being prepared, so it is "
                   L"prepared again");
        m_prepWant |= job->purposes;
        postTrash(std::move(job->cells));
        return;
    }

    const std::size_t overlapped = job->cells ? job->cells->overlapped : 0;
    const bool rebuilt = job->cells != nullptr;
    if (rebuilt) {
        installCells(std::move(job->cells));
    }
    const bool paletteSame = samePalette(placements, m_live);
    m_live = std::move(placements);
    if ((job->purposes & kPrepReload) != 0 || m_paletteStale || !paletteSame) {
        resolvePalette(m_live);
    }
    if (m_prepareLogCount++ < 32) log().info(L"Schematica: {} copy(ies) / {} cell(s){}{} - prepared in the background in "
               L"{} ms (read {} ms / cells {} ms)",
               m_live.size(),
               m_cellSet->size(),
               overlapped != 0 ? std::format(L" ({} dropped as overlapping)", overlapped)
                               : std::wstring{},
               rebuilt ? L"" : L" (unchanged, reused)",
               GetTickCount64() - job->startedAt,
               job->loadMs,
               job->cellsMs);
    finishPrepared(job->purposes);
}

void Schematica::installCells(std::unique_ptr<schematic::CellSet> cells)
{
    std::lock_guard guard(m_mutex);
    m_cellTrash = std::move(m_cellSet);
    m_cellSet = std::shared_ptr<const schematic::CellSet>(std::move(cells));
}

void Schematica::finishPrepared(unsigned purposes)
{
    (void)purposes;
    m_prepHasVisible = !m_cellSet->empty() && !m_palette.empty();
    m_ghostWanted.store(m_prepHasVisible, std::memory_order_relaxed);
    {
        std::scoped_lock guard(m_filesMutex,m_mutex);
        m_materialOwners.clear();
        m_materialOwners.resize(m_placements.size(),{0,0});
        for (const auto& placed : m_live) if (placed.owner>=0 && static_cast<std::size_t>(placed.owner)<m_materialOwners.size())
            m_materialOwners[static_cast<std::size_t>(placed.owner)]={placed.placementId,placed.paletteBase};
        if (enabled() && m_prepHasVisible) schematicworld::install(m_cellSet, m_paletteBlocks, m_air);
        else schematicworld::clear();
    }
    postTrash(std::move(m_cellTrash));
}

unsigned long __stdcall Schematica::workerThreadMain(void* param)
{
    static_cast<Schematica*>(param)->workerMain();
    return 0;
}

bool Schematica::ensureWorker()
{
    std::lock_guard<std::mutex> guard(m_workMutex);
    if (m_workThread != nullptr) {
        return true;
    }
    if (m_workStop) {
        return false;
    }
    const HANDLE thread =
        CreateThread(nullptr, 0, &Schematica::workerThreadMain, this, 0, nullptr);
    if (thread == nullptr) {
        notice::failOnce("Schematica.worker",
                         std::format(L"Schematica: could not start the background loader thread (error {}); "
                                     L"schematics are not loaded",
                                     GetLastError()),
                         "Schematica cannot load schematics: the background loader thread could not be started");
        return false;
    }
    m_workThread = thread;
    return true;
}

void Schematica::workerMain()
{
    for (;;) {
        std::unique_ptr<PrepareJob> job;
        std::unique_ptr<SelectionSaveJob> save;
        std::unique_ptr<PasteJob> pasteJob;
        std::unique_ptr<OutlineJob> outline;
        std::vector<std::unique_ptr<SelectionSaveJob>> saveTrash;
        std::vector<std::shared_ptr<const schematic::CellSet>> trash;
        {
            std::unique_lock<std::mutex> lock(m_workMutex);
            m_workCv.wait(lock, [this] {
                return m_workStop || m_workJob != nullptr || m_saveWork != nullptr || m_pasteWork != nullptr || m_outlineWork != nullptr || !m_workTrash.empty() || !m_saveTrash.empty();
            });
            trash.swap(m_workTrash);
            job = std::move(m_workJob);
            save = std::move(m_saveWork);
            pasteJob = std::move(m_pasteWork);
            outline = std::move(m_outlineWork);
            saveTrash.swap(m_saveTrash);
            if (m_workStop && job == nullptr && save == nullptr && pasteJob == nullptr && outline == nullptr && trash.empty() && saveTrash.empty()) {
                return;
            }
        }
        trash.clear();
        saveTrash.clear();
        if (pasteJob) {
            preparePaste(*pasteJob);
            std::lock_guard guard(m_workMutex);
            m_pasteDone = std::move(pasteJob);
        }
        if (save) {
            runSelectionSave(*save);
            std::lock_guard guard(m_workMutex);
            m_saveDone=std::move(save);
        }
        if (outline) {
            runOutlineJob(*outline);
            std::lock_guard guard(m_workMutex);
            m_outlineDone=std::move(outline);
        }
        if (job != nullptr) {
            runPrepareJob(*job);
            std::lock_guard<std::mutex> guard(m_workMutex);
            m_workDone = std::move(job);
        }
    }
}

void Schematica::postJob(std::unique_ptr<PrepareJob> job)
{
    {
        std::lock_guard<std::mutex> guard(m_workMutex);
        m_workJob = std::move(job);
    }
    m_workCv.notify_one();
}

void Schematica::postTrash(std::shared_ptr<const schematic::CellSet> trash)
{
    if (!trash) {
        return;
    }
    if (!ensureWorker()) {
        trash.reset();
        return;
    }
    {
        std::lock_guard<std::mutex> guard(m_workMutex);
        m_workTrash.push_back(std::move(trash));
    }
    m_workCv.notify_one();
}

void Schematica::stopWorker()
{
    HANDLE thread = nullptr;
    {
        std::lock_guard<std::mutex> guard(m_workMutex);
        m_workStop = true;
        thread = static_cast<HANDLE>(m_workThread);
        m_workThread = nullptr;
    }
    m_workCancel.store(true, std::memory_order_relaxed);
    m_workCv.notify_all();
    if (thread == nullptr) {
        return;
    }
    const DWORD waited = WaitForSingleObject(thread, 30000);
    if (waited != WAIT_OBJECT_0) {
        log().warn(L"Schematica: the background loader thread did not stop in 30 s");
        while (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0) Sleep(1);
    }
    CloseHandle(thread);
    std::unique_ptr<OutlineJob> outlineWork,outlineDone;
    std::unique_ptr<PrepareJob> job;
    std::unique_ptr<PrepareJob> done;
    std::vector<std::shared_ptr<const schematic::CellSet>> trash;
    std::unique_ptr<SelectionSaveJob> saveDone;
    std::vector<std::unique_ptr<SelectionSaveJob>> saveTrash;
    {
        std::lock_guard<std::mutex> guard(m_workMutex);
        outlineWork=std::move(m_outlineWork); outlineDone=std::move(m_outlineDone);
        job = std::move(m_workJob);
        done = std::move(m_workDone);
        trash.swap(m_workTrash);
        saveDone=std::move(m_saveDone);
        saveTrash.swap(m_saveTrash);
    }
}

void Schematica::resolvePalette(const std::vector<schematic::Placement>& live)
{
    std::vector<schematic::Placement> palettes;
    std::set<std::size_t> paletteBases;
    std::map<std::uint64_t, std::size_t> materialBases;
    using PaletteKey = std::tuple<const void*, int, int>;
    std::map<PaletteKey, std::size_t> known;
    std::size_t nextBase = 0;
    for (const auto& p : live) {
        materialBases[p.placementId] = p.paletteBase;
        if (p.data) nextBase = std::max(nextBase, p.paletteBase + p.data->palette.size());
        known[{p.data.get(), p.rotation & 3, p.mirror}] = p.paletteBase;
        if (paletteBases.insert(p.paletteBase).second) palettes.push_back(p);
    }
    {
        std::lock_guard guard(m_filesMutex);
        for (const auto& placed : m_placements) {
            if (materialBases.contains(placed->spec.id)) continue;
            const auto f = std::find_if(m_files.begin(), m_files.end(), [&](const auto& one) { return one->name == placed->spec.schematic; });
            if (f == m_files.end() || !(*f)->loaded) continue;
            schematic::Placement p;
            p.data = (*f)->loaded; p.placementId = placed->spec.id;
            p.rotation = placed->spec.rotation; p.mirror = static_cast<int>(placed->spec.mirror);
            const auto [at, fresh] = known.emplace(PaletteKey{p.data.get(), p.rotation & 3, p.mirror}, nextBase);
            p.paletteBase = at->second;
            materialBases[p.placementId] = p.paletteBase;
            if (fresh) { nextBase += p.data->palette.size(); palettes.push_back(std::move(p)); }
        }
    }
    std::vector<std::string> wanted;
    for (const schematic::Placement& bp : palettes) {
        if (!bp.data) {
            continue;
        }
        for (const structure::PaletteEntry& entry : bp.data->palette) {
            if (!entry.name.empty()
                && std::find(wanted.begin(), wanted.end(), entry.name) == wanted.end()) {
                wanted.push_back(entry.name);
            }
        }
    }
    if (std::find(wanted.begin(), wanted.end(), std::string{"minecraft:air"}) == wanted.end()) {
        wanted.emplace_back("minecraft:air");
    }

    blocks::Table table;
    if (!blocks::resolve(wanted, table)) {
        return;
    }

    if (m_prepareLogCount++ < 32) log().info(L"Schematica: resolved {}/{} names (table has {} entries)",
               table.size(), wanted.size(), blocks::lastEntryCount());
    if (table.size() != wanted.size()) {
        std::wstring missing;
        for (const std::string& one : wanted) {
            if (table.find(one) == table.end()) {
                missing += std::wstring(one.begin(), one.end());
                missing += L" ";
            }
        }
        log().warn(L"Schematica: could not resolve {} name(s): {}",
                   wanted.size() - table.size(),
                   missing);
    }

    blocks::calibrateItemIds();

    const auto air = table.find("minecraft:air");

    std::vector<const void*> merged;
    std::vector<structure::PaletteEntry> mergedSources;
    bool baseDrift = false;
    std::set<std::size_t> solved;
    for (const schematic::Placement& bp : palettes) {
        if (!bp.data || !solved.insert(bp.paletteBase).second) continue;
        const structure::Structure& loaded = *bp.data;
        const int quarters = bp.rotation & 3;
        std::vector<const void*> resolved(loaded.palette.size(), nullptr);
        for (std::size_t i = 0; i < loaded.palette.size(); ++i) {
            const structure::PaletteEntry& entry = loaded.palette[i];
            const auto base = table.find(entry.name);
            if (base == table.end()) {
                continue;
            }
            resolved[i] = base->second;
            const auto turnSlot = [&](std::size_t at) {
                if ((quarters == 0 && bp.mirror == 0) || resolved[at] == nullptr) {
                    return;
                }
                resolved[at] = blocks::transformedBlock(resolved[at], quarters, bp.mirror);
            };
            if (entry.states.empty()) {
                turnSlot(i);
                continue;
            }
            const std::vector<structure::StateValue>& states = entry.states;
            std::vector<blocks::WantedState> want;
            want.reserve(states.size());
            for (const structure::StateValue& state : states) {
                blocks::WantedState one;
                one.name = state.name;
                one.isText = state.tag == nbt::Tag::String;
                one.number = state.number;
                one.text = state.text;
                want.push_back(std::move(one));
            }
            std::size_t missing = 0;
            const void* const picked = blocks::stateVariant(base->second, want, &missing);
            if (missing != 0) {
                static std::size_t said = 0;
                if (said < 24) {
                    ++said;
                    std::string text;
                    for (const structure::StateValue& one : states) {
                        text += one.name + "=" + one.normalized() + " ";
                    }
                    log().warn(L"Schematica: could not resolve the state of {} [{}] (rotation "
                               L"{} / states on this block: {})",
                               toUtf16(entry.name),
                               toUtf16(text),
                               quarters * 90,
                               toUtf16(blocks::stateNamesOf(base->second)));
                }
            }
            if (picked != nullptr) {
                resolved[i] = picked;
            }
            turnSlot(i);
        }
        if (bp.paletteBase != merged.size()) {
            baseDrift = true;
        }
        merged.insert(merged.end(), resolved.begin(), resolved.end());
        for (const structure::PaletteEntry& entry : loaded.palette) {
            mergedSources.push_back(entry);
        }
    }
    if (baseDrift) {
        log().error(L"Schematica: the palette slots do not line up with the cell list "
                    L"(internal error) - some blocks may be drawn as the wrong kind");
    }

    std::scoped_lock guard(m_mutex, m_filesMutex);
    m_air = air == table.end() ? nullptr : air->second;
    m_paletteBlocks = std::make_shared<const std::vector<const void*>>(std::move(merged));
    {
        m_materialBases = std::move(materialBases);
        m_slotSources = std::move(mergedSources);
        m_slotItems.assign(m_slotSources.size(), {});
        m_placementInfo.clear();
        m_slotItemsDone = 0;
        m_iconIds.clear(); m_stackSizes.clear();
        ++m_materialRevision;
    }
    m_palette = std::move(table);

    blocks::setAirBlock(m_air);

    {
        std::vector<const void*> all;
        blocks::Table everything;
        if (blocks::resolve({}, everything)) {
            all.reserve(everything.size() * 4);
            std::size_t defaults = 0;
            for (const auto& [name, pointer] : everything) {
                all.push_back(pointer);
                ++defaults;
                blocks::appendStateVariants(pointer, all);
            }
            blockwrite::setKnownBlocks(all.data(), all.size());
            if (m_prepareLogCount++ < 32) log().info(L"Schematica: {} known block(s) for the air check ({} kinds)",
                       all.size(),
                       defaults);
        } else {
            log().warn(L"Schematica: could not list the block table - nothing will be placed");
        }
    }
    m_paletteStale = false;
}

namespace {

struct ImportJob {
    Schematica* owner = nullptr;
    void* window = nullptr;
};

}

unsigned long __stdcall Schematica::importThreadMain(void* param)
{
    std::unique_ptr<ImportJob> job(static_cast<ImportJob*>(param));
    if (job && job->owner != nullptr) {
        job->owner->runImport(job->window);
    }
    return 0;
}

void Schematica::startImport()
{
    std::lock_guard<std::mutex> guard(m_importMutex);
    if (m_importStop) return;
    if (m_importBusy.exchange(true)) {
        return;
    }
    if (m_importThread != nullptr) {
        while (WaitForSingleObject(static_cast<HANDLE>(m_importThread), INFINITE)
               != WAIT_OBJECT_0) Sleep(1);
        CloseHandle(static_cast<HANDLE>(m_importThread));
        m_importThread = nullptr;
    }
    auto job = std::make_unique<ImportJob>();
    job->owner = this;
    job->window = GetForegroundWindow();
    const HANDLE thread =
        CreateThread(nullptr, 0, &Schematica::importThreadMain, job.get(), 0, nullptr);
    if (thread == nullptr) {
        m_importBusy.store(false, std::memory_order_relaxed);
        log().warn(L"Schematica: could not start the file chooser thread");
        return;
    }
    job.release();
    m_importThread = thread;
}

void Schematica::stopImport()
{
    HANDLE thread = nullptr;
    {
        std::lock_guard<std::mutex> guard(m_importMutex);
        m_importStop = true;
        thread = static_cast<HANDLE>(m_importThread);
    }
    if (thread == nullptr) return;
    const DWORD threadId = GetThreadId(thread);
    bool warned = false;
    for (int waited = 0; WaitForSingleObject(thread, 100) != WAIT_OBJECT_0; ++waited) {
        EnumThreadWindows(threadId, [](HWND window, LPARAM) -> BOOL {
            PostMessageW(window, WM_CLOSE, 0, 0);
            return TRUE;
        }, 0);
        if (!warned && waited >= 300) {
            warned = true;
            log().warn(L"Schematica: the import thread did not stop in 30 s; waiting until it exits");
        }
    }
    std::lock_guard<std::mutex> guard(m_importMutex);
    CloseHandle(thread);
    m_importThread = nullptr;
}

void Schematica::runImport(void* ownerWindow)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    wchar_t chosen[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(ownerWindow);
    ofn.lpstrFilter = L"Schematic (*.mcstructure;*.litematic)\0*.mcstructure;*.litematic\0"
                      L"Structure (*.mcstructure)\0*.mcstructure\0Litematica (*.litematic)\0*.litematic\0"
                      L"All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = chosen;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Tsukuyomi - import a schematic (.mcstructure / .litematic)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    bool stopping = false;
    {
        std::lock_guard<std::mutex> guard(m_importMutex);
        stopping = m_importStop;
    }
    const bool picked = !stopping && GetOpenFileNameW(&ofn) != FALSE;
    {
        std::lock_guard<std::mutex> guard(m_importMutex);
        stopping = m_importStop;
    }

    if (picked && !stopping) {
        const std::filesystem::path from = chosen;
        const std::filesystem::path dir = paths::schematicsDir();
        if (dir.empty()) {
            log().warn(L"Schematica: no place to put the file");
        } else {
            const std::filesystem::path to = dir / from.filename();
            std::error_code ec;
            std::filesystem::copy_file(
                from, to, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                log().warn(L"Schematica: could not copy {} ({})", from.filename().wstring(),
                           toUtf16(ec.message()));
            } else {
                {
                    std::lock_guard<std::mutex> guard(m_importMutex);
                    m_importedStem = to.stem().wstring();
                }
                m_importDone.store(true, std::memory_order_release);
                log().success(L"Schematica: imported {}", to.filename().wstring());
            }
        }
    }

    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    m_importBusy.store(false, std::memory_order_relaxed);
}

void Schematica::finishImport()
{
    std::wstring stem;
    {
        std::lock_guard<std::mutex> guard(m_importMutex);
        stem = m_importedStem;
    }
    {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        for (const auto& bp : m_files) {
            if (bp->name != stem) continue;
            bp->generation = ++m_nextFileGeneration;
            bp->loaded.reset();
            bp->ready = false;
            bp->sizeX.store(0);
            bp->sizeY.store(0);
            bp->sizeZ.store(0);
            bp->solidCount.store(0);
            bp->paletteCounts.clear();
        }
    }
    scanFiles();
    std::size_t count = 0;
    {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        count = m_fileStems.size();
    }
    m_reloadPending.store(true, std::memory_order_relaxed);
    log().info(L"Schematica: now using {} ({} file(s))", stem, count);

    uiprobe::refreshOwnPage();
}

void Schematica::onScansReady()
{
    MaterialCollector::instance().onScansReady();
    m_useButton = GameButtons::instance().watchButton(gamebuttonlogic::button::buildOrInteract);
    settingscommand::addExtra(name(), [this] {
        std::vector<MenuItem> items;
        std::vector<std::wstring> choices;
        for (std::size_t i=0;i<placementCount();++i) {
            schematic::PlacementSpec p;
            if (placementSpec(placementId(i),p)) choices.push_back(p.name);
        }
        if (!choices.empty()) items.push_back(menu::choice(L"Placement",std::move(choices),
            [this] { for (std::size_t i=0;i<placementCount();++i) if (placementId(i)==selectedPlacement()) return static_cast<int>(i); return -1; },
            [this](int i) { if (i>=0) selectPlacement(placementId(static_cast<std::size_t>(i))); }));
        return items;
    });

    scanFiles();
    std::lock_guard<std::mutex> guard(m_filesMutex);
    if (m_fileStems.empty()) {
        log().info(L"Schematica: no .mcstructure / .litematic found in {}",
                   paths::schematicsDir().wstring());
        return;
    }
    log().info(L"Schematica: {} file(s) in {}", m_fileStems.size(),
               paths::schematicsDir().wstring());
    if (enabled()) {
        m_reloadPending.store(true, std::memory_order_relaxed);
    }
}

void Schematica::shutdown()
{
    MaterialCollector::instance().shutdown();
    lookplace::cancel();
    stopPaste();
    m_shuttingDown.store(true, std::memory_order_relaxed);
    saveWorld(true);
    worldmesh::setDebugLines(worldmesh::LineSource::Schematica,{});
    worldmesh::setDebugLines(worldmesh::LineSource::AreaSelection,{});
    stopImport();
    stopWorker();
    if (enabled()) {
        m_enabledAtShutdown.store(true, std::memory_order_relaxed);
        setEnabled(false);
    }
    boxes::setStyle(false, 0.0F, false);
    const auto until = GetTickCount64() + 1500;
    while ((m_clearRequest.load(std::memory_order_acquire) || m_pasteBusy.load()) && GetTickCount64() < until) Sleep(1);
    if (m_pasteBusy.load()) log().warn(L"Schematica: paste stopped on END; no view update was available to restore command feedback");
    if (m_clearRequest.exchange(false)) schematicworld::clear();
}

void Schematica::onEnabledChanged(bool enabled)
{
    m_outlinesDirty.store(true);
    if (enabled) m_reloadPending.store(true, std::memory_order_relaxed);
    else {
        lookplace::cancel();
        stopPaste();
        worldmesh::setDebugLines(worldmesh::LineSource::Schematica,{});
        worldmesh::setDebugLines(worldmesh::LineSource::AreaSelection,{});
        m_clearRequest.store(true, std::memory_order_release);
        m_ghostWanted.store(false, std::memory_order_relaxed);
    }
}

void Schematica::onUpdate()
{
    if (GameData::instance().msSinceView()>2000 && blockwrite::renderRegion()==nullptr) leaveWorld();
    saveWorld(false);
    MaterialCollector::instance().watchConfigFile();
    std::lock_guard<std::mutex> keysGuard(m_keysMutex);
    if (m_pageKey.triggered() && input::isInGameplay() && !writes::blocked("Schematica:page")) {
        m_pageRequested.store(true, std::memory_order_relaxed);
    }
    const bool up = m_layerUpKey.triggered();
    const bool down = m_layerDownKey.triggered();
    const bool easyToggle = m_easyToggleKey.triggered();
    const bool pickBlock = m_pickBlockKey.triggered();
    bool pressed[6]{};
    for (int i=0;i<6;++i) pressed[i]=m_placementKeys[i].triggered();
    for (int i=0;i<4;++i) {
        const bool pressedArea=m_areaKeys[i].triggered();
        if (pressedArea && enabled() && input::isInGameplay()) selectionAction(i);
    }
    if (!enabled() || !input::isInGameplay()) {
        return;
    }
    if (easyToggle) m_easyPlace.store(!m_easyPlace.load());
    if (pickBlock) m_pickRequested.store(true);
    if (pressed[5]) {
        const auto current=selectedPlacement();
        std::uint64_t next=0;
        bool after=false;
        for (std::size_t i=0;i<placementCount();++i) {
            schematic::PlacementSpec p;
            if (!placementSpec(placementId(i),p)) continue;
            if (p.enabled && (next==0 || after)) { next=p.id; if (after) break; }
            if (p.id==current) after=true;
        }
        if (next) { selectPlacement(next); m_selectionNoticePending.store(true); }
    }
    schematic::PlacementSpec p;
    if (placementSpec(selectedPlacement(),p)) {
        if (pressed[0]!=pressed[1]) {
            const auto view=GameData::instance().playerView();
            float look[3]{}; debuglines::viewVector(view.pitch,view.yaw,look);
            int axis=0; for (int a=1;a<3;++a) if (std::fabs(look[a])>std::fabs(look[axis])) axis=a;
            const int delta=(look[axis]<0?-1:1)*(pressed[0]?1:-1);
            setPlacementPos(p.id,axis,(axis==0?p.x:axis==1?p.y:p.z)+delta);
        }
        if (pressed[2]) setPlacementRotation(p.id,p.rotation+1);
        if (pressed[3]) setPlacementMirror(p.id,static_cast<schematic::Mirror>((static_cast<int>(p.mirror)+1)%3));
        if (pressed[4]) movePlacementToPlayer(p.id);
    }
    if (up != down) {
        stepLayer(up ? 1 : -1);
        log().info(L"Schematica: layer moved {} (key)", up ? L"up" : L"down");
    }
}

namespace {
schematicworld::SchematicHit placementRay(float reach)
{
    auto& data = GameData::instance();
    if (data.msSinceView() > 500) return {};
    const auto* player = static_cast<const std::byte*>(data.player());
    const void* rotation = nullptr;
    float angles[2]{}, direction[3]{};
    if (!player || !memory::copyGuarded(player + 0x250, &rotation, sizeof(rotation))
        || !rotation || !memory::copyGuarded(rotation, angles, sizeof(angles))) return {};
    debuglines::viewVector(angles[0], angles[1], direction);
    const auto view = data.playerView();
    const float eye[3]{view.x, view.y, view.z};
    return schematicworld::traceSchematic(eye, direction, reach);
}
}

void Schematica::placeNotice(const std::string& key, const std::string& message)
{
    std::lock_guard lock(m_placeNoticeMutex);
    if (!m_placeNotices.insert(key).second) return;
    m_placeMessages.push_back(message);
    static unsigned said = 0;
    if (said++ < 64) log().info(L"Schematica: {}", toUtf16(message));
}

void Schematica::stepPickBlock()
{
    if (!m_pickRequested.exchange(false)) return;
    const auto hit = placementRay(kEasyReach);
    if (!hit.hit) return;
    PlacementInfo info;
    {
        std::lock_guard lock(m_filesMutex);
        const auto at = m_placementInfo.find(hit.want);
        if (at == m_placementInfo.end()) return;
        info = at->second;
    }
    if (info.item.empty()) return;
    const auto result = HandRestock::instance().selectItem(info.item, m_pickSlots.load());
    static unsigned said = 0;
    if (said++ < 16) log().info(L"Pick block: {} at {}, {}, {} (result {})", toUtf16(info.item), hit.pos[0], hit.pos[1], hit.pos[2],
        static_cast<int>(result));
    if (result == HandRestock::PickResult::Missing)
        placeNotice("pick.missing:" + info.item, info.item + " is not in your inventory");
    else if (result == HandRestock::PickResult::Unavailable)
        placeNotice("pick.unavailable", "Pick block: cannot change the held item.");
    else if (result == HandRestock::PickResult::NoSlot)
        placeNotice("pick.noslot", "Pick block: no hotbar slot is allowed for picking (Easy Place: pick slots)");
}

int Schematica::slotAt(const std::int32_t pos[3], std::size_t& slot) const
{
    slot = 0;
    if (pos[0] == INT32_MAX || pos[1] == INT32_MAX || pos[2] == INT32_MAX) return 0;
    const schematic::Box range{pos[0], pos[1], pos[2], pos[0]+1, pos[1]+1, pos[2]+1};
    std::lock_guard lock(m_filesMutex);
    for (const auto& placed : m_placements) {
        const auto& p = placed->spec;
        if (!p.enabled) continue;
        const auto file = std::find_if(m_files.begin(), m_files.end(), [&](const auto& f) { return f->name == p.schematic; });
        if (file == m_files.end() || !(*file)->loaded) continue;
        const auto& data = *(*file)->loaded;
        const auto copies=schematic::copiesInBox(p.grid,schematic::placementBox(p,data.sizeX,data.sizeY,data.sizeZ),{p.x,p.y,p.z},range);
        for (auto ix=copies.first[0]; ix<=copies.last[0]; ++ix)
            for (auto iy=copies.first[1]; iy<=copies.last[1]; ++iy)
                for (auto iz=copies.first[2]; iz<=copies.last[2]; ++iz) {
            const auto origin=copies.origin({p.x,p.y,p.z},{ix,iy,iz});
            const auto dx = static_cast<std::int64_t>(pos[0]) - origin[0];
            const auto dy = static_cast<std::int64_t>(pos[1]) - origin[1];
            const auto dz = static_cast<std::int64_t>(pos[2]) - origin[2];
            if (dx <= INT32_MIN || dx > INT32_MAX || dz <= INT32_MIN || dz > INT32_MAX || dy < 0 || dy >= data.sizeY) continue;
            int x = 0, z = 0;
            rotation::inverseLocal(p.rotation, static_cast<int>(p.mirror), static_cast<int>(dx), static_cast<int>(dz), x, z);
            if (x < 0 || x >= data.sizeX || z < 0 || z >= data.sizeZ) continue;
            const auto entry = data.entryAt(data.indexOf(x, static_cast<int>(dy), z));
            if (entry < 0) return 1;
            const auto base = m_materialBases.find(p.id);
            if (base == m_materialBases.end()) return 1;
            slot = base->second + static_cast<std::size_t>(entry);
            return 2;
        }
    }
    return 0;
}

bool Schematica::restrictionItemAt(const std::int32_t pos[3], std::string& item) const
{
    item.clear();
    std::size_t slot = 0;
    const int where = slotAt(pos, slot);
    if (where != 2) return where != 0;
    std::lock_guard lock(m_filesMutex);
    if (slot < m_slotItemsDone && slot < m_slotItems.size() && !m_slotItems[slot].empty()
        && !m_slotItems[slot].front().noItem) item = m_slotItems[slot].front().name;
    return true;
}

const void* Schematica::wantAt(const std::int32_t pos[3]) const
{
    std::size_t slot = 0;
    if (slotAt(pos, slot) != 2) return nullptr;
    std::shared_ptr<const std::vector<const void*>> palette;
    { std::lock_guard guard(m_mutex); palette = m_paletteBlocks; }
    return palette && slot < palette->size() ? (*palette)[slot] : nullptr;
}

bool Schematica::nearPlacement(const std::int32_t pos[3], int margin) const
{
    const auto widen = [](std::int32_t v, int d) {
        return static_cast<std::int32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(v) + d, INT32_MIN, INT32_MAX));
    };
    const schematic::Box range{widen(pos[0], -margin), widen(pos[1], -margin), widen(pos[2], -margin),
        widen(pos[0], margin + 1), widen(pos[1], margin + 1), widen(pos[2], margin + 1)};
    std::lock_guard lock(m_filesMutex);
    for (const auto& placed : m_placements) {
        const auto& p = placed->spec;
        if (!p.enabled) continue;
        const auto file = std::find_if(m_files.begin(), m_files.end(), [&](const auto& f) { return f->name == p.schematic; });
        if (file == m_files.end() || !(*file)->loaded) continue;
        const auto& data = *(*file)->loaded;
        if (!schematic::copiesInBox(p.grid,schematic::placementBox(p,data.sizeX,data.sizeY,data.sizeZ),
                {p.x,p.y,p.z},range).empty()) return true;
    }
    return false;
}

bool Schematica::placementBlocked(const std::int32_t target[3]) const
{
    const auto held = containerui::itemName(HandRestock::instance().heldStack());
    if (held.empty()) return false;
    std::string item;
    if (restrictionItemAt(target, item)) {
        bool on = false;
        int axis = 1, lo = 0, hi = 0;
        layerRange(on, axis, lo, hi);
        const schematic::LayerFilter layer{on, axis, lo, hi};
        if (!layer.shows(target[0], target[1], target[2])) return true;
        return item.empty() || item != held;
    }
    return nearPlacement(target, kRestrictionMargin);
}

namespace {
bool skipUsedNeighbor(void* gameMode, const void* neighbor, const easyplace::Candidate& one)
{
    const int used = blocks::clickUses(gameMode, neighbor, one.neighbor, one.face, one.click);
    if (used == 0) return false;
    static std::mutex mutex;
    static std::set<const void*> said;
    const void* const type = blocks::legacyOfFast(neighbor);
    std::lock_guard lock(mutex);
    if (said.size() < 32 && said.insert(type).second) {
        char name[128]{};
        blocks::nameOfBlock(neighbor, name, sizeof(name));
        if (used > 0) log().info(L"Easy Place: not clicking {} to place against it (the game would use it)", toUtf16(name));
        else log().warn(L"Easy Place: could not tell whether {} is used by a click", toUtf16(name));
    }
    return used > 0;
}

void easyDecline(const wchar_t* reason)
{
    static std::mutex mutex;
    static std::set<std::wstring> said;
    std::lock_guard lock(mutex);
    if (said.size() >= 32 || !said.insert(reason).second) return;
    log().info(L"Easy Place: skipped ({})", reason);
}
}

bool Schematica::onUseItem(void* gameMode, bool& handled)
{
    handled = false;
    if (!enabled() || !m_easyPlace.load()) return false;
    if (m_shuttingDown.load() || m_paletteStale || GetTickCount64() < m_worldSettleUntil) {
        easyDecline(L"use: the world is still settling"); return false;
    }
    const auto hit = placementRay(kEasyReach);
    if (!hit.hit) { easyDecline(L"use: the view does not hit a schematic block"); return false; }
    handled = true;
    const auto sequence = GameButtons::instance().buttonPressSeq(m_useButton);
    if (m_easyPlaceHold.load() && sequence != 0 && m_easyPressSeen && sequence == m_easyPressSeq) return false;
    m_easyPressSeq = sequence; m_easyPressSeen = true;
    return easyPlaceAt(gameMode, hit, 0, false, false);
}

bool Schematica::onBuildBlock(void* gameMode, void* blockPos, unsigned char face, unsigned char extra,
                             bool simTick, bool& handled)
{
    handled = false;
    if (!enabled() || m_shuttingDown.load() || m_paletteStale || GetTickCount64() < m_worldSettleUntil) return false;
    if (m_easyPlace.load()) {
        const auto hit = placementRay(kEasyReach);
        if (hit.hit) {
            handled = true;
            if (m_easyPlaceHold.load() && simTick) return false;
            m_easyPressSeq = GameButtons::instance().buttonPressSeq(m_useButton); m_easyPressSeen = true;
            return easyPlaceAt(gameMode, hit, extra, simTick, false);
        }
    }
    if (!m_placementRestriction.load() && !m_easyPlace.load()) return false;
    std::int32_t pos[3]{}, target[3]{};
    if (!blockPos || !memory::copyGuarded(blockPos,pos,sizeof(pos))) return false;
    const int replaces=buildover::canBuildOver(gameMode,pos);
    if (replaces == 1) std::copy_n(pos,3,target);
    else if (!easyplace::offsetFace(pos,face,1,target)) return false;
    if (replaces < 0 && m_placementRestriction.load() && placementBlocked(pos)) { handled=true; return false; }
    if (m_easyPlace.load() && blockPos && memory::copyGuarded(blockPos, pos, sizeof(pos))) {
        std::size_t slot = 0;
        if (slotAt(pos, slot) != 0) {
            handled = true;
            const void* want = wantAt(pos);
            const void* real = schematicworld::realBlockAt(pos);
            if (!simTick && want && real && want != real && !schematicworld::emptyBlock(real))
                watchPlacement(pos, want, blocks::legacyOfFast(real));
            return false;
        }
    }
    if (m_easyPlace.load()) {
        bool on = false;
        int axis = 1, lo = 0, hi = 0;
        layerRange(on, axis, lo, hi);
        const void* want = wantAt(target);
        const void* targetReal = schematicworld::realBlockAt(target);
        if (want && !schematicworld::emptyBlock(want) && targetReal && schematicworld::emptyBlock(targetReal)
            && schematic::LayerFilter{on, axis, lo, hi}.shows(target[0], target[1], target[2])) {
            handled = true;
            if (m_easyPlaceHold.load() && simTick) return false;
            m_easyPressSeq = GameButtons::instance().buttonPressSeq(m_useButton); m_easyPressSeen = true;
            schematicworld::SchematicHit hit;
            hit.hit = true; std::copy_n(target, 3, hit.pos); hit.want = want; hit.realEmpty = true;
            return easyPlaceAt(gameMode, hit, extra, simTick, false);
        }
        if (want && targetReal && targetReal == want && replaces != 1) {
            handled = true;
            return false;
        }
    }
    if (!placementBlocked(target)) return false;
    handled = true;
    if (gameMode && !simTick && replaces == 0) {
        const void* clicked = schematicworld::realBlockAt(pos);
        const int useFace = clicked && !schematicworld::emptyBlock(clicked) ? safeUseFace(pos) : -1;
        if (useFace >= 0) {
            float click[3]{0.5F, 0.5F, 0.5F};
            const int axis = useFace < 2 ? 1 : useFace < 4 ? 2 : 0;
            click[axis] = (useFace & 1) != 0 ? 1.0F : 0.0F;
            if (hooks::callOriginalBuildBlock(gameMode, pos, static_cast<unsigned char>(useFace), click, extra, simTick)) return true;
        }
    }
    const auto now = GetTickCount64();
    {
        std::lock_guard lock(m_placeNoticeMutex);
        if (m_restrictedAt == 0 || now - m_restrictedAt >= 1000) {
            m_restrictedAt = now; m_placeMessages.emplace_back("Placement restricted");
        }
    }
    return false;
}

namespace {
std::wstring blockText(const void* block)
{
    if (!block) return L"(null)";
    char name[128]{};
    char states[16][blocks::kStateTextBytes]{};
    blocks::nameOfBlock(block, name, sizeof(name));
    const int count = blocks::statesOfBlock(block, states, 16);
    std::string text = name[0] ? name : "?";
    for (int i = 0; i < count; ++i) { text += i == 0 ? " [" : ", "; text += states[i]; }
    if (count > 0) text += "]";
    return toUtf16(text);
}
std::wstring statesText(const materialrules::States& states)
{
    std::string text;
    for (const auto& [name, value] : states) { if (!text.empty()) text += ", "; text += name + ": " + value; }
    return toUtf16(text);
}
materialrules::States statesOf(const void* block)
{
    std::vector<std::array<char, blocks::kStateTextBytes>> lines(16);
    int rows = 0;
    for (;;) {
        rows = blocks::statesOfBlock(block, reinterpret_cast<char (*)[blocks::kStateTextBytes]>(lines.data()), static_cast<int>(lines.size()));
        if (static_cast<std::size_t>(rows) < lines.size()) break;
        lines.resize(lines.size() * 2);
    }
    std::vector<std::string> text;
    for (int row = 0; row < rows; ++row) text.emplace_back(lines[static_cast<std::size_t>(row)].data());
    return materialrules::parseStates(text);
}
bool lookClose(flexible::Look a, flexible::Look b)
{
    return std::fabs(std::remainder(a.pitch - b.pitch, 360.0F)) <= 0.5F && std::fabs(std::remainder(a.yaw - b.yaw, 360.0F)) <= 0.5F;
}
bool queryAt(const placementquery::HeldBlock& held, const std::int32_t target[3], const easyplace::Candidate& one,
             flexible::Look look, const float* eye, const void*& out)
{
    out = nullptr;
    lookplace::ScopedLook scopedLook(look);
    if (!scopedLook.ok()) return false;
    std::optional<lookplace::ScopedPosition> moved;
    if (eye) { moved.emplace(eye); if (!moved->ok()) return false; }
    return placementquery::query(held, GameData::instance().player(), target, one.face, one.click, out);
}
template <class Accept>
bool holdsAhead(const placementquery::HeldBlock& held, const std::int32_t target[3], const easyplace::Candidate& one,
                flexible::Look look, const easyplace::Motion& motion, int ticks, Accept&& accept)
{
    float eyes[easyplace::kMaxAheadEyes][3]{};
    const auto count = easyplace::aheadEyes(motion, ticks, eyes);
    for (std::size_t i = 0; i < count; ++i) {
        const void* predicted = nullptr;
        if (!queryAt(held, target, one, look, eyes[i], predicted) || !accept(predicted)) return false;
    }
    return true;
}
bool readMotionLogged(easyplace::Motion& motion)
{
    static unsigned failures = 0;
    if (lookplace::readMotion(motion)) { failures = 0; return true; }
    static unsigned said = 0;
    if (said++ < 8) log().warn(L"Easy Place: the player motion could not be read ({}); blocks that face up or down are not placed this time",
        lookplace::motionFailure());
    if (++failures >= 40)
        notice::failOnce("Schematica.easyMotion", L"Easy Place: the player motion keeps failing to read; blocks that face up or down are not placed",
            "Easy Place: cannot read the player motion; blocks that face up or down are not placed.");
    return false;
}
std::size_t easyCandidates(const std::int32_t target[3], std::array<easyplace::Candidate, 11>& out)
{
    std::array<easyplace::Candidate, 10> sides;
    const auto count = easyplace::candidates(target, sides);
    std::copy_n(sides.begin(), count, out.begin());
    easyplace::Candidate air;
    std::copy_n(target, 3, air.neighbor); air.face = 1;
    air.click[0] = air.click[2] = 0.5F; air.click[1] = 1.0F;
    out[count] = air;
    return count + 1;
}
void standEyes(const std::int32_t target[3], float above[3], float below[3], float away[3])
{
    const float cx = static_cast<float>(target[0]) + 0.5F, cz = static_cast<float>(target[2]) + 0.5F;
    const float by = static_cast<float>(target[1]);
    above[0] = below[0] = cx; above[2] = below[2] = cz;
    above[1] = by + 3.0F; below[1] = by - 1.0F;
    const auto view = GameData::instance().playerView();
    const float px = view.x, py = view.y, pz = view.z;
    float dx = px - cx, dz = pz - cz;
    const float length = std::sqrt(dx * dx + dz * dz);
    if (!(length > 0.01F)) { dx = 1.0F; dz = 0.0F; } else { dx /= length; dz /= length; }
    away[0] = cx + dx * 3.5F; away[1] = std::isfinite(py) ? py : by + 1.62F; away[2] = cz + dz * 3.5F;
}
unsigned filledFaces(const std::int32_t pos[3])
{
    unsigned mask = 0;
    for (int f = 0; f < 6; ++f) {
        std::int32_t side[3]{};
        if (!easyplace::offsetFace(pos, f, 1, side)) continue;
        const void* block = schematicworld::realBlockAt(side);
        if (block && !schematicworld::emptyBlock(block)) mask |= 1u << f;
    }
    return mask;
}
std::vector<std::array<std::int32_t, 3>> learnSpots(const std::int32_t target[3], unsigned covered, std::size_t limit)
{
    struct Spot { std::array<std::int32_t, 3> pos; unsigned mask; int distance; };
    std::vector<Spot> spots;
    constexpr int kRadius = 3;
    for (int dx = -kRadius; dx <= kRadius; ++dx) for (int dy = -kRadius; dy <= kRadius; ++dy) for (int dz = -kRadius; dz <= kRadius; ++dz) {
        if (dx == 0 && dy == 0 && dz == 0) continue;
        const std::int64_t at[3]{static_cast<std::int64_t>(target[0]) + dx, static_cast<std::int64_t>(target[1]) + dy,
                                 static_cast<std::int64_t>(target[2]) + dz};
        if (std::any_of(at, at + 3, [](std::int64_t v) { return v <= INT32_MIN || v >= INT32_MAX; })) continue;
        const std::int32_t pos[3]{static_cast<std::int32_t>(at[0]), static_cast<std::int32_t>(at[1]), static_cast<std::int32_t>(at[2])};
        const void* block = schematicworld::realBlockAt(pos);
        if (!block || !schematicworld::emptyBlock(block)) continue;
        const unsigned mask = filledFaces(pos);
        if (mask != 0) spots.push_back({{pos[0], pos[1], pos[2]}, mask, std::abs(dx) + std::abs(dy) + std::abs(dz)});
    }
    std::stable_sort(spots.begin(), spots.end(), [](const Spot& a, const Spot& b) { return a.distance < b.distance; });
    std::vector<std::array<std::int32_t, 3>> out;
    for (const auto& spot : spots) {
        if ((spot.mask & ~covered) == 0) continue;
        out.push_back(spot.pos); covered |= spot.mask;
        if (out.size() >= limit || covered == 0x3F) break;
    }
    return out;
}
}

std::shared_ptr<const Schematica::PlaceProfile> Schematica::placeProfile(const void* type) const
{
    std::lock_guard lock(m_profileMutex);
    const auto at = m_placeProfiles.find(type);
    return at == m_placeProfiles.end() ? nullptr : at->second;
}

bool Schematica::profileNeedsLearning(const std::shared_ptr<const PlaceProfile>& profile, const std::int32_t target[3]) const
{
    if (!profile) return true;
    if (!profile->thin() || profile->learnings >= kProfileRelearnLimit) return false;
    return (filledFaces(target) & ~profile->coveredFaces) != 0;
}

std::shared_ptr<const Schematica::PlaceProfile> Schematica::buildPlaceProfile(const placementquery::HeldBlock& held,
                                                                             const std::int32_t target[3])
{
    if (!held.type) return nullptr;
    const auto known = placeProfile(held.type);
    if (known && !profileNeedsLearning(known, target)) return known;
    flexible::Look real;
    if (!lookplace::readLook(real)) return known;
    auto profile = known ? std::make_shared<PlaceProfile>(*known) : std::make_shared<PlaceProfile>();
    ++profile->learnings;
    std::vector<const void*>* spotAnswers = nullptr;
    const auto add = [&](const void* block) {
        if (block && std::find(profile->answers.begin(), profile->answers.end(), block) == profile->answers.end())
            profile->answers.push_back(block);
        if (block && spotAnswers && std::find(spotAnswers->begin(), spotAnswers->end(), block) == spotAnswers->end())
            spotAnswers->push_back(block);
    };
    const auto learnAt = [&](const std::int32_t spot[3], bool eyesToo) {
        std::array<easyplace::Candidate, 11> choices;
        const auto count = easyCandidates(spot, choices);
        profile->spotAnswers.emplace_back();
        spotAnswers = &profile->spotAnswers.back();
        float above[3]{}, below[3]{}, away[3]{};
        standEyes(spot, above, below, away);
        const float* eyes[4]{nullptr, above, below, away};
        for (const float* eye : eyes) {
            if (eye && !eyesToo) break;
            bool skip = false;
            for (int k = 0; k < 4 && !skip; ++k) for (std::size_t i = 0; i < count; ++i) {
                const void* out = nullptr;
                if (!queryAt(held, spot, choices[i], {real.pitch, k * 90.0F + 3.0F}, eye, out)) {
                    if (!eye) return false;
                    skip = true; break;
                }
                add(out);
            }
        }
        for (const float pitch : {-89.0F, 89.0F}) for (int k = 0; k < 4; ++k) for (std::size_t i = 0; i < count; ++i) {
            const void* out = nullptr;
            if (!queryAt(held, spot, choices[i], {pitch, k * 90.0F + 3.0F}, nullptr, out)) return false;
            add(out);
        }
        const unsigned filled = filledFaces(spot);
        profile->coveredFaces |= filled;
        if (std::popcount(filled) >= 2) profile->multiSupport = true;
        return true;
    };
    if (!learnAt(target, true)) return known;
    const auto spots = learnSpots(target, profile->coveredFaces, 4);
    for (const auto& spot : spots) if (!learnAt(spot.data(), false)) break;
    if (!profile->answers.empty()) {
        if (profile->states.empty()) {
            std::vector<const void*> variants;
            blocks::appendStateVariants(profile->answers.front(), variants);
            for (const void* block : variants) profile->states.emplace(block, statesOf(block));
        }
        for (const void* block : profile->answers) if (!profile->states.count(block)) profile->states.emplace(block, statesOf(block));
        std::vector<std::vector<materialrules::States>> answered;
        for (const auto& spot : profile->spotAnswers) {
            auto& one = answered.emplace_back();
            for (const void* block : spot) {
                if (!profile->states.count(block)) profile->states.emplace(block, statesOf(block));
                one.push_back(profile->states[block]);
            }
        }
        profile->varying = easyplace::placementStates(answered, profile->multiSupport);
    }
    static unsigned said = 0;
    if (said++ < 48) {
        std::wstring varying;
        for (const auto& name : profile->varying) { if (!varying.empty()) varying += L", "; varying += toUtf16(name); }
        log().info(L"Easy Place: learned {} ({} states, {} answers from {} spot(s), learning {}, faces {:#x}, multi-support {}, set by placement: [{}])",
            profile->answers.empty() ? std::wstring(L"(no answer)") : blockText(profile->answers.front()), profile->states.size(),
            profile->answers.size(), spots.size() + 1, profile->learnings, profile->coveredFaces, profile->multiSupport, varying);
    }
    std::lock_guard lock(m_profileMutex);
    auto& slot = m_placeProfiles[held.type];
    slot = std::move(profile);
    return slot;
}

void Schematica::stepEasyProfile()
{
    placementquery::HeldBlock wanted;
    std::int32_t at[3]{};
    {
        std::lock_guard lock(m_profileMutex);
        wanted = m_profileWanted; std::copy_n(m_profileWantedAt, 3, at);
        m_profileWanted = {};
    }
    if (wanted.type) buildPlaceProfile(wanted, at);
    const auto now = GetTickCount64();
    if (now - m_profileStepAt < 100) return;
    m_profileStepAt = now;
    const void* stack = HandRestock::instance().heldStack();
    placementquery::HeldBlock held;
    if (!stack || !placementquery::readHeld(stack, held)) return;
    const auto hit = placementRay(kEasyReach);
    if (!hit.hit) return;
    if (!held.type) {
        const void* type = nullptr;
        if (!memory::copyGuarded(static_cast<const std::byte*>(hit.want) + 0x68, &type, sizeof(type)) || !type) return;
        held.type = type; held.itemValue = 0;
    }
    if (!profileNeedsLearning(placeProfile(held.type), hit.pos)) return;
    {
        std::lock_guard lock(m_filesMutex);
        const auto info = m_placementInfo.find(hit.want);
        if (info == m_placementInfo.end() || info->second.item != containerui::itemName(stack)) return;
    }
    buildPlaceProfile(held, hit.pos);
}

void Schematica::watchPlacement(const std::int32_t pos[3], const void* want, const void* type)
{
    std::lock_guard lock(m_watchMutex);
    for (auto& w : m_easyWatches) if (std::equal(pos, pos + 3, w.pos)) {
        if (w.blind && w.clickAt != 0 && GetTickCount64() - w.clickAt < 3000) return;
        w.want = want; w.type = type; w.since = GetTickCount64(); w.clicks = 0; w.clickAt = 0; w.clickedFrom = nullptr; w.blind = false; return;
    }
    if (m_easyWatches.size() >= 32) m_easyWatches.erase(m_easyWatches.begin());
    EasyWatch w;
    std::copy_n(pos, 3, w.pos); w.want = want; w.type = type; w.since = GetTickCount64();
    m_easyWatches.push_back(w);
}

Schematica::ClickPlan Schematica::planClicks(void* gameMode, const std::int32_t pos[3], const materialrules::States& want)
{
    ClickPlan plan;
    const void* real = schematicworld::realBlockAt(pos);
    if (!gameMode || !real || schematicworld::emptyBlock(real)) return plan;
    const auto differs = [&](const void* block) {
        const auto have = statesOf(block);
        int count = 0;
        for (const auto& [name, value] : want) {
            const auto at = have.find(name);
            if (at == have.end() || at->second != value) ++count;
        }
        return count;
    };
    plan.differs = differs(real);
    plan.face = safeUseFace(pos);
    flexible::Look real0;
    if (plan.differs == 0 || plan.face < 0 || !lookplace::readLook(real0)) return plan;
    float click[3]{0.5F, 0.5F, 0.5F};
    const int axis = plan.face < 2 ? 1 : plan.face < 4 ? 2 : 0;
    click[axis] = (plan.face & 1) != 0 ? 1.0F : 0.0F;
    const flexible::Look looks[5]{real0, {real0.pitch, 3.0F}, {real0.pitch, 93.0F}, {real0.pitch, 183.0F}, {real0.pitch, 273.0F}};
    int best = plan.differs;
    bool computed = false;
    for (int l = 0; l < 5 && best > 0; ++l) {
        lookplace::ScopedLook look(looks[l]);
        if (!look.ok()) break;
        hooks::DryRun run(true);
        if (!run.ok()) break;
        std::int32_t at[3]{pos[0], pos[1], pos[2]};
        for (int k = 1; k <= 8; ++k) {
            const auto written = run.writes().size();
            bool used = false;
            if (!run.click(gameMode, at, static_cast<unsigned char>(plan.face), click, used)) break;
            if (run.writes().size() == written) break;
            computed = true;
            const void* now = schematicworld::realBlockAt(pos);
            if (!now || schematicworld::emptyBlock(now)) break;
            const int d = differs(now);
            if (d < best) { best = d; plan.clicks = k; plan.look = looks[l]; plan.turned = l != 0; }
            if (now == real) break;
        }
    }
    if (schematicworld::realBlockAt(pos) != real) {
        static bool told = false;
        if (!told) { told = true; log().error(L"Easy Place: a dry-run click was not rolled back at {}, {}, {} (was {}, now {})", pos[0], pos[1], pos[2],
            blockText(real), blockText(schematicworld::realBlockAt(pos))); }
        m_dryRunBroken.store(true);
        plan.clicks = 0;
        return plan;
    }
    plan.computed = computed;
    if (!computed && plan.differs == 1 && blocks::usedByClick(real) == 1 && !blocks::hasBlockEntity(real)) {
        std::vector<structure::StateValue> typed;
        if (blocks::typedStatesOfBlock(real, typed)) {
            for (const auto& state : typed) {
                const auto wanted = want.find(state.name);
                if (wanted == want.end() || wanted->second == state.normalized()) continue;
                if (state.tag == nbt::Tag::Byte && (state.number == 0 || state.number == 1) && wanted->second == (state.number ? "0" : "1")) {
                    plan.clicks = 1; plan.blind = true; plan.look = real0; plan.turned = false;
                }
                break;
            }
        }
    }
    return plan;
}

void Schematica::stepEasyWatch()
{
    std::vector<EasyWatch> list;
    { std::lock_guard lock(m_watchMutex); list.swap(m_easyWatches); }
    if (list.empty()) return;
    if (!enabled() || !m_easyPlace.load()) return;
    const auto now = GetTickCount64();
    void* gameMode = GameData::instance().gameMode();
    const auto view = GameData::instance().playerView();
    std::vector<EasyWatch> keep;
    static unsigned verified = 0, adjusted = 0;
    static std::set<std::pair<const void*, const void*>> mismatched;
    const auto report = [&](const EasyWatch& w, const void* real, const wchar_t* why) {
        if (mismatched.size() >= 48 || !mismatched.insert({w.want, real}).second) return;
        log().warn(L"Easy Place: {} at {}, {}, {}: wanted {} but the world has {}", why, w.pos[0], w.pos[1], w.pos[2],
            blockText(w.want), blockText(real));
    };
    for (auto& w : list) {
        const auto age = now - w.since;
        const void* real = schematicworld::realBlockAt(w.pos);
        if (real == w.want) {
            if (w.clicks > 0 && adjusted++ < 32) log().info(L"Easy Place: adjusted {} at {}, {}, {} with {} click(s)",
                blockText(real), w.pos[0], w.pos[1], w.pos[2], w.clicks);
            else if (w.clicks == 0 && verified++ < 16) log().info(L"Easy Place: verified {} at {}, {}, {}", blockText(real), w.pos[0], w.pos[1], w.pos[2]);
            continue;
        }
        if (!real || schematicworld::emptyBlock(real)) {
            if (age < 3000) keep.push_back(w);
            else if (real) report(w, real, L"nothing was placed");
            continue;
        }
        materialrules::States want;
        {
            std::lock_guard lock(m_filesMutex);
            const auto info = m_placementInfo.find(w.want);
            if (info == m_placementInfo.end()) continue;
            want = info->second.states;
        }
        if (const auto profile = placeProfile(w.type); profile && !profile->states.empty()
            && !easyplace::matchesOn(statesOf(real), want, profile->varying)) {
            if (age < 1500) keep.push_back(w); else report(w, real, L"placed with a different placement state");
            continue;
        }
        if (age >= 10000 || w.clicks >= 8) { report(w, real, L"gave up clicking"); continue; }
        if (w.blind && w.clickAt != 0) {
            if (real == w.clickedFrom && now - w.clickAt < 3000) { keep.push_back(w); continue; }
            report(w, real, real == w.clickedFrom ? L"clicked once but the server did not toggle it" : L"clicked once but it still differs");
            continue;
        }
        const bool waiting = age < 300 || (w.clickAt != 0 && (now - w.clickAt < 100 || (real == w.clickedFrom && now - w.clickAt < 1000)));
        const double dx = w.pos[0] + 0.5 - view.x, dy = w.pos[1] + 0.5 - view.y, dz = w.pos[2] + 0.5 - view.z;
        const bool blocked = !gameMode || std::sqrt(dx * dx + dy * dy + dz * dz) > kEasyReach + 0.5
            || FastUseItem::playerSneaking(gameMode) || lookplace::busy() || HandRestock::instance().itemMovePending()
            || !input::isInGameplay() || m_dryRunBroken.load();
        if (blocked || waiting) { keep.push_back(w); continue; }
        const auto plan = planClicks(gameMode, w.pos, want);
        if (plan.clicks <= 0) {
            report(w, real, plan.face < 0 ? L"placed (no solid neighbour to click it safely)"
                : plan.computed ? L"placed (clicking does not bring it closer)"
                                : L"placed (the game does not compute this click on the client, so it is not clicked)");
            continue;
        }
        float click[3]{0.5F, 0.5F, 0.5F};
        const int axis = plan.face < 2 ? 1 : plan.face < 4 ? 2 : 0;
        click[axis] = (plan.face & 1) != 0 ? 1.0F : 0.0F;
        std::int32_t pos[3]{w.pos[0], w.pos[1], w.pos[2]};
        if (plan.turned) {
            lookplace::Request request;
            request.gameMode = gameMode; request.face = static_cast<std::uint8_t>(plan.face); request.look = plan.look;
            request.owner = "EasyPlace adjust";
            std::copy_n(pos, 3, request.pos); std::copy_n(click, 3, request.click);
            const std::array<std::int32_t, 3> target{pos[0], pos[1], pos[2]};
            request.stillValid = [this, target, real] {
                return enabled() && m_easyPlace.load() && input::isInGameplay() && schematicworld::realBlockAt(target.data()) == real;
            };
            if (!lookplace::schedule(std::move(request))) { keep.push_back(w); continue; }
        } else hooks::callOriginalBuildBlock(gameMode, pos, static_cast<unsigned char>(plan.face), click, 0, false);
        ++w.clicks; w.clickAt = now; w.clickedFrom = real; w.blind = plan.blind;
        keep.push_back(w);
    }
    std::lock_guard lock(m_watchMutex);
    for (auto& w : keep) {
        const bool newer = std::any_of(m_easyWatches.begin(), m_easyWatches.end(),
            [&](const EasyWatch& other) { return std::equal(w.pos, w.pos + 3, other.pos); });
        if (!newer) m_easyWatches.push_back(w);
    }
}

bool Schematica::placementAccepted(const PlaceProfile* profile, const std::set<std::string>& names, const void* predicted,
                                   const void* want, const materialrules::States& wantStates)
{
    if (!predicted) return false;
    if (predicted == want) return true;
    if (!profile || profile->states.empty()) return false;
    const auto at = profile->states.find(predicted);
    return at != profile->states.end() && easyplace::matchesOn(at->second, wantStates, names);
}

bool Schematica::easyPlaceAt(void* gameMode, const schematicworld::SchematicHit& hit, unsigned char extra, bool simTick,
                             bool viewContext)
{
    if (!gameMode) { easyDecline(L"no game mode"); return false; }
    const void* targetReal = schematicworld::realBlockAt(hit.pos);
    if (!targetReal || !schematicworld::emptyBlock(targetReal)) { easyDecline(L"the target is not empty"); return false; }
    if (lookplace::busy()) { queueEasyRetry(hit); easyDecline(L"a rotated placement is pending"); return false; }
    if (easyPickSettling()) { queueEasyRetry(hit); easyDecline(L"waiting after switching the held item"); return false; }
    if (easySneakSettling(gameMode)) { queueEasyRetry(hit); easyDecline(L"waiting for the automatic sneak to reach the server"); return false; }
    m_easyTargetAt = GetTickCount64(); m_easyTargetSeen = true;
    std::copy_n(hit.pos, 3, m_easyTarget);
    PlacementInfo info;
    {
        std::lock_guard lock(m_filesMutex);
        const auto at = m_placementInfo.find(hit.want);
        if (at == m_placementInfo.end()) { easyDecline(L"the item for the block is not resolved yet"); return false; }
        info = at->second;
    }
    if (info.item.empty()) {
        placeNotice("easy.noitem:" + info.block, "Easy Place: no placeable item for " + info.block);
        return false;
    }
    auto& inventory = HandRestock::instance();
    if (inventory.itemMovePending()) { easyDecline(L"an inventory move is pending"); return false; }
    const void* const stack = inventory.heldStack();
    if (containerui::itemName(stack) != info.item) {
        const auto result = inventory.selectItem(info.item, m_pickSlots.load());
        static unsigned said = 0;
        if (said++ < 16) log().info(L"Easy Place: {} -> {} (pick result {})", toUtf16(containerui::itemName(stack)),
            toUtf16(info.item), static_cast<int>(result));
        if (result == HandRestock::PickResult::Missing)
            placeNotice("easy.missing:" + info.item, "Easy Place: " + info.item + " is not in your inventory");
        else if (result == HandRestock::PickResult::Unavailable)
            placeNotice("easy.inventory", "Easy Place: cannot change the held item.");
        else if (result == HandRestock::PickResult::NoSlot)
            placeNotice("easy.noslot", "Easy Place: no hotbar slot is allowed for picking (Easy Place: pick slots)");
        m_easyTargetSeen = false;
        if (result != HandRestock::PickResult::Missing && result != HandRestock::PickResult::Unavailable
            && result != HandRestock::PickResult::NoSlot)
        {
            m_easyPickAt = GetTickCount64();
            m_easyPickSettleUntil = m_easyPickAt + kEasyPickSettleMs;
            m_easyPickInputs = lookplace::inputsSent();
            queueEasyRetry(hit);
        }
        return false;
    }
    placementquery::HeldBlock held;
    if (!placementquery::readHeld(stack, held)) { easyDecline(L"the held item could not be read"); return false; }
    if (!held.type) {
        const void* type = nullptr;
        if (memory::copyGuarded(static_cast<const std::byte*>(hit.want) + 0x68, &type, sizeof(type)) && type) {
            held.type = type; held.itemValue = 0;
        }
    }
    if (!held.type) {
        placeNotice("easy.orientation:" + info.item, "Easy Place: cannot check how " + info.item + " is placed");
        return false;
    }
    std::shared_ptr<const PlaceProfile> profile;
    if (held.type) {
        profile = viewContext ? buildPlaceProfile(held, hit.pos) : placeProfile(held.type);
        if (!profile || (!viewContext && profileNeedsLearning(profile, hit.pos))) {
            {
                std::lock_guard lock(m_profileMutex);
                m_profileWanted = held; std::copy_n(hit.pos, 3, m_profileWantedAt);
            }
            if (!viewContext) queueEasyRetry(hit);
            easyDecline(L"learning how the held block is placed"); m_easyTargetSeen = false; return false;
        }
    }
    flexible::Look real;
    if (!lookplace::readLook(real)) { easyDecline(L"the player rotation could not be read"); return false; }
    std::array<easyplace::Candidate, 11> choices;
    const auto count = easyCandidates(hit.pos, choices);
    std::set<std::string> compared = profile->varying;
    {
        std::vector<materialrules::States> here;
        for (std::size_t i = 0; i < count; ++i) {
            if (i + 1 < count) {
                const void* neighbor = schematicworld::realBlockAt(choices[i].neighbor);
                if (!neighbor || schematicworld::emptyBlock(neighbor)) continue;
            }
            const void* predicted = nullptr;
            if (!placementquery::query(held, GameData::instance().player(), hit.pos, choices[i].face, choices[i].click, predicted)) {
                easyDecline(L"the placement query failed"); return false;
            }
            if (!predicted) continue;
            const auto at = profile->states.find(predicted);
            if (at != profile->states.end()) here.push_back(at->second);
        }
        const auto more = easyplace::varyingStates(here);
        compared.insert(more.begin(), more.end());
    }
    const auto accepted = [&](const void* predicted) { return placementAccepted(profile.get(), compared, predicted, hit.want, info.states); };
    std::array<flexible::Look, 17> looks{};
    std::size_t lookCount = 1;
    looks[0] = real;
    if (m_easyPlaceRotate.load()) {
        for (int k = 0; k < 16; k += 4) looks[lookCount++] = {real.pitch, k * 22.5F + 3.0F};
        for (int k = 0; k < 16; ++k) if (k % 4 != 0) looks[lookCount++] = {real.pitch, k * 22.5F + 3.0F};
    }
    easyplace::Candidate selected;
    flexible::Look selectedLook;
    bool found = false, changed = false;
    const void* firstAnswer = nullptr;
    const bool sneaking = FastUseItem::playerSneaking(gameMode);
    const bool positional = held.type && profile && !compared.empty();
    easyplace::Motion motion;
    const bool motionKnown = positional && readMotionLogged(motion);
    if (positional && !motionKnown) { easyDecline(L"the player motion could not be read"); return false; }
    const bool moving = motionKnown && easyplace::movesVertically(motion);
    bool needSneak = false;
    for (int pass = 0; pass < (sneaking ? 1 : 2) && !found; ++pass)
    for (std::size_t l = 0; l < lookCount && !found; ++l) {
        lookplace::ScopedLook look(looks[l]);
        if (!look.ok()) { easyDecline(L"the player rotation could not be written"); return false; }
        for (std::size_t i = 0; i < count; ++i) {
            const auto& one = choices[i];
            if (i + 1 < count) {
                const void* neighbor = schematicworld::realBlockAt(one.neighbor);
                if (!neighbor || schematicworld::emptyBlock(neighbor)) continue;
                const bool used = !sneaking && skipUsedNeighbor(gameMode, neighbor, one);
                if (used != (pass == 1)) continue;
            } else if (pass == 1) continue;
            if (held.type) {
                const void* predicted = nullptr;
                if (!placementquery::query(held, GameData::instance().player(), hit.pos, one.face, one.click, predicted)) { easyDecline(L"the placement query failed"); return false; }
                if (!firstAnswer) firstAnswer = predicted;
                if (moving && l != 0) {
                    if (!holdsAhead(held, hit.pos, one, looks[l], motion, 2, accepted)) continue;
                } else {
                    if (!accepted(predicted)) continue;
                    if (positional && !holdsAhead(held, hit.pos, one, looks[l], motion, 1, accepted)) continue;
                }
            }
            selected = one; selectedLook = looks[l]; changed = l != 0; found = true; needSneak = pass == 1;
            break;
        }
    }
    if (found && !changed && held.type && profile && !compared.empty()) {
        flexible::Look sent;
        if (!lookplace::lastSentLook(sent)) changed = true;
        else if (!lookClose(sent, real)) {
            const void* predicted = nullptr;
            if (!queryAt(held, hit.pos, selected, sent, nullptr, predicted)) { easyDecline(L"the placement query failed"); return false; }
            changed = !accepted(predicted);
        }
    }
    if (found && needSneak) {
        if (!m_easyAutoSneak.load()) {
            placeNotice("easy.sneak:" + info.block, "Easy Place: " + info.block
                + " goes only against a block that a click would use - sneak while placing, or turn on Easy Place: auto sneak");
            return false;
        }
        if (!autoSneakUsable()) {
            placeNotice("easy.autosneak", "Easy Place: cannot sneak automatically to place " + info.block
                + " against a block that a click would use - sneak while placing");
            return false;
        }
        const auto sent = lookplace::inputsSent();
        if (!m_easySneakAsked || m_easySneakReleased) {
            m_easySneakAsked = true; m_easySneakSeen = false; m_easySneakReleased = false;
            m_easySneakAskedInputs = sent; m_easySneakAskedMs = GetTickCount64();
            static unsigned said = 0;
            if (said++ < 16) log().info(L"Easy Place: sneaking to place {} at {}, {}, {} against a block that a click would use (face {})",
                toUtf16(info.block), hit.pos[0], hit.pos[1], hit.pos[2], selected.face);
        }
        input::holdSneakUntilInput(sent + kEasySneakWaitInputs);
        queueEasyRetry(hit); m_easyTargetSeen = false;
        easyDecline(L"sneaking first to place against a block that a click would use");
        return false;
    }
    if (found) {
        if (!changed) {
            const bool placed = hooks::callOriginalBuildBlock(gameMode, selected.neighbor, selected.face, selected.click, extra, simTick);
            static unsigned said = 0;
            if (placed && said++ < 8) log().info(L"Easy Place: placed at {}, {}, {} (face {}, click y {})",
                hit.pos[0], hit.pos[1], hit.pos[2], selected.face, selected.click[1]);
            if (placed) { watchPlacement(hit.pos, hit.want, held.type); keepEasySneak(); }
            return placed;
        }
        lookplace::Request request;
        request.gameMode = gameMode; request.face = selected.face; request.look = selectedLook; request.owner = "EasyPlace";
        std::copy_n(selected.neighbor, 3, request.pos); std::copy_n(selected.click, 3, request.click);
        const std::array<std::int32_t, 3> target{hit.pos[0], hit.pos[1], hit.pos[2]};
        request.stillValid = [this, target, item = info.item, held, selected, look = selectedLook, profile, want = hit.want,
                              wantStates = info.states, compared] {
            const void* block = schematicworld::realBlockAt(target.data());
            if (!(enabled() && m_easyPlace.load() && input::isInGameplay() && !HandRestock::instance().itemMovePending()
                && containerui::itemName(HandRestock::instance().heldStack()) == item && block && schematicworld::emptyBlock(block)))
                return false;
            if (!held.type || !profile || compared.empty()) return true;
            const void* predicted = nullptr;
            const auto accept = [&](const void* answer) { return placementAccepted(profile.get(), compared, answer, want, wantStates); };
            easyplace::Motion motion;
            if (queryAt(held, target.data(), selected, look, nullptr, predicted) && accept(predicted)
                && readMotionLogged(motion) && holdsAhead(held, target.data(), selected, look, motion, 1, accept)) return true;
            static unsigned said = 0;
            if (said++ < 16) log().info(L"Easy Place: the answer at {}, {}, {} changed before the rotated placement (the player moved); not placed",
                target[0], target[1], target[2]);
            m_easyTargetSeen = false;
            return false;
        };
        if (lookplace::schedule(std::move(request))) { watchPlacement(hit.pos, hit.want, held.type); keepEasySneak(); }
        return false;
    }
    if (moving) {
        m_easyTargetSeen = false;
        easyDecline(L"the player moves up or down in the next tick and the server would place it differently");
        return false;
    }
    {
        static std::mutex mutex;
        static std::set<const void*> said;
        std::lock_guard lock(mutex);
        if (said.size() < 32 && said.insert(hit.want).second) {
            std::wstring answer = L"(none)";
            if (firstAnswer && profile) {
                const auto at = profile->states.find(firstAnswer);
                if (at != profile->states.end()) answer = statesText(at->second);
            }
            std::wstring varying;
            for (const auto& name : compared) { if (!varying.empty()) varying += L", "; varying += toUtf16(name); }
            log().info(L"Easy Place: no way to place {} [{}] from here (set by placement: [{}]; the first answer: [{}])",
                toUtf16(info.block), statesText(info.states), varying, answer);
        }
    }
    static std::mutex standMutex;
    static std::array<std::int32_t, 3> standAt{INT32_MAX, INT32_MAX, INT32_MAX};
    static std::uint64_t standTime = 0;
    bool standAgain = false;
    {
        std::lock_guard lock(standMutex);
        const std::array<std::int32_t, 3> pos{hit.pos[0], hit.pos[1], hit.pos[2]};
        const auto now = GetTickCount64();
        if (pos != standAt || now - standTime >= 1000) { standAt = pos; standTime = now; standAgain = true; }
    }
    if (!standAgain) return false;
    if (held.type && profile && !compared.empty()) {
        float above[3]{}, below[3]{}, away[3]{};
        standEyes(hit.pos, above, below, away);
        const auto reachable = [&](const float* eye) {
            for (std::size_t i = 0; i < count; ++i) {
                if (i + 1 < count) {
                    const void* neighbor = schematicworld::realBlockAt(choices[i].neighbor);
                    if (!neighbor || schematicworld::emptyBlock(neighbor)) continue;
                }
                for (std::size_t l = 0; l < lookCount; ++l) {
                    const void* predicted = nullptr;
                    if (queryAt(held, hit.pos, choices[i], looks[l], eye, predicted) && accepted(predicted)) return true;
                }
            }
            return false;
        };
        std::string where;
        if (reachable(above)) where = std::format("stand within 2 blocks sideways with your feet at Y {} or higher", hit.pos[1] + 1);
        else if (reachable(below)) where = std::format("stand within 2 blocks sideways with your feet at Y {} or lower", hit.pos[1] - 2);
        else if (reachable(away)) where = "stand 2 or more blocks away sideways (close by, it faces up or down)";
        if (!where.empty()) {
            placeNotice(std::format("easy.stand:{},{},{}", hit.pos[0], hit.pos[1], hit.pos[2]),
                "Easy Place: " + info.block + " - the server decides up/down from where you stand: " + where);
            return false;
        }
        static constexpr const char* kSupportSide[6]{"above", "below", "south", "north", "east", "west"};
        std::string supports;
        unsigned supportFaces = 0;
        for (std::size_t i = 0; i + 1 < count; ++i) {
            const auto face = choices[i].face;
            if ((supportFaces >> face & 1u) != 0) continue;
            const void* neighbor = schematicworld::realBlockAt(choices[i].neighbor);
            if (neighbor && !schematicworld::emptyBlock(neighbor)) continue;
            for (std::size_t l = 0; l < lookCount; ++l) {
                const void* predicted = nullptr;
                if (!queryAt(held, hit.pos, choices[i], looks[l], nullptr, predicted) || !accepted(predicted)) continue;
                supportFaces |= 1u << face;
                if (!supports.empty()) supports += " or ";
                supports += kSupportSide[face];
                break;
            }
        }
        if (!supports.empty()) {
            placeNotice(std::format("easy.support:{},{},{}", hit.pos[0], hit.pos[1], hit.pos[2]),
                "Easy Place: " + info.block + " needs a block " + supports
                    + " of it first (in the air the server places it as if the top face was clicked)");
            return false;
        }
    }
    placeNotice(std::format("easy.facing:{},{},{}:{}", hit.pos[0], hit.pos[1], hit.pos[2], m_easyPlaceRotate.load()),
        m_easyPlaceRotate.load() ? "Easy Place: cannot place " + info.block + " with any facing from here"
            : "Easy Place: wrong facing - turn to place " + info.block);
    return false;
}

bool Schematica::easyPickSettling()
{
    if (m_easyPickAt == 0) return false;
    const auto now = GetTickCount64();
    const int announced = AutoTool::instance().selectedSlotAnnounced();
    bool settled = false;
    if (announced == 1) settled = true;
    else if (announced < 0) settled = now >= m_easyPickSettleUntil && lookplace::inputsSent() >= m_easyPickInputs + 2;
    else if (now - m_easyPickAt >= 1000) {
        settled = true;
        static unsigned said = 0;
        if (said++ < 4) log().warn(L"Easy Place: the slot switch was not announced to the server within 1 s; placing anyway");
    }
    if (settled) {
        static unsigned said = 0;
        if (said++ < 8) log().info(L"Easy Place: the slot switch reached the server after {} ms ({})", now - m_easyPickAt,
            announced == 1 ? L"announced" : announced < 0 ? L"the announced slot is unreadable; waited on the clock" : L"gave up waiting");
        m_easyPickAt = 0;
    }
    return !settled;
}

void Schematica::queueEasyRetry(const schematicworld::SchematicHit& hit)
{
    const int tries = m_inEasyRetry ? m_easyRetryTries + 1 : 0;
    if (tries > 3) return;
    m_easyRetry.active = true; std::copy_n(hit.pos, 3, m_easyRetry.pos); m_easyRetry.want = hit.want;
    m_easyRetry.at = GetTickCount64(); m_easyRetry.tries = tries;
}

void Schematica::stepEasyRetry()
{
    if (!m_easyRetry.active) return;
    const auto now = GetTickCount64();
    if (now - m_easyRetry.at > kEasyRetryMs || !m_easyPlace.load() || !input::isInGameplay()) { m_easyRetry.active = false; return; }
    void* gameMode = GameData::instance().gameMode();
    if (lookplace::busy() || HandRestock::instance().itemMovePending() || easyPickSettling() || easySneakSettling(gameMode)) return;
    if (!m_easyRetry.active) return;
    const auto retry = m_easyRetry;
    m_easyRetry.active = false;
    if (!gameMode || wantAt(retry.pos) != retry.want) return;
    const auto view = GameData::instance().playerView();
    const double dx = retry.pos[0] + 0.5 - view.x, dy = retry.pos[1] + 0.5 - view.y, dz = retry.pos[2] + 0.5 - view.z;
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > kEasyReach + 0.5) return;
    schematicworld::SchematicHit hit;
    hit.hit = true; std::copy_n(retry.pos, 3, hit.pos); hit.want = retry.want; hit.realEmpty = true;
    m_inEasyRetry = true; m_easyRetryTries = retry.tries;
    const bool placed = easyPlaceAt(gameMode, hit, 0, false, true);
    m_inEasyRetry = false;
    static unsigned said = 0;
    if (said++ < 16) log().info(L"Easy Place: retried {}, {}, {} after {} ms ({})", retry.pos[0], retry.pos[1], retry.pos[2], now - retry.at,
        placed ? L"placed" : m_easyRetry.active ? L"waiting again" : L"not placed");
}

bool Schematica::autoSneakUsable() const
{
    return ToggleSneakSprint::instance().available() && !writes::blocked("Schematica:autosneak") && !FreeCamera::instance().active();
}

void Schematica::stopEasySneak()
{
    input::holdSneakUntilInput(0);
    m_easySneakAsked = false; m_easySneakSeen = false; m_easySneakReleased = false;
}

void Schematica::keepEasySneak()
{
    if (!m_easySneakAsked) return;
    const auto sent = lookplace::inputsSent();
    input::holdSneakUntilInput(sent + kEasySneakAfterInputs);
    m_easySneakReleased = true; m_easySneakReleasedInputs = sent;
}

bool Schematica::easySneakSettling(void* gameMode)
{
    if (!m_easySneakAsked || m_easySneakReleased) return false;
    const auto sent = lookplace::inputsSent();
    std::uint64_t since = 0;
    if (input::sneakForcedSince(since) && sent > since && gameMode && FastUseItem::playerSneaking(gameMode)) {
        if (!m_easySneakSeen) {
            m_easySneakSeen = true;
            static unsigned said = 0;
            if (said++ < 16) log().info(L"Easy Place: sneaking; the start was sent {} tick(s) after asking", sent - m_easySneakAskedInputs);
        }
        return false;
    }
    const bool stalled = sent == m_easySneakAskedInputs && GetTickCount64() - m_easySneakAskedMs >= kEasySneakStallMs;
    if (sent - m_easySneakAskedInputs < kEasySneakWaitInputs && !stalled) return true;
    static unsigned said = 0;
    if (said++ < 8) {
        if (stalled) log().warn(L"Easy Place: no PlayerAuthInput was counted for {} ms while waiting for the automatic sneak; not placed", kEasySneakStallMs);
        else log().warn(L"Easy Place: the automatic sneak did not start within {} ticks; not placed", kEasySneakWaitInputs);
    }
    placeNotice("easy.sneakfail", "Easy Place: could not sneak automatically - sneak while placing");
    stopEasySneak();
    m_easyRetry.active = false;
    return true;
}

void Schematica::stepEasySneak()
{
    if (!m_easySneakAsked) return;
    if (!enabled() || !m_easyPlace.load() || !m_easyAutoSneak.load() || !input::isInGameplay()) { stopEasySneak(); return; }
    const auto sent = lookplace::inputsSent();
    if (lookplace::busy() && m_easySneakSeen) {
        input::holdSneakUntilInput(sent + 1 + kEasySneakAfterInputs);
        m_easySneakReleased = true; m_easySneakReleasedInputs = sent;
        return;
    }
    if (input::sneakHoldWanted(sent)) return;
    if (!m_easySneakReleased) { m_easySneakReleased = true; m_easySneakReleasedInputs = sent; }
    void* gameMode = GameData::instance().gameMode();
    const auto after = sent - m_easySneakReleasedInputs;
    const bool still = gameMode && FastUseItem::playerSneaking(gameMode);
    if (still && after < kEasySneakWaitInputs) return;
    static unsigned said = 0;
    if (said++ < 16) {
        if (still && input::sneakHeldWithin(input::kSneakHoldGraceMs)) log().info(L"Easy Place: released the automatic sneak; the player keeps sneaking");
        else if (still) log().warn(L"Easy Place: still sneaking {} ticks after releasing the automatic sneak", kEasySneakWaitInputs);
        else log().info(L"Easy Place: the automatic sneak ended {} tick(s) after placing ({} tick(s) from asking)", after,
            sent - m_easySneakAskedInputs);
    }
    stopEasySneak();
}

int Schematica::safeUseFace(const std::int32_t pos[3])
{
    for (const int f : {1, 0, 2, 3, 4, 5}) {
        std::int32_t side[3]{};
        if (!easyplace::offsetFace(pos, f, 1, side)) continue;
        const void* block = schematicworld::realBlockAt(side);
        bool blocksMotion = false, liquid = false;
        if (block && !schematicworld::emptyBlock(block) && blocks::materialFlags(block, blocksMotion, liquid) && blocksMotion && !liquid)
            return f;
    }
    return -1;
}

void Schematica::stepEasyHold()
{
    auto& buttons = GameButtons::instance();
    if (!buttons.buttonHeld(m_useButton)) {
        m_easyTargetSeen = false; m_easyPressSeen = false; return;
    }
    if (!enabled() || !m_easyPlace.load() || !m_easyPlaceHold.load() || m_paletteStale
        || GetTickCount64() < m_worldSettleUntil || !input::isInGameplay() || lookplace::busy()
        || HandRestock::instance().itemMovePending()) return;
    const auto now = GetTickCount64();
    void* gameMode = GameData::instance().gameMode();
    if (!gameMode) { easyDecline(L"hold: no game mode"); return; }
    const auto hit = placementRay(kEasyReach);
    if (!hit.hit) return;
    if (m_easyTargetSeen && std::equal(hit.pos, hit.pos + 3, m_easyTarget) && now - m_easyTargetAt < 300) return;
    m_easyPressSeq = buttons.buttonPressSeq(m_useButton); m_easyPressSeen = true;
    easyPlaceAt(gameMode, hit, 0, false, true);
}

void Schematica::onPlayerViewUpdate()
{
    if (m_shuttingDown.load()) { stepPaste(); return; }
    std::vector<std::string> messages;
    { std::lock_guard lock(m_placeNoticeMutex); messages.swap(m_placeMessages); }
    for (const auto& message : messages) clientchat::printLocal(message);
    syncWorld();
    stepSelection();
    if (m_lockedNoticePending.exchange(false)) clientchat::printLocal("[Tsukuyomi] The selected placement is locked.");
    if (m_selectionNoticePending.exchange(false)) {
        schematic::PlacementSpec p;
        if (placementSpec(selectedPlacement(),p)) {
            log().info(L"Schematica: selected {}",p.name);
            clientchat::printLocal("[Tsukuyomi] Selected placement: "+toUtf8(p.name));
        }
    }
    if (const auto generation=m_loadFileGeneration.exchange(0); generation!=0) {
        int x=0,y=0,z=0;
        if (playerBlockPos(x,y,z)) {
            std::lock_guard lock(m_filesMutex);
            for (const auto& f : m_files) if (f->generation==generation && m_nextId!=UINT64_MAX) {
                auto p=std::make_unique<PlacementEntry>();
                p->spec.id=m_nextId++; p->spec.name=f->name; p->spec.schematic=f->name;
                p->spec.x=x; p->spec.y=y; p->spec.z=z; p->spec.color=unusedColor();
                m_selectedId.store(p->spec.id); m_placements.push_back(std::move(p)); placementChanged(kPrepReload); break;
            }
        }
    }
    if (const auto id=m_moveToPlayerId.exchange(0); id!=0) {
        int x=0,y=0,z=0;
        if (playerBlockPos(x,y,z)) mutatePlacement(id,true,kPrepRedraw,[&](auto& p) { p.x=x;p.y=y;p.z=z; });
    }
    syncPlacementRange();
    const auto generation = blockwrite::regionGeneration();
    if (generation != m_regionGeneration) {
        lookplace::cancel();
        m_easyTargetSeen = false; m_easyPressSeen = false;
        m_easyRetry.active = false; m_easyPickAt = 0;
        if (m_easySneakAsked) stopEasySneak();
        stopPaste();
        m_regionGeneration = generation;
        m_pickedItems.clear();
        m_pickWorked = false; m_pickEmptySince = 0;
        { std::lock_guard lock(m_placeNoticeMutex); m_placeNotices.clear(); m_restrictedAt = 0; }
        { std::lock_guard lock(m_filesMutex); m_placementInfo.clear(); }
        { std::lock_guard lock(m_profileMutex); m_placeProfiles.clear(); m_profileWanted = {}; }
        { std::lock_guard lock(m_watchMutex); m_easyWatches.clear(); }
        { std::lock_guard guard(m_filesMutex); m_slotItems.clear(); m_slotSources.clear(); m_materialBases.clear(); m_slotItemsDone = 0; m_availableByName.clear(); m_availableKnown = false; ++m_materialRevision; }
        m_paletteStale = true;
        m_worldSettleUntil = GetTickCount64() + kWorldSettleMs;
        if (enabled()) m_reloadPending.store(true, std::memory_order_relaxed);
    }
    if (m_pageRequested.exchange(false, std::memory_order_relaxed)) {
        if (!uiprobe::openOwnPage()) {
            log().warn(L"Schematica: the settings page can only be opened from the settings screen for now");
        }
    }
    if (m_importDone.exchange(false, std::memory_order_acquire)) finishImport();
    if (m_clearRequest.load(std::memory_order_acquire)) {
        schematicworld::clear();
        m_ghostWanted.store(false, std::memory_order_relaxed);
        m_clearRequest.store(false, std::memory_order_release);
    }
    if (!m_shuttingDown.load(std::memory_order_relaxed)) {
        if (m_reloadPending.exchange(false, std::memory_order_relaxed)) requestPrepare(kPrepReload);
        pollPrepare();
    }
    m_ghostWanted.store(!m_cellSet->empty() || (m_prepBusy && m_prepHasVisible), std::memory_order_relaxed);
    if (const auto at = m_posChangedAt.load(std::memory_order_relaxed);
        enabled() && !m_shuttingDown.load(std::memory_order_relaxed) && at != 0 && GetTickCount64() - at >= kPosSettleMs) {
        m_posChangedAt.store(0, std::memory_order_relaxed);
        requestPrepare(kPrepRedraw);
    }
    m_redrawPending.exchange(false, std::memory_order_relaxed);
    applyLayerFilter();
    const auto now = GetTickCount64();
    if (m_radiusReadAt == 0 || now - m_radiusReadAt >= 5000) {
        int distance = 0;
        if (gameoptions::getInt("gfx_viewdistance", distance) && distance > 0) m_radiusChunks = distance / 16 + 2;
        else {
            m_radiusChunks = 18;
            static bool told = false;
            if (!told) { told = true; log().warn(L"Schematica: view distance is unavailable; using a radius of 18 chunks"); }
        }
        m_radiusReadAt = now;
    }
    const auto view = GameData::instance().playerView();
    schematicworld::FrameInput in;
    in.now = now; in.eye[0] = view.x; in.eye[1] = view.y; in.eye[2] = view.z;
    in.renderRegion = enabled() && now >= m_worldSettleUntil ? blockwrite::renderRegion() : nullptr;
    in.radiusChunks = m_radiusChunks; in.layer = m_layerFilter;
    in.ghostOverMismatch = m_ghostOverMismatch.load(std::memory_order_relaxed);
    syncVerifier();
    schematicworld::frame(in);
    stepPaste();
    const auto verifierSnap = schematicworld::renderSnapshot();
    const bool highlighting = verifierSnap && verifierSnap->verifierActive;
    boxes::setStyle(enabled() && (m_diffBoxes.load(std::memory_order_relaxed) || highlighting),
        static_cast<float>(m_boxAlpha.load(std::memory_order_relaxed)) / 100.0F,
        m_diffXray.load(std::memory_order_relaxed) || highlighting);
    syncBoxSections();
    updateOutlines();
    resolveMaterialItems();
    refreshAvailableCounts();
    if (enabled() && !m_paletteStale && now >= m_worldSettleUntil) {
        if (m_easyPlace.load()) placementquery::initialize();
        stepPickBlock();
        if (m_easyPlace.load()) stepEasyProfile();
        stepEasySneak();
        stepEasyRetry();
        stepEasyHold();
        stepEasyWatch();
    } else m_pickRequested.store(false);
}

bool Schematica::saveWorldLocked()
{
    if (m_worldKey.empty() || m_worldWriteBlocked) return false;
    m_worldSaveAttemptAt=GetTickCount64();
    const auto base=paths::schematicsDir().parent_path();
    if (base.empty()) return false;
    const auto dir=base/"placements";
    std::error_code ec;
    std::filesystem::create_directories(dir,ec);
    const auto path=dir/(m_worldKey+".json");
    const auto temp=dir/(m_worldKey+".json.tmp");
    if (m_placements.empty() && m_placementsDirtyAt==0 && !m_migrationPending && !std::filesystem::exists(path,ec)) return true;
    ec.clear();
    schematic::PlacementFile file;
    file.kind=m_worldKind; file.levelId=m_worldLevelId; file.name=m_worldName;
    file.selected=m_selectedId.load(); file.nextId=m_nextId;
    for (const auto& p : m_placements) file.placements.push_back(p->spec);
    bool ok=false;
    if (!ec) {
        std::ofstream out(temp,std::ios::binary|std::ios::trunc);
        if (out) {
            out << schematic::toJson(file).dump(2);
            out.close();
            ok=out.good() && MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING)!=FALSE;
        }
    }
    if (!ok) {
        notice::failOnce("Schematica.placementWrite",L"Schematica: could not save placements; the original file and the migration source were kept.",
            "Could not save schematic placements.");
        return false;
    }
    m_placementsDirtyAt=0;
    if (m_migrationPending) {
        m_placementsMigrated=true;
        m_migrationPending=false;
        m_legacyBlueprints=nlohmann::json::array();
    }
    return true;
}

void Schematica::saveWorld(bool force)
{
    std::lock_guard worldLock(m_worldMutex);
    saveSelectionLocked(force);
    std::lock_guard lock(m_filesMutex);
    const auto now=GetTickCount64();
    if (m_worldKey.empty() || (!force && (m_placementsDirtyAt==0 || now-m_placementsDirtyAt<1000
        || now-m_worldSaveAttemptAt<1000))) return;
    saveWorldLocked();
}

void Schematica::leaveWorld()
{
    std::lock_guard worldLock(m_worldMutex);
    std::lock_guard lock(m_filesMutex);
    if (GameData::instance().msSinceView()<=2000 || blockwrite::renderRegion()!=nullptr) return;
    if (!m_inWorld.exchange(false)) return;
    stopPaste();
    saveWorldLocked();
    worldidentity::clear(m_worldEntry);
    saveSelectionLocked();
    {
        std::lock_guard areaLock(m_areaMutex);
        m_area={}; m_areaDirty=false; m_areaWriteBlocked=false; ++m_areaRevision;
    }
    m_areaActions.store(0);
    m_worldKey.clear(); m_worldKind.clear(); m_worldLevelId.clear(); m_worldName.clear();
    m_hasWorldKey.store(false);
    m_worldIndex.reset();
    m_identityResolveAt=0;
    m_placements.clear(); m_selectedId.store(0); m_nextId=1;
    m_migrationPending=false; m_worldWriteBlocked=false;
    m_placementsDirtyAt=0; ++m_placementsRevision;
    m_loadFileGeneration.store(0); m_moveToPlayerId.store(0);
    m_reloadPending.store(true); m_outlinesDirty.store(true);
    worldmesh::setDebugLines(worldmesh::LineSource::Schematica,{});
    worldmesh::setDebugLines(worldmesh::LineSource::AreaSelection,{});
}

void Schematica::syncWorld()
{
    std::lock_guard worldLock(m_worldMutex);
    const auto identity=worldidentity::snapshot();
    const auto now=GetTickCount64();
    bool integrated=GameData::instance().integratedServer();
    if (m_inWorld.load() && identity.entry==m_worldEntry && !m_worldKey.empty()) integrated=m_worldKind=="local";
    if (!m_inWorld.load() || identity.entry!=m_worldEntry) {
        m_identityResolveAt=now+2000;
        m_worldIndex.reset();
    }
    if (integrated && !identity.received) m_worldIndex.update(now,identity.entry);
    auto key=m_inWorld.load() && identity.entry==m_worldEntry && !m_worldKey.empty() ? m_worldKey
        : worldidentity::worldKey(identity.received,integrated,identity.levelId,identity.name,
            toUtf8(m_worldIndex.folderName()),integrated?std::string{}:rejoin::readConnectionUrl(hooks::gameClientInstance()));
    if (!integrated && now<m_identityResolveAt) key.clear();
    if (key.empty() && !integrated && now>=m_identityResolveAt) {
        notice::failOnce("Schematica.unsavedWorld",L"Schematica: placements on this server are not saved (join the server after loading Tsukuyomi to choose a save location).",
            "Placements here are not saved. Join the server after loading Tsukuyomi to choose a save location.");
    }
    std::lock_guard lock(m_filesMutex);
    if (m_inWorld.load() && identity.entry==m_worldEntry && key==m_worldKey) return;
    const auto previousKey=m_worldKey;
    saveWorldLocked();
    saveSelectionLocked();
    const bool sameEntry=m_inWorld.load() && identity.entry==m_worldEntry;
    m_inWorld.store(true); m_worldEntry=identity.entry; m_worldKey=key;
    m_hasWorldKey.store(!key.empty());
    m_worldKind=integrated?"local":"server";
    m_worldLevelId=identity.received?identity.levelId:toUtf8(m_worldIndex.folderName());
    m_worldName=identity.name;
    schematic::PlacementFile file;
    if (sameEntry && key.empty()) return;
    loadSelectionLocked(sameEntry && previousKey.empty() && !key.empty());
    if (sameEntry && previousKey.empty() && !key.empty()) {
        file.selected=m_selectedId.load(); file.nextId=m_nextId;
        for (const auto& p:m_placements) file.placements.push_back(p->spec);
    }
    m_placements.clear(); m_selectedId.store(0); m_nextId=1;
    m_migrationPending=false; m_worldWriteBlocked=false; m_placementsDirtyAt=0;
    m_loadFileGeneration.store(0); m_moveToPlayerId.store(0);
    if (!key.empty()) {
        const auto path=paths::schematicsDir().parent_path()/"placements"/(key+".json");
        std::error_code ec;
        const bool exists=std::filesystem::exists(path,ec);
        if (exists || ec) {
            std::ifstream in(path,std::ios::binary);
            const auto json=nlohmann::json::parse(in,nullptr,false);
            if (json.is_discarded() || !schematic::fromJson(json,file)) {
                file={};
                auto bad=path;
                const auto stamp=std::chrono::system_clock::now().time_since_epoch();
                bad+=L".bad-"+std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(stamp).count());
                std::filesystem::rename(path,bad,ec);
                m_worldWriteBlocked=static_cast<bool>(ec);
                notice::failOnce("Schematica.placementRead",L"Schematica: cannot read the placement JSON; the original file was moved aside and the list starts empty.",
                    "Could not read schematic placements; the original file was preserved.");
            }
        } else if (!m_placementsMigrated) {
            bool migrated=false;
            schematic::migrateLegacy(m_legacyBlueprints,file,migrated);
            m_migrationPending=migrated;
        }
        m_selectedId.store(file.selected); m_nextId=file.nextId;
        for (auto& p : file.placements) {
            auto entry=std::make_unique<PlacementEntry>(); entry->spec=std::move(p);
            m_placements.push_back(std::move(entry));
        }
        if (m_migrationPending) { m_placementsDirtyAt=now; saveWorldLocked(); }
    }
    ++m_placementsRevision;
    m_outlinesDirty.store(true); m_reloadPending.store(true);
}

void Schematica::syncPlacementRange()
{
    const auto now=GetTickCount64();
    if (m_radiusReadAt==0 || now-m_radiusReadAt>=5000) {
        int distance=0;
        if (gameoptions::getInt("gfx_viewdistance",distance) && distance>0) m_radiusChunks=distance/16+2;
        else {
            m_radiusChunks=18;
            static bool told=false;
            if (!told) { told=true;log().warn(L"Schematica: view distance is unavailable; using a radius of 18 chunks"); }
        }
        m_radiusReadAt=now;
    }
    const auto view=GameData::instance().playerView();
    if (!std::isfinite(view.x) || !std::isfinite(view.z)) return;
    const int cx=static_cast<int>(std::floor(view.x))>>4, cz=static_cast<int>(std::floor(view.z))>>4;
    const bool rangeChanged=!m_copyRangeValid || cx!=m_copyCamX || cz!=m_copyCamZ || m_radiusChunks!=m_copyRadius;
    if (!rangeChanged && !m_outlinesDirty.load()) return;
    m_copyCamX=cx; m_copyCamZ=cz; m_copyRadius=m_radiusChunks; m_copyRangeValid=true;
    m_copyRange={(cx-m_radiusChunks)*16,kMinY,(cz-m_radiusChunks)*16,
                 (cx+m_radiusChunks+1)*16,kMaxY+1,(cz+m_radiusChunks+1)*16};
    std::map<std::uint64_t,schematic::CopyRange> copies;
    {
        std::lock_guard lock(m_filesMutex);
        for (const auto& p : m_placements) {
            if (!p->spec.enabled) continue;
            for (const auto& f : m_files) if (f->name==p->spec.schematic && f->sizeX.load()>0) {
                copies[p->spec.id]=p->spec.grid.enabled
                    ? schematic::copiesInBox(p->spec.grid,schematic::placementBox(p->spec,f->sizeX.load(),f->sizeY.load(),f->sizeZ.load()),
                        {p->spec.x,p->spec.y,p->spec.z},m_copyRange) : schematic::CopyRange{};
                break;
            }
        }
    }
    if (copies!=m_rangeCopies) {
        if (m_prepareLogCount++ < 32) {
            std::size_t count=0;for(const auto& [id,range]:copies) { (void)id;std::size_t n=0;if(range.count(n) && n<=SIZE_MAX-count) count+=n; }
            log().info(L"Schematica: {} placement(s) / {} copy(ies) in range",copies.size(),count);
        }
        m_rangeCopies=std::move(copies);
        m_outlinesDirty.store(true);
        if (rangeChanged) requestPrepare(kPrepRedraw);
    }
}

void Schematica::runOutlineJob(OutlineJob& job) try
{
    std::vector<schematic::GridEdge> edges;
    for(const auto& entry:job.entries) {
        if (m_workCancel.load() || job.revision!=m_outlineRevision.load()) { job.failed=true;return; }
        if (!schematic::gridOutline(entry.box,entry.copies,edges,&m_workCancel)) { job.failed=true;return; }
        for(const auto& edge:edges) {
            if (m_workCancel.load() || job.revision!=m_outlineRevision.load()) { job.failed=true;return; }
            worldmesh::DebugLine line{};
            for(int a=0;a<3;++a) { line.a[a]=static_cast<float>(edge[a]);line.b[a]=static_cast<float>(edge[a+3]); }
            std::copy_n(entry.color,4,line.rgba);line.width=entry.width;
            job.lines.push_back(line);
        }
    }
} catch(const std::bad_alloc&) { job.lines.clear();job.failed=true; }

void Schematica::updateOutlines()
{
    std::unique_ptr<OutlineJob> done;
    { std::lock_guard lock(m_workMutex); done=std::move(m_outlineDone); }
    if (done && done->revision==m_outlineRevision.load()) {
        if (!done->failed && enabled() && m_placementOutlines.load())
            worldmesh::setDebugLines(worldmesh::LineSource::Schematica,std::move(done->lines));
        else if (done->failed && !m_workCancel.load())
            notice::failOnce("Schematica.outlines",L"Schematica: could not prepare placement outlines.","Schematica: could not prepare placement outlines.");
    }
    if (!m_outlinesDirty.exchange(false)) return;
    auto job=std::make_unique<OutlineJob>();
    job->revision=m_outlineRevision.fetch_add(1)+1;
    if (!enabled() || !m_placementOutlines.load()) {
        worldmesh::setDebugLines(worldmesh::LineSource::Schematica,{});return;
    }
    {
        std::lock_guard lock(m_filesMutex);
        for(const auto& p:m_placements) {
            if(!p->spec.enabled) continue;
            const auto copies=m_rangeCopies.find(p->spec.id);
            if(copies==m_rangeCopies.end() || copies->second.empty()) continue;
            for(const auto& f:m_files) if(f->name==p->spec.schematic) {
                OutlineJob::Entry entry;
                entry.box=schematic::placementBox(p->spec,f->sizeX.load(),f->sizeY.load(),f->sizeZ.load());
                entry.copies=copies->second;
                const bool selected=p->spec.id==m_selectedId.load();
                std::copy_n(schematic::kPlacementColors[p->spec.color],4,entry.color);
                if(selected) for(int a=0;a<3;++a) entry.color[a]=(entry.color[a]+1.0F)*0.5F;
                entry.width=selected?debuglines::kStrokeWidth:1.0F;
                job->entries.push_back(entry);break;
            }
        }
    }
    if (!ensureWorker()) return;
    { std::lock_guard lock(m_workMutex); m_outlineWork=std::move(job); }
    m_workCv.notify_one();
}

void Schematica::syncBoxSections()
{
    std::vector<schematic::SectionKey> dirty;
    if (!boxes::boxesOn()) {
        if (m_boxWasOn) boxes::pushSectionRequest({true, m_boxXray, {}, {}});
        m_boxSent.clear(); m_boxWasOn = false;
        schematicworld::takeBoxDirty(dirty);
        return;
    }
    float eye[3]{}, vp[16]{};
    if (!boxes::cameraSnapshot(eye, vp)) return;
    const int cx = static_cast<int>(std::floor(eye[0])) >> 4;
    const int cz = static_cast<int>(std::floor(eye[2])) >> 4;
    const auto snap = schematicworld::renderSnapshot();
    const bool xray = m_diffXray.load(std::memory_order_relaxed) || (snap && snap->verifierActive);
    const auto highlightGeneration = snap ? snap->highlightGeneration : 0;
    const auto generation = schematicworld::indexGeneration();
    boxes::SectionRequest request;
    request.xray = xray;
    request.reset = !m_boxWasOn || generation != m_boxIndexGeneration
        || m_layerGeneration != m_boxLayerGeneration || m_boxXray != xray || m_boxRadius != m_radiusChunks
        || highlightGeneration != m_boxHighlightGeneration;
    const bool moved = cx != m_boxCamX || cz != m_boxCamZ;
    auto inRange = [&](const schematic::SectionKey& key) {
        return std::abs(key.cx - cx) <= m_radiusChunks && std::abs(key.cz - cz) <= m_radiusChunks;
    };
    if (snap && (snap->layer.on != m_layerFilter.on || snap->layer.axis != m_layerFilter.axis
        || snap->layer.lo != m_layerFilter.lo || snap->layer.hi != m_layerFilter.hi)) return;
    if (!snap && !m_boxSent.empty()) request.reset = true;
    if (request.reset) m_boxSent.clear();
    if (request.reset || moved) {
        for (auto it = m_boxSent.begin(); it != m_boxSent.end();) {
            if (!inRange(it->first)) { request.remove.push_back(it->first); it = m_boxSent.erase(it); }
            else ++it;
        }
        if (snap) for (const auto& key : schematicworld::inRangeKeys()) {
            const auto it = snap->at.find(key);
            if (it == snap->at.end()) continue;
            const auto& view = snap->sections[it->second];
            if (view.diff && view.diff->anyColor && inRange(key) && !m_boxSent.contains(key)) {
                request.collect.push_back(key); m_boxSent[key] = true;
            }
        }
    }
    schematicworld::takeBoxDirty(dirty);
    for (const auto& key : dirty) {
        if (!inRange(key)) continue;
        const auto* view = [&]() -> const schematic::SectionView* {
            if (!snap) return nullptr;
            const auto it = snap->at.find(key);
            return it == snap->at.end() ? nullptr : &snap->sections[it->second];
        }();
        if (view && view->diff && view->diff->anyColor) {
            request.collect.push_back(key); m_boxSent[key] = true;
        } else if (m_boxSent.erase(key)) request.remove.push_back(key);
    }
    if (request.reset || !request.collect.empty() || !request.remove.empty()) boxes::pushSectionRequest(std::move(request));
    m_boxWasOn = true; m_boxXray = xray; m_boxRadius = m_radiusChunks;
    m_boxIndexGeneration = generation; m_boxLayerGeneration = m_layerGeneration;
    m_boxHighlightGeneration = highlightGeneration;
    m_boxCamX = cx; m_boxCamZ = cz;
}

selection::Area Schematica::areaSelection() const
{
    std::lock_guard lock(m_areaMutex);
    return m_area;
}

void Schematica::editSelection(const std::function<void(selection::Area&)>& change)
{
    std::lock_guard lock(m_areaMutex);
    change(m_area);
    m_areaDirty=true;
    m_areaDirtyAt=GetTickCount64();
    ++m_areaRevision;
}

void Schematica::selectionAction(int action)
{
    if (action>=0 && action<8) m_areaActions.fetch_or(1U<<action);
}

std::wstring Schematica::selectionFileName() const { std::lock_guard lock(m_areaMutex); return m_areaFileName; }
void Schematica::setSelectionFileName(std::wstring name) { std::lock_guard lock(m_areaMutex); m_areaFileName=std::move(name); }
std::string Schematica::selectionSaveStatus() const { std::lock_guard lock(m_areaMutex); return m_areaSaveStatus; }

bool Schematica::saveSelectionLocked(bool force)
{
    std::lock_guard lock(m_areaMutex);
    if (!m_areaDirty || m_worldKey.empty() || m_areaWriteBlocked) return false;
    const auto now=GetTickCount64();
    if (!force && (now-m_areaDirtyAt<1000 || now-m_areaSaveAttemptAt<1000)) return false;
    m_areaSaveAttemptAt=now;
    const auto dir=paths::schematicsDir().parent_path()/"selections";
    std::error_code ec;
    std::filesystem::create_directories(dir,ec);
    const auto path=dir/(m_worldKey+".json"), temp=dir/(m_worldKey+".json.tmp");
    bool ok=false;
    if (!ec) {
        std::ofstream out(temp,std::ios::binary|std::ios::trunc);
        if (out) { out<<selection::toJson(m_area); out.close(); ok=out.good() && MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING)!=FALSE; }
    }
    if (ok) m_areaDirty=false;
    else notice::failOnce("Schematica.selectionWrite",L"Schematica: cannot save the area selection; the original file was kept.","Could not save area selection.");
    return ok;
}

void Schematica::loadSelectionLocked(bool keepCurrent)
{
    std::lock_guard lock(m_areaMutex);
    m_areaWriteBlocked=false;
    if (!keepCurrent) { m_area={}; m_areaDirty=false; m_areaFileName=L"Unnamed"; m_areaDirtyAt=0; m_areaSaveAttemptAt=0; }
    ++m_areaRevision;
    if (m_worldKey.empty()) return;
    const auto path=paths::schematicsDir().parent_path()/"selections"/(m_worldKey+".json");
    std::error_code ec;
    if (!std::filesystem::exists(path,ec) && !ec) return;
    std::ifstream in(path,std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)),{});
    selection::Area loaded;
    if (!in.bad() && selection::fromJson(text,loaded)) {
        if (!keepCurrent || !m_areaDirty) { m_area=std::move(loaded); m_areaDirty=false; m_areaFileName=toUtf16(m_area.name); }
        return;
    }
    auto bad=path;
    bad+=L".bad-"+std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    std::filesystem::rename(path,bad,ec);
    m_areaWriteBlocked=static_cast<bool>(ec);
    notice::failOnce("Schematica.selectionRead",L"Schematica: cannot read the area selection JSON; the original file was moved aside.","Could not read area selection; the original file was preserved.");
}

namespace {
bool selectionHere(std::int32_t pos[3],bool feetOnly=false)
{
    unsigned char raw[hooks::kHitResultBytes]{};
    if (!feetOnly && hooks::readHitResult(raw)) {
        std::int32_t kind=0;
        std::memcpy(&kind,raw+0x18,4);
        if (kind==0) { std::memcpy(pos,raw+0x20,12); return true; }
    }
    return playerBlockPos(pos[0],pos[1],pos[2]);
}

class SelectionFileBuffer : public std::streambuf {
    HANDLE file;
    char buffer[65536]{};
    bool flushBuffer() {
        DWORD count=static_cast<DWORD>(pptr()-pbase()), written=0;
        if (count && (!WriteFile(file,pbase(),count,&written,nullptr) || written!=count)) return false;
        setp(buffer,buffer+sizeof(buffer)); return true;
    }
protected:
    int_type overflow(int_type ch) override {
        if (!flushBuffer()) return traits_type::eof();
        if (!traits_type::eq_int_type(ch,traits_type::eof())) { *pptr()=traits_type::to_char_type(ch); pbump(1); }
        return traits_type::not_eof(ch);
    }
    int sync() override { return flushBuffer()?0:-1; }
public:
    explicit SelectionFileBuffer(HANDLE handle):file(handle) { setp(buffer,buffer+sizeof(buffer)); }
};
}

void Schematica::runSelectionSave(SelectionSaveJob& job)
{
    try {
        if (job.allocate) {
            job.data.blocks.resize(job.data.volume(),1);
            job.data.blocks2.resize(job.data.volume(),0);
            job.ok=true;
            return;
        }
        HANDLE file=CreateFileW(job.path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file==INVALID_HANDLE_VALUE) { job.status="File already exists or could not be created"; return; }
        {
            SelectionFileBuffer buffer(file);
            std::ostream out(&buffer);
            job.ok=structure::write(out,job.data,job.origin,job.status);
            out.flush();
            job.ok=job.ok && out.good();
        }
        const bool closed=CloseHandle(file)!=FALSE;
        job.ok=job.ok && closed;
        if (!job.ok) {
            DeleteFileW(job.path.c_str());
            if (job.status.empty()) job.status="Could not finish writing schematic";
        } else job.status=std::format("Saved {} ({}x{}x{}, {} blocks). Blocks only.",toUtf8(job.path.stem().wstring()),
            job.data.sizeX,job.data.sizeY,job.data.sizeZ,job.area.blockCount());
    } catch (const std::exception& e) { job.ok=false; job.status=e.what(); }
}

void Schematica::startSelectionSave()
{
    if (m_saveBusy) return;
    auto job=std::make_unique<SelectionSaveJob>();
    std::wstring name;
    { std::lock_guard lock(m_areaMutex); job->area=m_area; name=m_areaFileName; }
    const auto fail=[&](const char* why) { std::lock_guard lock(m_areaMutex); m_areaSaveStatus=why; ++m_areaRevision; };
    if (!m_inWorld.load() || blockwrite::renderRegion()==nullptr) { fail("Not in a world"); return; }
    if (!job->area.boundsOf(job->lo,job->hi)) { fail("Set both corners of at least one box"); return; }
    if (name.empty() || name==L"." || name==L".." || name.back()==L'.' || name.back()==L' '
        || name.find_first_of(L"<>:\"/\\|?*")!=std::wstring::npos
        || std::any_of(name.begin(),name.end(),[](wchar_t ch) { return ch<32; })) { fail("Invalid file name"); return; }
    auto baseName=name.substr(0,name.find(L'.'));
    std::transform(baseName.begin(),baseName.end(),baseName.begin(),[](wchar_t ch) { return static_cast<wchar_t>(std::towupper(ch)); });
    if (baseName==L"CON" || baseName==L"PRN" || baseName==L"AUX" || baseName==L"NUL"
        || (baseName.size()==4 && (baseName.starts_with(L"COM") || baseName.starts_with(L"LPT")) && baseName[3]>=L'1' && baseName[3]<=L'9')) {
        fail("Invalid file name"); return;
    }
    const auto dir=paths::schematicsDir();
    if (dir.empty()) { fail("Schematic directory is unavailable"); return; }
    job->path=dir/(name+L".mcstructure");
    std::error_code ec;
    if (std::filesystem::exists(job->path,ec) || ec) { fail("File already exists or is unavailable"); return; }
    std::int32_t* dims[3]{&job->data.sizeX,&job->data.sizeY,&job->data.sizeZ};
    for (int a=0;a<3;++a) {
        const auto length=std::int64_t(job->hi[a])-job->lo[a]+1;
        if (length>INT32_MAX) { fail("Selection dimensions exceed the NBT format"); return; }
        *dims[a]=static_cast<std::int32_t>(length);
        job->origin[a]=job->area.originSet?job->area.origin[a]:job->lo[a];
        job->section[a]=job->sectionLo[a]=job->lo[a]>>4;
        job->sectionHi[a]=job->hi[a]>>4;
    }
    if (job->data.volume()>INT32_MAX) { fail("Selection volume exceeds the NBT format"); return; }
    job->sectionsTotal=1;
    for (int a=0;a<3;++a) job->sectionsTotal*=static_cast<std::size_t>(job->sectionHi[a]-job->sectionLo[a]+1);
    job->worldGeneration=blockwrite::regionGeneration();
    job->data.palette.push_back({"minecraft:structure_void",18168865,{}});
    job->data.palette.push_back({"minecraft:air",18168865,{}});
    blocks::Table table;
    if (!blocks::resolve({},table)) { fail("Block registry is unavailable"); return; }
    std::vector<const void*> known;
    for (const auto& [blockName,block] : table) { (void)blockName; known.push_back(block); blocks::appendStateVariants(block,known); }
    blockwrite::setKnownBlocks(known.data(),known.size());
    if (!ensureWorker()) { fail("Could not start schematic writer"); return; }
    m_saveBusy=true;
    fail("Preparing save... Blocks only.");
    static bool told=false;
    if (!told) { told=true; log().info(L"Schematica: saving blocks only; block entities and entities are omitted; palette version fallback = 18168865 (1.20.80)"); }
    { std::lock_guard lock(m_workMutex); m_saveWork=std::move(job); }
    m_workCv.notify_one();
}

void Schematica::stepSelection() try
{
    const unsigned actions=m_areaActions.exchange(0);
    if (m_inWorld.load()) for (int action=0;action<8;++action) if (actions&(1U<<action)) {
        if (action==5) { startSelectionSave(); continue; }
        std::int32_t pos[3]{};
        if ((action<3 || action>=6) && !selectionHere(pos,action>=6)) continue;
        editSelection([&](auto& area) {
            if (action==2) {
                selection::Box b; b.name="Box "+std::to_string(area.boxes.size()+1); b.has1=b.has2=true;
                std::copy_n(pos,3,b.p1); std::copy_n(pos,3,b.p2);
                area.boxes.push_back(std::move(b)); area.selected=static_cast<int>(area.boxes.size()-1);
            } else if (action==3) {
                if (!area.boxes.empty()) area.selected=(area.selected+1)%static_cast<int>(area.boxes.size());
            } else if (action==4) {
                if (area.selected>=0 && static_cast<std::size_t>(area.selected)<area.boxes.size()) {
                    area.boxes.erase(area.boxes.begin()+area.selected);
                    area.selected=area.boxes.empty()?-1:(std::min)(area.selected,static_cast<int>(area.boxes.size()-1));
                }
            } else {
                if (area.selected<0 || static_cast<std::size_t>(area.selected)>=area.boxes.size()) {
                    area.boxes.push_back({"Box "+std::to_string(area.boxes.size()+1)}); area.selected=static_cast<int>(area.boxes.size()-1);
                }
                auto& b=area.boxes[area.selected];
                const bool first=action==0 || action==6;
                std::copy_n(pos,3,first?b.p1:b.p2); (first?b.has1:b.has2)=true;
            }
        });
    }
    std::unique_ptr<SelectionSaveJob> done;
    { std::lock_guard lock(m_workMutex); done=std::move(m_saveDone); }
    if (done) {
        if (done->allocate && done->ok) { done->allocate=false; done->ok=false; m_saveRead=std::move(done); }
        else {
            m_saveBusy=false;
            { std::lock_guard lock(m_areaMutex); m_areaSaveStatus=done->status; ++m_areaRevision; }
            log().info(L"Schematica: {}",toUtf16(done->status));
            if (done->ok) refreshFiles();
            { std::lock_guard lock(m_workMutex); m_saveTrash.push_back(std::move(done)); }
            m_workCv.notify_one();
        }
    }
    if (m_saveRead) {
        auto& job=*m_saveRead;
        std::string error;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(4);
        void* region=blockwrite::renderRegion();
        if (!m_inWorld.load() || job.worldGeneration!=blockwrite::regionGeneration() || !region)
            error="World changed; save cancelled";
        const auto paletteIndex=[&](const void* block,std::uint16_t& index) {
            const auto found=job.palette.find(block);
            if (found!=job.palette.end()) { index=found->second; return true; }
            char name[256]{};
            structure::PaletteEntry p;
            p.version=18168865;
            if (!blocks::nameOfBlock(block,name,sizeof(name)) || !blocks::typedStatesOfBlock(block,p.states)) return false;
            p.name=name;
            if (job.data.palette.size()>=65534) return false;
            job.data.palette.push_back(std::move(p));
            index=static_cast<std::uint16_t>(job.data.palette.size()); job.palette.emplace(block,index); return true;
        };
        while (error.empty() && job.sectionsDone<job.sectionsTotal && std::chrono::steady_clock::now()<deadline) {
            const int base[3]{job.section[0]*16,job.section[1]*16,job.section[2]*16};
            void* sub=blockwrite::findSubChunk(region,base[0],base[1],base[2]);
            const bool air=!sub && blockwrite::lastFindWasAboveTop();
            if (sub) blockwrite::noteSubChunkAt(base[0],base[1],base[2],sub);
            else if (!air) { error="Some chunks are not loaded; move closer and try again"; break; }
            for (;job.cell<4096;++job.cell) {
                if ((job.cell&63)==0 && std::chrono::steady_clock::now()>=deadline) break;
                const int x=base[0]+static_cast<int>(job.cell>>8), y=base[1]+static_cast<int>((job.cell>>4)&15), z=base[2]+static_cast<int>(job.cell&15);
                if (x<job.lo[0] || x>job.hi[0] || y<job.lo[1] || y>job.hi[1] || z<job.lo[2] || z>job.hi[2] || !job.area.insideAnyBox(x,y,z)) continue;
                const auto at=job.data.indexOf(x-job.lo[0],y-job.lo[1],z-job.lo[2]);
                if (air) { job.data.blocks[at]=2; continue; }
                const unsigned be=blockwrite::beIndexOf(x,y,z);
                const void* block=blockwrite::readBlockOf(sub,0,be);
                const void* extra=nullptr;
                if (!block || !blockwrite::readExtraOf(sub,be,extra)) { error="Could not read all blocks; move closer and try again"; break; }
                if (!paletteIndex(block,job.data.blocks[at]) || (extra && !paletteIndex(extra,job.data.blocks2[at]))) {
                    error="Could not read typed block states or palette is full"; break;
                }
            }
            if (job.cell<4096) break;
            job.cell=0; ++job.sectionsDone;
            if (++job.section[2]>job.sectionHi[2]) { job.section[2]=job.sectionLo[2]; if (++job.section[1]>job.sectionHi[1]) { job.section[1]=job.sectionLo[1]; ++job.section[0]; } }
        }
        if (!error.empty()) {
            { std::lock_guard lock(m_areaMutex); m_areaSaveStatus=error; ++m_areaRevision; }
            log().warn(L"Schematica: {}",toUtf16(error));
            m_saveBusy=false;
            { std::lock_guard lock(m_workMutex); m_saveTrash.push_back(std::move(m_saveRead)); }
            m_workCv.notify_one();
        } else if (job.sectionsDone==job.sectionsTotal) {
            { std::lock_guard lock(m_areaMutex); m_areaSaveStatus="Writing schematic... Blocks only."; ++m_areaRevision; }
            { std::lock_guard lock(m_workMutex); m_saveWork=std::move(m_saveRead); }
            m_workCv.notify_one();
        } else {
            std::lock_guard lock(m_areaMutex);
            const auto status=std::format("Saving... {} / {} sections",job.sectionsDone,job.sectionsTotal);
            if (status!=m_areaSaveStatus) { m_areaSaveStatus=status; ++m_areaRevision; }
            const auto progress=static_cast<unsigned>(job.sectionsDone*16/job.sectionsTotal);
            if (progress>job.progressLogged) { job.progressLogged=progress; log().info(L"Schematica: {}",toUtf16(status)); }
        }
    }
    updateSelectionLines();
}
catch (const std::exception& e)
{
    { std::lock_guard lock(m_areaMutex); m_areaSaveStatus=std::string("Could not save selection: ")+e.what(); ++m_areaRevision; }
    log().warn(L"Schematica: selection save failed: {}",toUtf16(e.what()));
    if (m_saveRead) {
        std::lock_guard lock(m_workMutex);
        m_saveTrash.push_back(std::move(m_saveRead));
        m_workCv.notify_one();
        m_saveBusy=false;
    }
}

void Schematica::updateSelectionLines()
{
    const bool on=enabled() && m_inWorld.load();
    const auto revision=m_areaRevision.load();
    if (revision==m_areaLinesRevision && on==m_areaLinesEnabled) return;
    m_areaLinesRevision=revision; m_areaLinesEnabled=on;
    std::vector<worldmesh::DebugLine> lines;
    if (on) {
        const auto area=areaSelection();
        for (std::size_t i=0;i<area.boxes.size();++i) {
            const auto& b=area.boxes[i];
            const bool selected=static_cast<int>(i)==area.selected;
            if (b.has1 && b.has2) {
                float low[3]{},high[3]{};
                for (int a=0;a<3;++a) { low[a]=static_cast<float>((std::min)(b.p1[a],b.p2[a])); high[a]=static_cast<float>(double((std::max)(b.p1[a],b.p2[a]))+1); }
                const float color[4]{selected?1.0F:0.5F,selected?0.9F:0.5F,selected?0.3F:0.5F,1.0F};
                debuglines::boxLines(low,high,color,lines,selected?debuglines::kStrokeWidth:1.0F);
            }
            for (int corner=0;corner<2;++corner) if (corner==0?b.has1:b.has2) {
                const auto* pos=corner==0?b.p1:b.p2;
                float low[3]{},high[3]{};
                for (int a=0;a<3;++a) { low[a]=static_cast<float>(double(pos[a])+0.25); high[a]=static_cast<float>(double(pos[a])+0.75); }
                const float color[4]{corner==0?1.0F:0.2F,0.2F,corner==0?0.2F:1.0F,1.0F};
                debuglines::boxLines(low,high,color,lines,debuglines::kStrokeWidth);
            }
        }
    }
    worldmesh::setDebugLines(worldmesh::LineSource::AreaSelection,std::move(lines));
}

void Schematica::setPasteStatus(std::string status)
{
    std::lock_guard lock(m_pasteMutex);
    if (m_pasteStatus != status) { m_pasteStatus = std::move(status); ++m_pasteRevision; }
}

std::string Schematica::pasteStatus() const
{
    std::lock_guard lock(m_pasteMutex);
    return m_pasteStatus;
}

bool Schematica::pasteArmed(std::uint64_t placement) const
{
    std::lock_guard lock(m_pasteMutex);
    return placement != 0 && m_pasteArmedId == placement && m_pasteArmedAt != 0
        && GetTickCount64() - m_pasteArmedAt < kDeleteArmMs;
}

void Schematica::pressPaste(std::uint64_t placement)
{
    if (!placement || m_pasteBusy.load() || m_shuttingDown.load()) return;
    const auto now = GetTickCount64();
    std::lock_guard lock(m_pasteMutex);
    if (now < m_pasteRestAt || m_pasteRequest.load() != 0) return;
    if (m_pasteArmedId == placement && m_pasteArmedAt != 0 && now - m_pasteArmedAt < kDeleteArmMs) {
        if (now - m_pasteArmedAt < kDeleteMinMs) return;
        m_pasteRequest.store(placement);
        m_pasteArmedId = 0; m_pasteArmedAt = 0;
        m_pasteRestAt = now + kDeleteRestMs;
    } else {
        m_pasteArmedId = placement; m_pasteArmedAt = now;
    }
    ++m_pasteRevision;
}

void Schematica::stopPaste()
{
    m_pasteRequest.store(0); m_pasteStop.store(true); m_pasteCancel.store(true);
    std::lock_guard lock(m_pasteMutex);
    if (m_pasteArmedId != 0) { m_pasteArmedId = 0; m_pasteArmedAt = 0; ++m_pasteRevision; }
}

void Schematica::startPaste(std::uint64_t placement)
{
    const auto fail = [this](const char* why) {
        setPasteStatus(why); chatcommand::postNotice(why); log().warn(L"Schematica: {}", toUtf16(why));
    };
    if (m_shuttingDown.load() || m_pasteBusy.load()) return;
    if (!enabled() || !m_inWorld.load() || !blockwrite::renderRegion()) { fail("Paste requires an enabled placement in a world"); return; }
    if (GameModeState::instance().currentMode() != gamemode::kCreative) { fail("Paste requires creative mode"); return; }
    if (!gamecommand::available()) { fail("Paste: the chat command entry is unavailable"); return; }
    auto job = std::make_unique<PasteJob>();
    if (!placementSpec(placement, job->spec)) { fail("Paste: select a placement first"); return; }
    job->snapshot = schematicworld::renderSnapshot();
    if (m_prepBusy || m_prepWant != 0 || m_reloadPending.load() || m_posChangedAt.load() != 0 || m_paletteStale
        || !m_paletteBlocks || !m_air) {
        fail("Paste: wait for the schematic to finish loading"); return;
    }
    {
        std::lock_guard lock(m_filesMutex);
        const auto base = m_materialBases.find(placement);
        const auto file = std::find_if(m_files.begin(), m_files.end(), [&](const auto& f) { return f->name == job->spec.schematic; });
        if (base != m_materialBases.end() && file != m_files.end() && (*file)->loaded) {
            job->source.data = (*file)->loaded; job->source.paletteBase = base->second;
        }
    }
    if (!job->source.data) { fail("Paste: load the selected schematic first"); return; }
    job->source.placementId = placement;
    job->source.rotation = job->spec.rotation; job->source.mirror = static_cast<int>(job->spec.mirror);
    if (!job->snapshot || job->snapshot->palette != m_paletteBlocks) {
        auto snapshot = std::make_shared<schematic::RenderSnapshot>();
        snapshot->palette = m_paletteBlocks; snapshot->air = m_air;
        job->snapshot = std::move(snapshot);
    }
    job->layer = m_layerFilter;
    job->replace = static_cast<paste::Replace>(m_pasteReplace.load());
    job->visibleOnly = m_pasteVisibleOnly.load(); job->mute = m_pasteMute.load();
    job->worldGeneration = blockwrite::regionGeneration(); job->worldEntry = worldidentity::snapshot().entry;
    if (!ensureWorker()) { fail("Paste: could not start background preparation"); return; }
    m_pasteCancel.store(false); m_pasteStop.store(false); m_pasteBusy.store(true);
    setPasteStatus("Preparing paste... Blocks only; layer 1 (waterlogging) and block entity contents are omitted.");
    { std::lock_guard lock(m_workMutex); m_pasteWork = std::move(job); }
    m_workCv.notify_one();
}

void Schematica::preparePaste(PasteJob& job) try
{
    if (job.cells) {
        if (!m_pasteCancel.load()) job.commands = paste::commands(std::move(job.runs));
        job.sending = true;
        return;
    }
    const auto& data = *job.source.data;
    auto p = job.source;
    p.x=job.spec.x; p.y=job.spec.y; p.z=job.spec.z; p.owner=0;
    if (job.spec.grid.enabled) p.copies=schematic::copiesInBox(job.spec.grid,
        schematic::placementBox(job.spec,data.sizeX,data.sizeY,data.sizeZ),{p.x,p.y,p.z},
        {kMinXZ,kMinY,kMinXZ,kMaxXZ+1,kMaxY+1,kMaxXZ+1});
    const std::vector<schematic::Placement> placements{std::move(p)};
    job.cells = std::make_shared<schematic::CellSet>();
    if (!schematic::buildCells(placements, *job.cells, &m_pasteCancel))
        job.error = job.cells->why ? job.cells->why : "Paste preparation cancelled";
} catch (const std::exception&) {
    job.error = "Paste: could not allocate the placement copies";
}

void Schematica::finishPaste(const std::string& reason)
{
    if (!m_pasteJob) return;
    auto& job = *m_pasteJob;
    std::string status = reason;
    const bool sameWorld = job.worldGeneration == blockwrite::regionGeneration()
        && job.worldEntry == worldidentity::snapshot().entry;
    if (job.muted) {
        bool handled = false;
        if (!sameWorld || !blockwrite::renderRegion() || !gamecommand::run("/gamerule sendcommandfeedback true", handled))
            status += "; command feedback could not be restored in the original world";
    }
    if (reason.empty()) status = std::format("Pasted {} commands ({} blocks)", job.sent, job.blocks) + status;
    else status += std::format(" ({} commands, {} blocks sent)", job.sent, job.blocks);
    if (job.missing != 0) status += std::format("; {} sections are not loaded; move closer and paste again", job.missing);
    if (job.water != 0) status += std::format("; omitted layer 1 in {} blocks", job.water);
    setPasteStatus(status);
    chatcommand::postNotice(status);
    log().info(L"Schematica: {}", toUtf16(status));
    if (job.sent != 0) log().info(L"Schematica: command replies are unavailable; counts are commands/blocks sent; verify the result with Verifier (cheats/OP required)");
    postTrash(std::move(job.cells));
    m_pasteJob.reset();
    m_pasteBusy.store(false); m_pasteStop.store(false);
    { std::lock_guard lock(m_pasteMutex); m_pasteRestAt = GetTickCount64() + kDeleteRestMs; }
    m_verifierRefresh.store(true);
}

void Schematica::stepPaste() try
{
    if (!m_pasteJob) {
        std::lock_guard lock(m_workMutex);
        m_pasteJob = std::move(m_pasteDone);
        if (m_pasteJob && m_pasteJob->sending)
            log().info(L"Schematica: paste prepared {} commands; layer 1 and block entity contents are omitted", m_pasteJob->commands.size());
    }
    const auto request = m_pasteRequest.exchange(0);
    if (request != 0 && !m_pasteBusy.load()) startPaste(request);
    if (!m_pasteJob) return;
    auto& job = *m_pasteJob;
    if (m_shuttingDown.load() || m_pasteStop.load() || m_pasteCancel.load() || !enabled()) { finishPaste("Paste stopped"); return; }
    void* region = blockwrite::renderRegion();
    if (!m_inWorld.load() || job.worldGeneration != blockwrite::regionGeneration()
        || job.worldEntry != worldidentity::snapshot().entry) { finishPaste("World changed; paste stopped"); return; }
    if (!region) { finishPaste("World is unavailable; paste stopped"); return; }
    if (GameModeState::instance().currentMode() != gamemode::kCreative) { finishPaste("Creative mode ended; paste stopped"); return; }
    if (!job.error.empty()) { const auto error = job.error; finishPaste(error); return; }
    if (!job.cells) { finishPaste("Paste preparation failed"); return; }
    if (!job.sending) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4);
        while (job.section < job.cells->chunks.size() && std::chrono::steady_clock::now() < deadline) {
            const auto base = job.cells->chunks[job.section++];
            std::vector<schematic::Cell> cells;
            job.cells->forEachInChunk(base.x >> 4, base.y >> 4, base.z >> 4, [&](std::size_t at) {
                schematic::Cell cell;
                if (job.cells->cellAt(at, cell) && (!job.visibleOnly || job.layer.shows(cell.x, cell.y, cell.z))) cells.push_back(cell);
            });
            if (cells.empty()) continue;
            void* sub = blockwrite::findSubChunk(region, base.x, base.y, base.z);
            const bool empty = !sub && blockwrite::lastFindWasAboveTop();
            if (!sub && !empty) { ++job.missing; continue; }
            if (sub) blockwrite::noteSubChunkAt(base.x, base.y, base.z, sub);
            std::vector<paste::Cell> send;
            bool unread = false;
            for (const auto& cell : cells) {
                const void* want = job.snapshot->air;
                if (cell.entry != schematic::kAirCell) {
                    if (cell.entry < job.snapshot->palette->size()) want = (*job.snapshot->palette)[cell.entry];
                    else want = nullptr;
                    if (!want) {
                        finishPaste("Paste: a transformed palette entry is unresolved"); return;
                    }
                }
                const void* real = empty ? job.snapshot->air : blockwrite::readBlockOf(sub, 0, blockwrite::beIndexOf(cell.x, cell.y, cell.z));
                if (!real) { unread = true; break; }
                if (cell.entry2 >= 0) ++job.water;
                const bool wantAir = want == job.snapshot->air, realAir = real == job.snapshot->air;
                if (!paste::shouldSend(job.replace, wantAir, realAir, want == real)) continue;
                auto [arg, added] = job.arguments.try_emplace(want);
                if (added) {
                    char name[512]{};
                    std::vector<structure::StateValue> states;
                    if (!blocks::nameOfBlock(want, name, sizeof(name)) || !blocks::typedStatesOfBlock(want, states)
                        || !paste::blockArgument(name, states, arg->second)) {
                        finishPaste("Paste: could not read typed block states"); return;
                    }
                }
                send.push_back({cell.x, cell.y, cell.z, arg->second});
            }
            if (unread) { ++job.missing; continue; }
            auto runs = paste::runs(std::move(send));
            for (auto& run : runs) job.runs.push_back(std::move(run));
        }
        if (job.section < job.cells->chunks.size()) {
            setPasteStatus(std::format("Preparing paste... {} / {} sections", job.section, job.cells->chunks.size())); return;
        }
        setPasteStatus("Preparing paste commands... Layer 1 and block entity contents are omitted.");
        { std::lock_guard lock(m_workMutex); m_pasteWork = std::move(m_pasteJob); }
        m_workCv.notify_one();
        return;
    }
    if (job.sent == job.commands.size()) { finishPaste({}); return; }
    if (const auto now = GetTickCount64(); job.sentAt != 0 && now - job.sentAt < 50) return;
    else job.sentAt = now;
    int budget = std::clamp(m_pastePerTick.load(), 1, 64);
    if (job.mute && !job.muted) {
        bool handled = false;
        if (!gamecommand::run("/gamerule sendcommandfeedback false", handled)) { finishPaste("Paste: could not mute command feedback"); return; }
        job.muted = true; --budget;
    }
    while (budget-- > 0 && job.sent < job.commands.size()) {
        if (m_pasteStop.load() || m_shuttingDown.load()) { finishPaste("Paste stopped"); return; }
        if (job.worldGeneration != blockwrite::regionGeneration() || job.worldEntry != worldidentity::snapshot().entry) {
            finishPaste("World changed; paste stopped"); return;
        }
        bool handled = false;
        const auto& command = job.commands[job.sent];
        const auto end = std::int64_t(command.x) + static_cast<std::int64_t>(command.blocks) - 1;
        for (auto x = std::int64_t((command.x >> 4) * 16); x <= end; x += 16) {
            void* sub = blockwrite::findSubChunk(region, static_cast<int>(x), command.y, command.z);
            if (!sub && !blockwrite::lastFindWasAboveTop()) {
                ++job.missing; finishPaste("A section became unavailable; paste stopped"); return;
            }
        }
        if (!gamecommand::run(command.text, handled)) { finishPaste("Paste: chat command execution failed"); return; }
        ++job.sent; job.blocks += command.blocks;
    }
    const auto grouped = [](std::size_t value) {
        auto text = std::to_string(value);
        for (auto at = static_cast<std::ptrdiff_t>(text.size()) - 3; at > 0; at -= 3) text.insert(static_cast<std::size_t>(at), 1, ',');
        return text;
    };
    setPasteStatus("Pasting... " + grouped(job.sent) + " / " + grouped(job.commands.size()) + " commands");
} catch (const std::exception&) {
    finishPaste("Paste: could not allocate command data");
}

MenuItem Schematica::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());

    {
        MenuItem open = menu::action(L"Settings page", [] {});
        open.opensPage = true;
        open.hidden = writes::blocked("Schematica:page");
        open.value = [] { return std::wstring(L"Open"); };
        children.push_back(std::move(open));
    }

    {
        MenuItem pageKey = menu::keybind(
                L"Page key", [this] { std::lock_guard<std::mutex> guard(m_keysMutex); return m_pageKey.combo(); },
                [this](std::vector<int> combo) {
                    std::lock_guard<std::mutex> guard(m_keysMutex);
                m_pageKey.set(std::move(combo));
                log().info(L"Schematica: page key set to {}", m_pageKey.name());
            });
        bindPad(pageKey, m_pageKey);
        pageKey.hidden = writes::blocked("Schematica:page");
        children.push_back(std::move(pageKey));
    }

    children.push_back(menu::keybind(
        L"Layer up key", [this] { std::lock_guard<std::mutex> guard(m_keysMutex); return m_layerUpKey.combo(); },
        [this](std::vector<int> combo) {
            std::lock_guard<std::mutex> guard(m_keysMutex);
            m_layerUpKey.set(std::move(combo));
            log().info(L"Schematica: layer up key set to {}", m_layerUpKey.name());
        },
        {kLayerUpDefaultKey}));
    bindPad(children.back(), m_layerUpKey);
    children.push_back(menu::keybind(
        L"Layer down key", [this] { std::lock_guard<std::mutex> guard(m_keysMutex); return m_layerDownKey.combo(); },
        [this](std::vector<int> combo) {
            std::lock_guard<std::mutex> guard(m_keysMutex);
            m_layerDownKey.set(std::move(combo));
            log().info(L"Schematica: layer down key set to {}", m_layerDownKey.name());
        },
        {kLayerDownDefaultKey}));
    bindPad(children.back(), m_layerDownKey);
    const wchar_t* placeKeyLabels[2]{L"Easy Place toggle", L"Pick block"};
    for (int i = 0; i < 2; ++i) {
        auto key = menu::keybind(placeKeyLabels[i], [this,i] {
            std::lock_guard lock(m_keysMutex); return (i == 0 ? m_easyToggleKey : m_pickBlockKey).combo();
        }, [this,i](std::vector<int> combo) {
            std::lock_guard lock(m_keysMutex); (i == 0 ? m_easyToggleKey : m_pickBlockKey).set(std::move(combo));
        });
        bindPad(key, i == 0 ? m_easyToggleKey : m_pickBlockKey);
        children.push_back(std::move(key));
        auto pageKey = menu::text(placeKeyLabels[i], [this,i] {
            std::lock_guard lock(m_keysMutex); return keys::comboName((i == 0 ? m_easyToggleKey : m_pickBlockKey).combo());
        }, [this,i](std::wstring text) {
            std::vector<int> combo;
            if (keys::parseCombo(text, combo)) {
                std::lock_guard lock(m_keysMutex); (i == 0 ? m_easyToggleKey : m_pickBlockKey).set(std::move(combo));
            }
        });
        pageKey.textInput = TextInputKind::FreeText;
        pageKey.onPage = true; pageKey.pageTab = 3; pageKey.textOnCommit = true;
        children.push_back(std::move(pageKey));
    }
    const wchar_t* keyLabels[6]={L"Nudge + key",L"Nudge - key",L"Rotate key",L"Mirror key",L"Move to player key",L"Select next key"};
    for (int i=0;i<6;++i) {
        auto key=menu::keybind(keyLabels[i],[this,i] { return m_placementKeys[i].combo(); },
            [this,i](std::vector<int> combo) { m_placementKeys[i].set(std::move(combo)); });
        bindPad(key,m_placementKeys[i]);
        children.push_back(std::move(key));
        auto pageKey=menu::text(keyLabels[i],[this,i] { return keys::comboName(m_placementKeys[i].combo()); },
            [this,i](std::wstring text) { std::vector<int> combo; if(keys::parseCombo(text,combo)) m_placementKeys[i].set(std::move(combo)); });
        pageKey.textInput=TextInputKind::FreeText;
        pageKey.onPage=true; pageKey.pageTab=3; children.push_back(std::move(pageKey));
    }
    auto add=[&](MenuItem item,int tab) { item.onPage=true; item.pageTab=tab; children.push_back(std::move(item)); };
    std::vector<MenuItem> pageKeys;
    for (auto i=children.begin();i!=children.end();) {
        if (i->onPage && i->pageTab==3) { i->pageTab=0;pageKeys.push_back(std::move(*i)); i=children.erase(i); }
        else ++i;
    }
    add(menu::action(L"Import schematic",[this] { startImport(); }),0);
    add(menu::number(L"Opacity",[this] { return static_cast<float>(m_alpha.load()); },
        [this](float v) { m_alpha.store(std::clamp(static_cast<int>(v),kMinAlpha,kMaxAlpha)); m_redrawPending.store(true); },true,kMinAlpha,kMaxAlpha),0);
    add(menu::number(L"Box opacity",[this] { return static_cast<float>(m_boxAlpha.load()); },
        [this](float v) { m_boxAlpha.store(std::clamp(static_cast<int>(v),kMinAlpha,kMaxAlpha)); },true,kMinAlpha,kMaxAlpha),0);
    add(menu::toggle(L"Diff boxes",[this] { return m_diffBoxes.load(); },[this] { m_diffBoxes.store(!m_diffBoxes.load()); }),0);
    add(menu::toggle(L"See through",[this] { return m_diffXray.load(); },[this] { m_diffXray.store(!m_diffXray.load()); }),0);
    add(menu::toggle(L"Ghost over wrong blocks",[this] { return m_ghostOverMismatch.load(); },[this] { m_ghostOverMismatch.store(!m_ghostOverMismatch.load()); }),0);
    add(menu::toggle(L"Placement outlines",[this] { return m_placementOutlines.load(); },[this] { m_placementOutlines.store(!m_placementOutlines.load()); m_outlinesDirty.store(true); }),0);
    add(menu::toggle(L"Easy Place", [this] { return m_easyPlace.load(); }, [this] {
        m_easyPlace.store(!m_easyPlace.load()); if (!m_easyPlace.load()) lookplace::cancel();
    }), 0);
    add(menu::toggle(L"Easy Place: rotate", [this] { return m_easyPlaceRotate.load(); },
        [this] { m_easyPlaceRotate.store(!m_easyPlaceRotate.load()); }), 0);
    {
        auto autoSneak = menu::toggle(L"Easy Place: auto sneak", [this] { return m_easyAutoSneak.load(); },
            [this] { m_easyAutoSneak.store(!m_easyAutoSneak.load()); });
        autoSneak.hidden = writes::blocked("Schematica:autosneak");
        add(std::move(autoSneak), 0);
    }
    add(menu::toggle(L"Easy Place: hold", [this] { return m_easyPlaceHold.load(); },
        [this] { m_easyPlaceHold.store(!m_easyPlaceHold.load()); }), 0);
    auto pickSlots = menu::text(L"Easy Place: pick slots", [this] { return toUtf16(easyplace::slotMaskText(m_pickSlots.load())); },
        [this](std::wstring text) {
            unsigned mask = 0;
            if (easyplace::parseSlotMask(toUtf8(text), mask)) m_pickSlots.store(mask);
        });
    pickSlots.textInput = TextInputKind::FreeText;
    pickSlots.textOnCommit = true;
    add(std::move(pickSlots), 0);
    add(menu::toggle(L"Placement restriction", [this] { return m_placementRestriction.load(); },
        [this] { m_placementRestriction.store(!m_placementRestriction.load()); }), 0);
    for (auto& key : pageKeys) children.push_back(std::move(key));
    const wchar_t* areaKeyLabels[4]{L"Corner 1 here",L"Corner 2 here",L"Add box",L"Next box"};
    for (int i=0;i<4;++i) {
        auto key=menu::keybind(areaKeyLabels[i],[this,i] { std::lock_guard lock(m_keysMutex); return m_areaKeys[i].combo(); },
            [this,i](std::vector<int> combo) { std::lock_guard lock(m_keysMutex); m_areaKeys[i].set(std::move(combo)); });
        bindPad(key,m_areaKeys[i]); children.push_back(std::move(key));
        auto pageKey=menu::text(areaKeyLabels[i],[this,i] { std::lock_guard lock(m_keysMutex); return keys::comboName(m_areaKeys[i].combo()); },
            [this,i](std::wstring text) { std::vector<int> combo; if (keys::parseCombo(text,combo)) { std::lock_guard lock(m_keysMutex); m_areaKeys[i].set(std::move(combo)); } });
        pageKey.textInput=TextInputKind::FreeText; pageKey.textOnCommit=true; add(std::move(pageKey),0);
    }
    add(menu::text(L"Area",[this] { return toUtf16(areaSelection().name); },[this](std::wstring text) {
        editSelection([&](auto& area) { area.name=toUtf8(text); });
    },TextInputKind::FreeText),3);
    const wchar_t* cornerLabels[6]{L"X",L"Y",L"Z",L"X",L"Y",L"Z"};
    for (int i=0;i<6;++i) add(menu::text(cornerLabels[i],[this,i] {
        const auto area=areaSelection();
        if (area.selected<0 || static_cast<std::size_t>(area.selected)>=area.boxes.size()) return std::wstring{};
        const auto& b=area.boxes[area.selected];
        if (i<3?!b.has1:!b.has2) return std::wstring{};
        return std::to_wstring((i<3?b.p1:b.p2)[i%3]);
    },[this,i](std::wstring text) {
        int value=0; if (!parseTypedInt(text,value)) return;
        editSelection([&](auto& area) {
            if (area.selected<0 || static_cast<std::size_t>(area.selected)>=area.boxes.size()) return;
            auto& b=area.boxes[area.selected];
            (i<3?b.p1:b.p2)[i%3]=value; (i<3?b.has1:b.has2)=true;
        });
    }),3);
    add(menu::toggle(L"Origin: auto",[this] { return !areaSelection().originSet; },[this] {
        editSelection([](auto& area) {
            if (!area.originSet) { std::int32_t high[3]{}; area.boundsOf(area.origin,high); }
            area.originSet=!area.originSet;
        });
    }),3);
    const wchar_t* originLabels[3]{L"Origin X",L"Origin Y",L"Origin Z"};
    for (int a=0;a<3;++a) add(menu::text(originLabels[a],[this,a] { return std::to_wstring(areaSelection().origin[a]); },
        [this,a](std::wstring text) { int value=0; if (parseTypedInt(text,value)) editSelection([&](auto& area) { area.origin[a]=value; area.originSet=true; }); }),3);
    add(menu::text(L"File name",[this] { return selectionFileName(); },[this](std::wstring text) { setSelectionFileName(std::move(text)); },TextInputKind::FreeText),3);
    add(menu::action(L"Save schematic",[this] { selectionAction(5); }),3);
    const auto numberRow=[&](const wchar_t* label,std::atomic<int>& value,int low,int high,const std::function<void()>& after) {
        add(menu::text(label,[&value] { return std::format(L"{}",value.load()); },
            [&value,low,high,after](std::wstring text) { int v=0; if (parseTypedInt(text,v)) { value.store(std::clamp(v,low,high)); if (after) after(); } }),0);
    };
    const auto selected=[this] { schematic::PlacementSpec p; placementSpec(selectedPlacement(),p); return p; };
    add(menu::text(L"Name",[selected] { return selected().name; },[this](std::wstring name) { setPlacementName(selectedPlacement(),std::move(name)); },TextInputKind::FreeText),1);
    add(menu::toggle(L"Enabled",[selected] { return selected().id!=0 && selected().enabled; },[this,selected] { setPlacementEnabled(selectedPlacement(),!selected().enabled); }),1);
    add(menu::toggle(L"Render",[selected] { return selected().id!=0 && selected().render; },[this,selected] { setPlacementRender(selectedPlacement(),!selected().render); }),1);
    add(menu::toggle(L"Locked",[selected] { return selected().locked; },[this,selected] { setPlacementLocked(selectedPlacement(),!selected().locked); }),1);
    const wchar_t* axes[3]={L"Origin X",L"Origin Y",L"Origin Z"};
    for (int a=0;a<3;++a) add(menu::text(axes[a],[selected,a] { const auto p=selected(); return std::format(L"{}",a==0?p.x:a==1?p.y:p.z); },
        [this,a](std::wstring text) { int v=0; if (parseTypedInt(text,v)) setPlacementPos(selectedPlacement(),a,v); }),1);
    add(menu::action(L"Set to player",[this] { movePlacementToPlayer(selectedPlacement()); }),1);
    add(menu::cycle(L"Rotation",[selected] { return std::format(L"{} degrees",selected().rotation*90); },[this,selected] { setPlacementRotation(selectedPlacement(),selected().rotation+1); }),1);
    add(menu::cycle(L"Mirror",[selected] { const wchar_t* names[3]={L"None",L"X",L"Z"}; return std::wstring(names[static_cast<int>(selected().mirror)]); },
        [this,selected] { setPlacementMirror(selectedPlacement(),static_cast<schematic::Mirror>((static_cast<int>(selected().mirror)+1)%3)); }),1);
    add(menu::cycle(L"Color",[selected] { return std::format(L"{}",selected().color+1); },[this] { cyclePlacementColor(selectedPlacement()); }),1);
    add(menu::toggle(L"Grid: Enabled",[selected] { return selected().grid.enabled; },[this,selected] { auto grid=selected().grid;grid.enabled=!grid.enabled;setPlacementGrid(selectedPlacement(),grid); }),1);
    const wchar_t* gridLabels[9]={L"Grid size X",L"Grid size Y",L"Grid size Z",L"Grid repeat -X",L"Grid repeat -Y",L"Grid repeat -Z",L"Grid repeat +X",L"Grid repeat +Y",L"Grid repeat +Z"};
    for (int i=0;i<9;++i) add(menu::text(gridLabels[i],[selected,i] { auto g=selected().grid;return std::format(L"{}",i<3?g.size[i]:i<6?g.repeatNeg[i-3]:g.repeatPos[i-6]); },
        [this,selected,i](std::wstring text) { int v=0;if (!parseTypedInt(text,v)||v<0)return;auto g=selected().grid;if(i<3)g.size[i]=v;else if(i<6)g.repeatNeg[i-3]=v;else g.repeatPos[i-6]=v;setPlacementGrid(selectedPlacement(),g); }),1);
    add(menu::action(L"Duplicate",[this] { duplicatePlacement(selectedPlacement()); }),1);
    add(menu::action(L"Remove",[this] { removePlacement(selectedPlacement()); }),1);
    add(menu::choice(L"Paste: replace", {L"None", L"All", L"Non-air"}, [this] { return m_pasteReplace.load(); },
        [this](int value) { m_pasteReplace.store(std::clamp(value, 0, 2)); }),1);
    add(menu::text(L"Paste: commands per tick", [this] { return std::to_wstring(m_pastePerTick.load()); },
        [this](std::wstring text) { int value = 0; if (parseTypedInt(text, value)) m_pastePerTick.store(std::clamp(value, 1, 64)); }),1);
    add(menu::toggle(L"Paste: mute command feedback", [this] { return m_pasteMute.load(); },
        [this] { m_pasteMute.store(!m_pasteMute.load()); }),1);
    add(menu::toggle(L"Paste: only visible layers", [this] { return m_pasteVisibleOnly.load(); },
        [this] { m_pasteVisibleOnly.store(!m_pasteVisibleOnly.load()); }),1);

    {
        MenuItem item = menu::cycle(
            L"Layer mode",
            [this] {
                static constexpr const wchar_t* kNames[kLayerModeCount] = {
                    L"All", L"Single layer", L"All below", L"All above", L"Layer range"};
                return std::wstring(
                    kNames[std::clamp(m_layerMode.load(std::memory_order_relaxed), 0,
                                      kLayerModeCount - 1)]);
            },
            [this] {
                const int next = (m_layerMode.load(std::memory_order_relaxed) + 1) % kLayerModeCount;
                if (next == kLayerRange && m_layerMin.load() == 0 && m_layerMax.load() == 0) {
                    m_layerMin.store(m_layerValue.load());
                    m_layerMax.store(m_layerValue.load());
                }
                m_layerMode.store(next, std::memory_order_relaxed);
            });
        item.onPage = true;
        item.pageTab = 2;
        children.push_back(std::move(item));
    }
    {
        MenuItem item = menu::cycle(
            L"Axis",
            [this] {
                const int axis = m_layerAxis.load(std::memory_order_relaxed);
                return std::wstring(axis == 0 ? L"X" : (axis == 2 ? L"Z" : L"Y"));
            },
            [this] {
                const int next = (m_layerAxis.load(std::memory_order_relaxed) + 1) % 3;
                m_layerAxis.store(next, std::memory_order_relaxed);
                m_layerValue.store(clampLayerValue(next, m_layerValue.load()));
                m_layerMin.store(clampLayerValue(next, m_layerMin.load()));
                m_layerMax.store(clampLayerValue(next, m_layerMax.load()));
            });
        item.onPage = true;
        item.pageTab = 2;
        children.push_back(std::move(item));
    }
    const auto layerNumberRow = [&numberRow, &children, this](
                                    const wchar_t* label, std::atomic<int>& value) {
        numberRow(label, value, kMinXZ, kMaxXZ, [this, &value] {
            value.store(clampLayerValue(m_layerAxis.load(std::memory_order_relaxed), value.load()));
            m_layerTypedAt.store(GetTickCount64(), std::memory_order_relaxed);
        });
        children.back().pageTab = 2;
    };
    layerNumberRow(L"Layer", m_layerValue);
    layerNumberRow(L"Range min", m_layerMin);
    layerNumberRow(L"Range max", m_layerMax);
    {
        MenuItem item = menu::action(L"Set to player", [this] { setLayerHere(); });
        item.commandName = "layer_set_to_player";
        item.onPage = true;
        item.pageTab = 2;
        children.push_back(std::move(item));
    }
    const auto withKey = [](const wchar_t* step, const Hotkey& key) {
        const std::wstring name = key.name();
        return name.empty() ? std::wstring(step) : std::wstring(step) + L" (" + name + L")";
    };
    {
        MenuItem item = menu::action(L"Move up", [this] { stepLayer(1); });
        item.value = [this, withKey] { std::lock_guard<std::mutex> guard(m_keysMutex); return withKey(L"+1", m_layerUpKey); };
        item.onPage = true;
        item.pageTab = 2;
        children.push_back(std::move(item));
    }
    {
        MenuItem item = menu::action(L"Move down", [this] { stepLayer(-1); });
        item.value = [this, withKey] { std::lock_guard<std::mutex> guard(m_keysMutex); return withKey(L"-1", m_layerDownKey); };
        item.onPage = true;
        item.pageTab = 2;
        children.push_back(std::move(item));
    }

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void Schematica::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    m_pasteReplace.store(std::clamp(Config::getInt(section, "pasteReplace", 0), 0, 2));
    m_pastePerTick.store(std::clamp(Config::getInt(section, "pasteCommandsPerTick", 8), 1, 64));
    m_pasteMute.store(Config::getBool(section, "pasteMuteCommandFeedback", true));
    m_pasteVisibleOnly.store(Config::getBool(section, "pasteOnlyVisibleLayers", false));

    m_materialListType.store(std::clamp(Config::getInt(section, "materialListType", kMaterialAll), 0, kMaterialListTypeCount - 1));
    m_materialAll.store(Config::getBool(section, "materialAllPlacements", false));
    m_materialMultiplier.store(std::clamp(Config::getInt(section, "materialMultiplier", 1), 1, 999));
    m_materialHideAvailable.store(Config::getBool(section, "materialHideAvailable", false));
    m_materialSort.store(std::clamp(Config::getInt(section, "materialSort", 0), 0, 3));
    m_materialSortDescending.store(Config::getBool(section, "materialSortDescending", false));
    MaterialCollector::instance().loadConfig(section);
    {
        std::lock_guard guard(m_filesMutex);
        m_ignoredMaterials.clear();
        if (const auto at = section.find("materialIgnored"); at != section.end() && at->is_array())
            for (const auto& name : *at) if (name.is_string()) m_ignoredMaterials.insert(name.get<std::string>());
    }
    m_alpha.store(std::clamp(Config::getInt(section, "opacity", 45), kMinAlpha, kMaxAlpha));
    m_easyPlace.store(Config::getBool(section, "easyPlace", false));
    m_easyPlaceRotate.store(Config::getBool(section, "easyPlaceRotate", true));
    m_easyAutoSneak.store(Config::getBool(section, "easyPlaceAutoSneak", true));
    m_easyPlaceHold.store(Config::getBool(section, "easyPlaceHold", true));
    m_placementRestriction.store(Config::getBool(section, "placementRestriction", false));
    {
        unsigned mask = easyplace::kAllHotbarSlots;
        if (const auto at = section.find("easyPlacePickSlots"); at != section.end() && at->is_string()
            && !easyplace::parseSlotMask(at->get<std::string>(), mask)) mask = easyplace::kAllHotbarSlots;
        m_pickSlots.store(mask);
    }
    {
        std::lock_guard<std::mutex> keysGuard(m_keysMutex);
        if (const auto at = section.find("pageKeys"); at != section.end() && at->is_array()) {
            std::vector<int> combo;
            for (const auto& value : *at) {
                if (value.is_number_integer()) {
                    combo.push_back(value.get<int>());
                }
            }
            m_pageKey.set(std::move(combo));
        }
        const auto readKeys = [&section](const char* key, Hotkey& into, std::vector<int> fallback) {
            const auto at = section.find(key);
            if (at == section.end() || !at->is_array()) {
                into.set(std::move(fallback));
                return;
            }
            std::vector<int> combo;
            for (const auto& value : *at) {
                if (value.is_number_integer()) {
                    combo.push_back(value.get<int>());
                }
            }
            into.set(std::move(combo));
        };
        readKeys("layerUpKeys", m_layerUpKey, {kLayerUpDefaultKey});
        readKeys("layerDownKeys", m_layerDownKey, {kLayerDownDefaultKey});
        readKeys("easyPlaceToggleKeys", m_easyToggleKey, {});
        readKeys("pickBlockKeys", m_pickBlockKey, {});
        for (int i=0;i<6;++i) readKeys(kPlacementKeyNames[i],m_placementKeys[i],{});
        for (int i=0;i<4;++i) readKeys(kAreaKeyNames[i],m_areaKeys[i],{});
    }
    m_layerAxis.store(std::clamp(Config::getInt(section, "layerAxis", 1), 0, 2));
    m_layerMode.store(std::clamp(Config::getInt(section, "layerMode", kLayerAll), 0,
                                 kLayerModeCount - 1));
    m_layerValue.store(clampLayerValue(m_layerAxis.load(), Config::getInt(section, "layerValue", 64)));
    m_layerMin.store(clampLayerValue(m_layerAxis.load(), Config::getInt(section, "layerMin", 0)));
    m_layerMax.store(clampLayerValue(m_layerAxis.load(), Config::getInt(section, "layerMax", 0)));

    m_diffBoxes.store(Config::getBool(section, "diffBoxes", true));
    m_placementOutlines.store(Config::getBool(section,"placementOutlines",true));
    m_diffXray.store(Config::getBool(section, "diffXray", true));
    m_ghostOverMismatch.store(Config::getBool(section, "ghostOverMismatch", true));
    m_boxAlpha.store(std::clamp(Config::getInt(section, "boxOpacity", 35), kMinAlpha, kMaxAlpha));
    {
        std::lock_guard lock(m_verifierMutex);
        m_verifierIgnored.clear(); m_verifierSelection = {}; m_verifierAll.store(false);
        if (const auto at = section.find("verifierIgnored"); at != section.end() && at->is_array()) {
            for (const auto& value : *at) {
                if (!value.is_object()) continue;
                const auto kind = value.find("kind"), want = value.find("want"), real = value.find("real");
                if (kind == value.end() || !kind->is_number_integer() || want == value.end() || !want->is_string()
                    || real == value.end() || !real->is_string()) continue;
                const int k = kind->get<int>();
                if (k != 1 && k != 2 && k != 3 && k != 6) continue;
                schematicworld::VerifierIgnored ignored{static_cast<schematic::DiffKind>(k),
                    want->get<std::string>(), real->get<std::string>()};
                if (std::find(m_verifierIgnored.begin(), m_verifierIgnored.end(), ignored) == m_verifierIgnored.end())
                    m_verifierIgnored.push_back(std::move(ignored));
            }
        }
        m_verifierIgnoredPending = true; refreshVerifier();
    }

    std::lock_guard<std::mutex> filesGuard(m_filesMutex);
    m_placementsMigrated=Config::getBool(section,"placementsMigrated",false);
    m_legacyBlueprints=nlohmann::json::array();
    if (const auto at=section.find("blueprints");at!=section.end() && at->is_array()) m_legacyBlueprints=*at;
}

void Schematica::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["pasteReplace"] = m_pasteReplace.load();
    section["pasteCommandsPerTick"] = m_pastePerTick.load();
    section["pasteMuteCommandFeedback"] = m_pasteMute.load();
    section["pasteOnlyVisibleLayers"] = m_pasteVisibleOnly.load();
    if (m_enabledAtShutdown.load(std::memory_order_relaxed)) {
        section["enabled"] = true;
    }

    section["materialListType"] = materialListType();
    section["materialAllPlacements"] = materialScopeAll();
    section["materialMultiplier"] = materialMultiplier();
    section["materialHideAvailable"] = materialHideAvailable();
    section["materialSort"] = materialSort();
    section["materialSortDescending"] = materialSortDescending();
    MaterialCollector::instance().saveConfig(section);
    { std::lock_guard guard(m_filesMutex); section["materialIgnored"] = m_ignoredMaterials; }
    section["opacity"] = m_alpha.load();
    section["easyPlace"] = m_easyPlace.load();
    section["easyPlaceRotate"] = m_easyPlaceRotate.load();
    section["easyPlaceAutoSneak"] = m_easyAutoSneak.load();
    section["easyPlaceHold"] = m_easyPlaceHold.load();
    section.erase("easyPlaceHoldIntervalMs");
    section.erase("easyPlaceReach");
    section["easyPlacePickSlots"] = easyplace::slotMaskText(m_pickSlots.load());
    section["placementRestriction"] = m_placementRestriction.load();
    section["diffBoxes"] = m_diffBoxes.load();
    section["placementOutlines"] = m_placementOutlines.load();
    section["diffXray"] = m_diffXray.load();
    section["ghostOverMismatch"] = m_ghostOverMismatch.load();
    section["boxOpacity"] = m_boxAlpha.load();
    {
        std::lock_guard lock(m_verifierMutex);
        auto ignored = nlohmann::json::array();
        for (const auto& value : m_verifierIgnored) ignored.push_back({
            {"kind", static_cast<int>(value.kind)}, {"want", value.wantName}, {"real", value.realName}});
        section["verifierIgnored"] = std::move(ignored);
    }
    {
        std::lock_guard<std::mutex> keysGuard(m_keysMutex);
        section["pageKeys"] = m_pageKey.combo();
        section["layerUpKeys"] = m_layerUpKey.combo();
        section["layerDownKeys"] = m_layerDownKey.combo();
        section["easyPlaceToggleKeys"] = m_easyToggleKey.combo();
        section["pickBlockKeys"] = m_pickBlockKey.combo();
        for (int i=0;i<6;++i) section[kPlacementKeyNames[i]]=m_placementKeys[i].combo();
        for (int i=0;i<4;++i) section[kAreaKeyNames[i]]=m_areaKeys[i].combo();
    }
    section["layerMode"] = m_layerMode.load();
    section["layerAxis"] = m_layerAxis.load();
    section["layerValue"] = m_layerValue.load();
    section["layerMin"] = m_layerMin.load();
    section["layerMax"] = m_layerMax.load();

    std::lock_guard<std::mutex> guard(m_filesMutex);
    if (m_placementsMigrated) { section.erase("blueprints"); section["placementsMigrated"]=true; }
    else { section["blueprints"]=m_legacyBlueprints; section.erase("placementsMigrated"); }
}

}
