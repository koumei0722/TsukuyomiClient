#include "modules/Schematica.h"

#include "config/WriteSwitches.h"

#include "render/BoxRenderer.h"
#include "render/WorldMesh.h"

#include <algorithm>
#include <array>
#include <map>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <format>
#include <string>
#include <unordered_set>
#include <utility>

#include <Windows.h>

#include <commdlg.h>
#include <objbase.h>

#include "config/Config.h"
#include "core/Logger.h"
#include "modules/FreeCamera.h"
#include "modules/HandRestock.h"
#include "core/Paths.h"
#include "core/Perf.h"
#include "core/Strings.h"
#include "game/BlockRegistry.h"
#include "hooks/Detours.h"
#include "hooks/HookManager.h"
#include "game/BlockWrite.h"
#include "game/GameData.h"
#include "game/Rotation.h"
#include "game/StackCount.h"
#include "game/UiProbe.h"
#include "input/Foreground.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

namespace tsukuyomi {

namespace {

constexpr std::size_t kMaxFiles = 32;

void learnBothSides(std::int32_t x, std::int32_t y, std::int32_t z, void* renderRegion,
                    void* renderSub)
{
    (void)renderRegion;
    blockwrite::noteSubChunkAt(x, y, z, renderSub);
}

}

Schematica& Schematica::instance()
{
    static Schematica module;
    return module;
}

void Schematica::scanFiles()
{
    std::vector<std::wstring> stems;
    std::vector<std::wstring> names;

    const std::filesystem::path dir = paths::schematicsDir();
    if (dir.empty()) {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        m_files.clear();
        m_fileNames.clear();
        return;
    }

    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        m_files.clear();
        m_fileNames.clear();
        return;
    }
    for (const auto& entry : it) {
        if (stems.size() >= kMaxFiles) {
            log().warn(L"Schematica: more than {} files, listing only the first ones",
                       kMaxFiles);
            break;
        }
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc) || fileEc) {
            continue;
        }
        std::filesystem::path path = entry.path();
        std::wstring extension = path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (extension != L".mcstructure") {
            continue;
        }
        stems.push_back(path.stem().wstring());
        names.push_back(path.filename().wstring());
    }

    std::vector<std::size_t> order(stems.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(),
              [&stems](std::size_t a, std::size_t b) { return stems[a] < stems[b]; });
    std::vector<std::wstring> sortedStems;
    std::vector<std::wstring> sortedNames;
    sortedStems.reserve(order.size());
    sortedNames.reserve(order.size());
    for (const std::size_t at : order) {
        sortedStems.push_back(std::move(stems[at]));
        sortedNames.push_back(std::move(names[at]));
    }
    std::lock_guard<std::mutex> guard(m_filesMutex);
    m_files = std::move(sortedStems);
    m_fileNames = std::move(sortedNames);

    {
        std::vector<std::unique_ptr<Blueprint>> next;
        next.reserve(m_files.size());
        std::wstring editingName;
        if (m_editing >= 0 && static_cast<std::size_t>(m_editing) < m_blueprints.size()) {
            editingName = m_blueprints[static_cast<std::size_t>(m_editing)]->name;
        }
        for (std::size_t i = 0; i < m_files.size(); ++i) {
            auto fresh = std::make_unique<Blueprint>();
            fresh->name = m_files[i];
            fresh->fileName = m_fileNames[i];
            for (const auto& old : m_blueprints) {
                if (old && old->name == fresh->name) {
                    fresh->visible.store(old->visible.load());
                    fresh->posX.store(old->posX.load());
                    fresh->posY.store(old->posY.load());
                    fresh->posZ.store(old->posZ.load());
                    fresh->rotation.store(old->rotation.load());
                    fresh->loaded = std::move(old->loaded);
                    fresh->ready = old->ready;
                    fresh->sizeX.store(old->sizeX.load());
                    fresh->sizeY.store(old->sizeY.load());
                    fresh->sizeZ.store(old->sizeZ.load());
                    fresh->solidCount.store(old->solidCount.load());
                    fresh->materials = std::move(old->materials);
                    break;
                }
            }
            for (const Saved& one : m_pendingBlueprints) {
                if (one.name != fresh->name) {
                    continue;
                }
                fresh->visible.store(one.visible);
                fresh->posX.store(one.x);
                fresh->posY.store(one.y);
                fresh->posZ.store(one.z);
                fresh->rotation.store(one.rotation);
                break;
            }
            next.push_back(std::move(fresh));
        }
        m_blueprints = std::move(next);
        m_pendingBlueprints.clear();

        m_editing = -1;
        for (std::size_t i = 0; i < m_blueprints.size(); ++i) {
            if (m_blueprints[i]->name == editingName) {
                m_editing = static_cast<int>(i);
                break;
            }
        }
        if (m_editing < 0 && !m_blueprints.empty()) {
            m_editing = 0;
        }
    }
}

std::size_t Schematica::blueprintCount() const
{
    std::lock_guard<std::mutex> guard(m_filesMutex);
    return m_blueprints.size();
}

std::wstring Schematica::blueprintName(std::size_t at) const
{
    std::lock_guard<std::mutex> guard(m_filesMutex);
    return at < m_blueprints.size() ? m_blueprints[at]->name : std::wstring{};
}

bool Schematica::blueprintVisible(std::size_t at) const
{
    std::lock_guard<std::mutex> guard(m_filesMutex);
    return at < m_blueprints.size() && m_blueprints[at]->visible.load();
}

void Schematica::setBlueprintVisible(std::size_t at, bool on)
{
    {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        if (at >= m_blueprints.size()) {
            return;
        }
        m_blueprints[at]->visible.store(on);
    }
    m_reloadPending.store(true, std::memory_order_relaxed);
}

int Schematica::editingIndex() const
{
    std::lock_guard<std::mutex> guard(m_filesMutex);
    return m_editing;
}

void Schematica::refreshFiles()
{
    scanFiles();
}

void Schematica::selectBlueprintQuiet(std::size_t at)
{
    std::lock_guard<std::mutex> guard(m_filesMutex);
    if (at < m_blueprints.size()) {
        m_editing = static_cast<int>(at);
    }
}

void Schematica::selectBlueprint(std::size_t at)
{
    {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        if (at >= m_blueprints.size()) {
            return;
        }
        m_editing = static_cast<int>(at);
    }
    m_deleteNeedsPick.store(false, std::memory_order_relaxed);
    m_deleteArmedAt.store(0, std::memory_order_relaxed);
    m_deleteArmedName.clear();
}

int Schematica::blueprintPos(std::size_t at, int axis) const
{
    std::lock_guard<std::mutex> guard(m_filesMutex);
    if (at >= m_blueprints.size()) {
        return 0;
    }
    const Blueprint& one = *m_blueprints[at];
    return axis == 0 ? one.posX.load() : (axis == 1 ? one.posY.load() : one.posZ.load());
}

void Schematica::setBlueprintPos(std::size_t at, int axis, int value)
{
    {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        if (at >= m_blueprints.size()) {
            return;
        }
        Blueprint& one = *m_blueprints[at];
        if (axis == 0) {
            one.posX.store(std::clamp(value, kMinXZ, kMaxXZ));
        } else if (axis == 1) {
            one.posY.store(std::clamp(value, kMinY, kMaxY));
        } else {
            one.posZ.store(std::clamp(value, kMinXZ, kMaxXZ));
        }
    }
    m_posChangedAt.store(GetTickCount64(), std::memory_order_relaxed);
}

int Schematica::blueprintRotation(std::size_t at) const
{
    std::lock_guard<std::mutex> guard(m_filesMutex);
    return at < m_blueprints.size() ? (m_blueprints[at]->rotation.load() & 3) : 0;
}

void Schematica::setBlueprintRotation(std::size_t at, int quarters)
{
    bool shown = false;
    {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        if (at >= m_blueprints.size()) {
            return;
        }
        Blueprint& one = *m_blueprints[at];
        const int want = ((quarters % 4) + 4) % 4;
        if (one.rotation.exchange(want) == want) {
            return;
        }
        shown = one.visible.load();
        log().info(L"Schematica: rotated {} to {} degrees", one.name, want * 90);
    }
    if (shown) {
        m_reloadPending.store(true, std::memory_order_relaxed);
    }
}

int Schematica::clampLayerValue(int axis, int value)
{
    return (axis == 1) ? std::clamp(value, -kMaxLayerOffsetY, kMaxLayerOffsetY)
                       : std::clamp(value, kMinXZ, kMaxXZ);
}

bool Schematica::layerOrigin(int& x, int& y, int& z, std::wstring* name) const
{
    x = 0;
    y = 0;
    z = 0;
    if (name != nullptr) {
        name->clear();
    }
    std::lock_guard<std::mutex> guard(m_filesMutex);
    if (m_blueprints.empty()) {
        return false;
    }
    const Blueprint* pick = nullptr;
    const int at = m_editing;
    if (at >= 0 && static_cast<std::size_t>(at) < m_blueprints.size()) {
        pick = m_blueprints[static_cast<std::size_t>(at)].get();
    }
    if (pick == nullptr) {
        for (const auto& one : m_blueprints) {
            if (one->visible.load(std::memory_order_relaxed)) {
                pick = one.get();
                break;
            }
        }
    }
    if (pick == nullptr) {
        return false;
    }
    x = pick->posX.load(std::memory_order_relaxed);
    y = pick->posY.load(std::memory_order_relaxed);
    z = pick->posZ.load(std::memory_order_relaxed);
    if (name != nullptr) {
        *name = pick->name;
    }
    return true;
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

static bool playerBlockPos(int& outX, int& outY, int& outZ)
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
    m_layerVersion.fetch_add(1, std::memory_order_relaxed);
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
    m_layerVersion.fetch_add(1, std::memory_order_relaxed);
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
    m_layerVersion.fetch_add(1, std::memory_order_relaxed);
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
    m_layerVersion.fetch_add(1, std::memory_order_relaxed);
}

void Schematica::applyLayerFilter()
{
    const unsigned long long typedAt = m_layerTypedAt.load(std::memory_order_relaxed);
    if (typedAt != 0 && GetTickCount64() - typedAt < kLayerTypeSettleMs) {
        return;
    }
    bool on = false;
    int axis = 1;
    int lo = 0;
    int hi = 0;
    layerRange(on, axis, lo, hi);
    const bool sameAsApplied = on == m_layerAppliedOn
                               && (!on
                                   || (axis == m_layerAppliedAxis && lo == m_layerAppliedLo
                                       && hi == m_layerAppliedHi));
    if (sameAsApplied) {
        return;
    }
    const bool oldOn = m_layerAppliedOn;
    const int oldAxis = m_layerAppliedAxis;
    const int oldLo = m_layerAppliedLo;
    const int oldHi = m_layerAppliedHi;
    const auto shownBy = [](bool isOn, int a, int l, int h, int wantAxis, int v) {
        if (!isOn) {
            return true;
        }
        if (a != wantAxis) {
            return true;
        }
        return v >= l && v <= h;
    };
    blocks::setLayerFilter(on, axis, lo, hi);
    m_layerAppliedOn = on;
    m_layerAppliedAxis = axis;
    m_layerAppliedLo = lo;
    m_layerAppliedHi = hi;
    log().info(L"Schematica: layer view {} (axis {} / {} to {})",
               on ? L"on" : L"off",
               axis == 0 ? L"X" : (axis == 2 ? L"Z" : L"Y"),
               lo,
               hi);

    std::vector<std::array<std::int32_t, 3>> ask;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        const bool axisChanged = (oldOn && on && oldAxis != axis);
        for (const blockwrite::BlockPos& chunk : regionChunkList()) {
            bool changed = axisChanged;
            if (!changed) {
                const int useAxis = on ? axis : oldAxis;
                const int base = (useAxis == 0) ? chunk.x : ((useAxis == 2) ? chunk.z : chunk.y);
                for (int k = 0; k < 16 && !changed; ++k) {
                    const int v = base + k;
                    changed = shownBy(oldOn, oldAxis, oldLo, oldHi, useAxis, v)
                              != shownBy(on, axis, lo, hi, useAxis, v);
                }
            }
            if (changed) {
                noteChunkChange(chunk.x, chunk.y, chunk.z);
                ask.push_back({chunk.x >> 4, chunk.y >> 4, chunk.z >> 4});
            }
        }
        if (boxes::boxesOn()) {
            publishBoxes(true);
        }
    }
    if (!ask.empty()) {
        const std::uint64_t seq = hooks::requestChunkRebuilds(ask);
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_layerAskChunks = std::move(ask);
        m_layerAskSeq = seq;
        m_layerAskAt = GetTickCount64();
        m_layerAskTries = 0;
        m_layerAskRound = 1;
    }
}

void Schematica::checkLayerRebuilds(unsigned long long now)
{
    if (m_layerAskAt == 0 || now - m_layerAskAt < kLayerCheckMs) {
        return;
    }
    const std::size_t asked = m_layerAskChunks.size();
    std::size_t rebuilt = 0;
    std::size_t stale = 0;
    std::size_t noRecord = 0;
    std::int32_t staleAt[3] = {0, 0, 0};
    for (const std::array<std::int32_t, 3>& one : m_layerAskChunks) {
        std::uint32_t tries = 0;
        std::uint64_t built = 0;
        if (!hooks::chunkLastBuildBoxTries(one[0], one[1], one[2], tries, &built)) {
            ++noRecord;
            continue;
        }
        if (built > m_layerAskSeq) {
            ++rebuilt;
            continue;
        }
        if (stale == 0) {
            staleAt[0] = one[0] << 4;
            staleAt[1] = one[1] << 4;
            staleAt[2] = one[2] << 4;
        }
        ++stale;
    }
    if (stale == 0) {
        m_layerAskChunks.clear();
        m_layerAskChunks.shrink_to_fit();
        m_layerAskAt = 0;
        m_layerAskTries = 0;
        log().info(L"Schematica: the layer view change reached every chunk ({} rebuilt / {} "
                   L"never built of {} asked, {} round(s))",
                   rebuilt,
                   noRecord,
                   asked,
                   m_layerAskRound);
        return;
    }
    std::vector<std::array<std::int32_t, 3>> again;
    again.reserve(stale);
    for (const std::array<std::int32_t, 3>& one : m_layerAskChunks) {
        std::uint32_t tries = 0;
        std::uint64_t built = 0;
        if (hooks::chunkLastBuildBoxTries(one[0], one[1], one[2], tries, &built)
            && built <= m_layerAskSeq) {
            again.push_back(one);
        }
    }
    if (m_layerAskTries + 1 < kLayerAskTries && !again.empty()) {
        ++m_layerAskTries;
        ++m_layerAskRound;
        log().info(L"Schematica: {} of the {} chunk(s) of the layer view change are still "
                   L"showing the old layers, so they are asked again (round {})",
                   again.size(),
                   asked,
                   m_layerAskRound);
        m_layerAskSeq = hooks::requestChunkRebuilds(again);
        m_layerAskChunks = std::move(again);
        m_layerAskAt = now;
        return;
    }
    m_layerAskChunks.clear();
    m_layerAskChunks.shrink_to_fit();
    m_layerAskAt = 0;
    m_layerAskTries = 0;
    log().warn(L"Schematica: {} of the {} chunk(s) asked for the layer view change were not "
               L"rebuilt after {} round(s), so they still show the ghosts of the other layers "
               L"({} rebuilt / {} never built; the first one starts at {},{},{})",
               stale,
               asked,
               m_layerAskRound,
               rebuilt,
               noRecord,
               staleAt[0],
               staleAt[1],
               staleAt[2]);
}

bool Schematica::deleteArmed() const
{
    const unsigned long long armed = m_deleteArmedAt.load(std::memory_order_relaxed);
    return armed != 0 && GetTickCount64() - armed < kDeleteArmMs;
}

bool Schematica::blueprintInfo(std::size_t at, int& sizeX, int& sizeY, int& sizeZ,
                               std::size_t& solid) const
{
    sizeX = 0;
    sizeY = 0;
    sizeZ = 0;
    solid = 0;
    std::lock_guard<std::mutex> guard(m_filesMutex);
    if (at >= m_blueprints.size()) {
        return false;
    }
    const Blueprint& one = *m_blueprints[at];
    sizeX = one.sizeX.load(std::memory_order_relaxed);
    sizeY = one.sizeY.load(std::memory_order_relaxed);
    sizeZ = one.sizeZ.load(std::memory_order_relaxed);
    solid = one.solidCount.load(std::memory_order_relaxed);
    return sizeX > 0 && sizeY > 0 && sizeZ > 0;
}

std::vector<Schematica::MaterialRow> Schematica::blueprintMaterials(std::size_t at,
                                                                   std::size_t limit,
                                                                   std::size_t* kinds) const
{
    std::vector<MaterialRow> out;
    if (kinds != nullptr) {
        *kinds = 0;
    }
    std::lock_guard<std::mutex> guard(m_filesMutex);
    if (at >= m_blueprints.size()) {
        return out;
    }
    out.reserve(std::min<std::size_t>(limit, m_blueprints[at]->materials.size()));
    const auto& src = m_blueprints[at]->materials;
    std::map<std::string, std::size_t> missingByName;
    const bool missingKnown = m_diffTallyLaps.load(std::memory_order_relaxed) != 0;
    if (missingKnown) {
        for (std::size_t e = 0;
             e < m_missingByEntryDone.size() && e < m_missingNamesDone.size(); ++e) {
            if (m_missingByEntryDone[e] != 0) {
                missingByName[m_missingNamesDone[e]] += m_missingByEntryDone[e];
            }
        }
    }
    const int listType = m_materialListType.load(std::memory_order_relaxed);
    std::size_t shown = 0;
    for (std::size_t i = 0; i < src.size(); ++i) {
        MaterialRow row;
        row.name = toUtf16(src[i].first);
        row.count = src[i].second;
        row.ignored = (m_ignoredMaterials.count(src[i].first) != 0);
        row.missingKnown = missingKnown;
        if (const auto found = missingByName.find(src[i].first); found != missingByName.end()) {
            row.missing = found->second;
        }
        row.availableKnown = m_availableKnown;
        if (const auto found = m_availableByName.find(src[i].first);
            found != m_availableByName.end()) {
            row.available = found->second;
        }
        if (row.ignored) {
            continue;
        }
        if (listType == kMaterialMissing && missingKnown && row.missing == 0) {
            continue;
        }
        ++shown;
        if (out.size() >= limit) {
            continue;
        }
        if (const auto icon = m_iconIds.find(src[i].first); icon != m_iconIds.end()) {
            row.iconIdAux = icon->second;
            row.hasIcon = true;
        }
        if (const auto stack = m_stackSizes.find(src[i].first); stack != m_stackSizes.end()) {
            row.stackSize = stack->second;
        }
        out.push_back(std::move(row));
    }
    if (kinds != nullptr) {
        *kinds = shown;
    }
    return out;
}

void Schematica::refreshAvailableCounts()
{
    const unsigned long long now = GetTickCount64();
    const bool asked = m_materialRefreshWanted.exchange(false, std::memory_order_relaxed);
    if (!asked && now - m_availableAt < 1000) {
        return;
    }
    m_availableAt = now;

    std::map<const void*, const std::string*> nameByLegacy;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        for (std::size_t i = 0; i < m_paletteLegacy.size() && i < m_paletteNames.size(); ++i) {
            if (m_paletteLegacy[i] != nullptr) {
                nameByLegacy.emplace(m_paletteLegacy[i], &m_paletteNames[i]);
            }
        }
    }
    if (nameByLegacy.empty()) {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        m_availableByName.clear();
        m_availableKnown = false;
        return;
    }

    std::map<std::string, std::size_t> counted;
    const bool ok = HandRestock::instance().forEachInventorySlot(
        [&](const void* block, int count) {
            if (block == nullptr || count <= 0) {
                return;
            }
            const void* const legacy = blocks::legacyOfFast(block);
            if (legacy == nullptr) {
                return;
            }
            const auto found = nameByLegacy.find(legacy);
            if (found != nameByLegacy.end()) {
                counted[*found->second] += static_cast<std::size_t>(count);
            }
        });

    std::lock_guard<std::mutex> guard(m_filesMutex);
    m_availableByName = std::move(counted);
    m_availableKnown = ok;
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

bool Schematica::writeMaterialList(std::size_t at, std::wstring& outPath)
{
    std::wstring name;
    {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        if (at >= m_blueprints.size()) {
            return false;
        }
        name = m_blueprints[at]->name;
    }
    std::size_t kinds = 0;
    const std::vector<MaterialRow> rows = blueprintMaterials(at, 4096, &kinds);

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
        out << toUtf8(row.name) << '\t' << row.count << '\t';
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

void Schematica::diffTally(std::size_t (&out)[blocks::kDiffKindCount]) const
{
    const bool done = m_diffTallyLaps.load(std::memory_order_relaxed) != 0;
    for (std::size_t i = 0; i < blocks::kDiffKindCount; ++i) {
        out[i] = done ? m_diffTallyDone[i] : m_diffTally[i];
    }
}

void Schematica::closeDiffTally()
{
    for (std::size_t i = 0; i < blocks::kDiffKindCount; ++i) {
        m_diffTallyDone[i] = m_diffTally[i];
        m_diffTally[i] = 0;
    }
    {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        m_missingByEntryDone = m_missingByEntry;
        m_missingNamesDone = m_paletteNames;
    }
    std::fill(m_missingByEntry.begin(), m_missingByEntry.end(), 0);
    m_diffByEntryDone = m_diffByEntry;
    std::fill(m_diffByEntry.begin(), m_diffByEntry.end(), 0);
    m_diffTallyLaps.fetch_add(1, std::memory_order_relaxed);
}

void Schematica::logDrawDiag(unsigned long long now)
{
    (void)now;
    std::size_t overlayStacked = 0;
    std::size_t overlayMissed = 0;
    hooks::overlayStats(overlayStacked, overlayMissed);
    log().info(L"Schematica: faces since injection - ghost {} / layer swap {} / empty chunk "
               L"answered {} (the color boxes are drawn by the overlay/name-tag "
               L"materials - see the WorldMesh line); overlay on mismatched cells since the "
               L"last line - stacked {} / stacked nothing {}",
               blocks::ghostHitCount(blocks::GhostHook::Tessellate),
               blocks::ghostHitCount(blocks::GhostHook::Layer),
               blocks::ghostHitCount(blocks::GhostHook::Filled),
               overlayStacked,
               overlayMissed);

    std::size_t ask[9] = {};
    hooks::askBuildStats(ask);
    std::size_t preds = 0;
    std::size_t scans = 0;
    std::size_t getBlockGhost = 0;
    std::size_t getBlockCalls = 0;
    hooks::hotPathStats(preds, scans, getBlockGhost, getBlockCalls);
    log().info(L"Schematica: chunk rebuild asks since the last line - forced round(s) {} / "
               L"records found {} / skipped as already built {} / forced near booked {} refused "
               L"{} / forced far booked {} refused {} / normal booked {} refused {}; the \"is "
               L"this chunk empty\" hooks were asked {} time(s) (storage) and {} time(s) "
               L"(visibility scan); getBlock {} of {}",
               ask[0], ask[1], ask[2], ask[3], ask[4], ask[5], ask[6], ask[7], ask[8],
               preds, scans, getBlockGhost, getBlockCalls);

    std::size_t re[9] = {};
    hooks::chunkRebuildStats(re);
    log().info(L"Schematica: chunk rebuild retries since the last line - queued {} / done "
               L"(build seen) {} / tried {} / waited (no record) {} / gave up {} / still "
               L"waiting {} / booked {} refused {} / empty flag cleared {}",
               re[0], re[1], re[2], re[3], re[4], re[5], re[6], re[7], re[8]);

    if (m_diffTallyLaps.load(std::memory_order_relaxed) == 0 || m_diffByEntryDone.empty()) {
        return;
    }
    struct Row {
        std::size_t missing = 0;
        std::size_t wrong = 0;
        std::size_t state = 0;
        std::size_t match = 0;
        std::size_t unknown = 0;
        std::size_t bad() const { return missing + wrong + state; }
    };
    std::map<std::string, Row> byName;
    const auto at = [this](std::size_t base, blocks::DiffKind kind) {
        return m_diffByEntryDone[base + static_cast<std::size_t>(kind)];
    };
    for (std::size_t e = 0; e < m_paletteNames.size(); ++e) {
        const std::size_t base = e * blocks::kDiffKindCount;
        if (base + blocks::kDiffKindCount > m_diffByEntryDone.size()) {
            break;
        }
        Row& row = byName[m_paletteNames[e]];
        row.match += at(base, blocks::DiffKind::Match);
        row.missing += at(base, blocks::DiffKind::Missing);
        row.wrong += at(base, blocks::DiffKind::Wrong);
        row.state += at(base, blocks::DiffKind::State);
        row.unknown += at(base, blocks::DiffKind::UnknownWorld)
                       + at(base, blocks::DiffKind::UnknownWant);
    }
    std::vector<std::pair<std::string, Row>> rows;
    rows.reserve(byName.size());
    std::size_t kinds = 0;
    for (const auto& [name, row] : byName) {
        if (row.missing + row.wrong + row.state + row.match + row.unknown == 0) {
            continue;
        }
        if (row.bad() != 0) {
            ++kinds;
        }
        rows.emplace_back(name, row);
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.second.bad() != b.second.bad()) {
            return a.second.bad() > b.second.bad();
        }
        return a.second.match > b.second.match;
    });
    static constexpr std::string_view kVanilla{"minecraft:"};
    std::wstring line;
    for (std::size_t i = 0; i < rows.size() && i < kDiagTopEntries; ++i) {
        if (!line.empty()) {
            line += L", ";
        }
        std::string_view name{rows[i].first};
        if (name.starts_with(kVanilla)) {
            name.remove_prefix(kVanilla.size());
        }
        line += std::format(L"{} {}/{}/{}/{}",
                            toUtf16(name),
                            rows[i].second.missing,
                            rows[i].second.wrong,
                            rows[i].second.state,
                            rows[i].second.match);
        if (rows[i].second.unknown != 0) {
            line += std::format(L" (world not read {})", rows[i].second.unknown);
        }
    }
    log().info(L"Schematica: materials as missing/wrong/state/match - {} ({} of the {} "
               L"material(s) differ from the world; showing the {} with the most differences)",
               line.empty() ? std::wstring{L"(none)"} : line,
               kinds,
               rows.size(),
               (std::min)(rows.size(), kDiagTopEntries));
}

bool Schematica::deleteBlueprint(std::size_t at)
{
    std::wstring fileName;
    {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        if (at >= m_blueprints.size()) {
            return false;
        }
        fileName = m_blueprints[at]->fileName;
        m_blueprints[at]->visible.store(false);
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

void Schematica::noteLapChunk(std::unordered_set<std::uint64_t>& into, std::int32_t x,
                              std::int32_t y, std::int32_t z)
{
    into.insert(packCell(x >> 4, y >> 4, z >> 4));
}

void Schematica::noteChunkChange(std::int32_t x, std::int32_t y, std::int32_t z)
{
    const std::uint64_t key = packCell(x >> 4, y >> 4, z >> 4);
    ChunkChange& one = m_chunkChanges[key];
    one.cx = x >> 4;
    one.cy = y >> 4;
    one.cz = z >> 4;
    one.seq = hooks::nextBuildSeq();
}

std::size_t Schematica::healStaleChunks(unsigned long long now)
{
    std::vector<std::array<std::int32_t, 3>> ask;
    for (auto& [key, one] : m_chunkChanges) {
        if (m_learnedKeys.find(key) == m_learnedKeys.end()) {
            continue;
        }
        if (m_lapTagChunks.find(key) != m_lapTagChunks.end()
            || m_lapColorChunks.find(key) != m_lapColorChunks.end()) {
            continue;
        }
        std::uint32_t tries = 0;
        std::uint64_t built = 0;
        if (!hooks::chunkLastBuildBoxTries(one.cx, one.cy, one.cz, tries, &built)) {
            continue;
        }
        if (built > one.seq) {
            continue;
        }
        if (one.askedSeq == one.seq) {
            if (now - one.askedAt < kHealRetryMs || one.askCount >= kHealMaxAsks) {
                continue;
            }
        } else {
            one.askCount = 0;
        }
        one.askedSeq = one.seq;
        one.askedAt = now;
        ++one.askCount;
        ask.push_back({one.cx, one.cy, one.cz});
    }
    if (!ask.empty()) {
        hooks::requestChunkRebuilds(ask);
    }
    return ask.size();
}

std::vector<Schematica::PrepEntry> Schematica::snapshotVisible(bool& needLoad)
{
    needLoad = false;
    std::vector<PrepEntry> out;
    const std::filesystem::path dir = paths::schematicsDir();
    std::lock_guard<std::mutex> guard(m_filesMutex);
    for (const auto& one : m_blueprints) {
        if (!one->visible.load()) {
            if (one->ready || one->loaded) {
                one->loaded.reset();
                one->ready = false;
            }
            continue;
        }
        PrepEntry entry;
        entry.name = one->name;
        entry.path = dir / one->fileName;
        entry.x = one->posX.load();
        entry.y = one->posY.load();
        entry.z = one->posZ.load();
        entry.rotation = one->rotation.load() & 3;
        if (one->ready && one->loaded) {
            entry.data = one->loaded;
        } else {
            needLoad = true;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

std::vector<schematic::Placement> Schematica::placementsOf(const std::vector<PrepEntry>& entries)
{
    std::vector<schematic::Placement> out;
    out.reserve(entries.size());
    for (const PrepEntry& one : entries) {
        if (!one.data) {
            continue;
        }
        schematic::Placement placed;
        placed.data = one.data;
        placed.x = one.x;
        placed.y = one.y;
        placed.z = one.z;
        placed.rotation = one.rotation;
        out.push_back(std::move(placed));
    }
    schematic::assignPaletteBases(out);
    return out;
}

namespace {

bool samePalette(const std::vector<schematic::Placement>& a,
                 const std::vector<schematic::Placement>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].data != b[i].data || (a[i].rotation & 3) != (b[i].rotation & 3)
            || a[i].paletteBase != b[i].paletteBase) {
            return false;
        }
    }
    return true;
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
        std::vector<schematic::Placement> placements = placementsOf(entries);
        const std::vector<schematic::LayoutKey> key = schematic::layoutOf(placements);
        const bool unchanged = key == m_cellsKey
                               && (key.empty() ? m_cells.empty() : !m_cells.empty());
        if (unchanged) {
            const perf::Scope perfScope{perf::Slot::Install};
            const bool paletteSame = samePalette(placements, m_live);
            m_live = std::move(placements);
            if ((purposes & kPrepReload) != 0 || m_paletteStale || !paletteSame) {
                resolvePalette(m_live);
            }
            if ((purposes & (kPrepReload | kPrepRedraw)) != 0) {
                log().info(L"Schematica: {} blueprint(s) / {} cell(s) (unchanged, reused)",
                           m_live.size(),
                           m_cells.size());
            }
            m_cellsDirty.store(false, std::memory_order_relaxed);
            finishPrepared(purposes);
            return;
        }
    }

    auto job = std::make_unique<PrepareJob>();
    job->purposes = purposes;
    job->entries = std::move(entries);
    job->prevKey = m_cellsKey;
    job->prevHasCells = !m_cells.empty();
    job->startedAt = GetTickCount64();
    std::size_t toRead = 0;
    for (const PrepEntry& one : job->entries) {
        if (!one.data) {
            ++toRead;
        }
    }
    if (!ensureWorker()) {
        runPrepareJob(*job);
        installPrepared(std::move(job));
        return;
    }
    log().info(L"Schematica: preparing {} blueprint(s) in the background ({} file(s) to read)",
               job->entries.size(),
               toRead);
    m_prepBusy = true;
    postJob(std::move(job));
}

void Schematica::runPrepareJob(PrepareJob& job)
{
    const unsigned long long began = GetTickCount64();
    for (PrepEntry& one : job.entries) {
        if (one.data) {
            continue;
        }
        if (m_workCancel.load(std::memory_order_relaxed)) {
            job.cancelled = true;
            return;
        }
        const perf::Scope perfScope{perf::Slot::Load};
        one.result = schematic::loadBlueprint(one.path);
        one.loadedNow = true;
        if (one.result.ok()) {
            one.data = one.result.data;
        }
    }
    const unsigned long long loaded = GetTickCount64();
    job.loadMs = loaded - began;

    job.placements = placementsOf(job.entries);
    job.key = schematic::layoutOf(job.placements);
    const bool unchanged = job.key == job.prevKey
                           && (job.key.empty() ? !job.prevHasCells : job.prevHasCells);
    if (!unchanged) {
        auto cells = std::make_unique<schematic::CellSet>();
        const perf::Scope perfScope{perf::Slot::Cells};
        if (!schematic::buildCells(job.placements, *cells, &m_workCancel)) {
            job.cancelled = true;
            return;
        }
        job.cells = std::move(cells);
    }
    job.cellsMs = GetTickCount64() - loaded;
}

void Schematica::installPrepared(std::unique_ptr<PrepareJob> job)
{
    const perf::Scope perfScope{perf::Slot::Install};
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
        for (PrepEntry& one : job->entries) {
            if (!one.loadedNow) {
                continue;
            }
            notes.push_back(LoadNote{one.name, &one.result,
                                     one.result.data ? one.result.data->palette.size() : 0});
            Blueprint* found = nullptr;
            for (const auto& bp : m_blueprints) {
                if (bp->name == one.name) {
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
            found->materials = one.result.materials;
            if (found->visible.load() && !found->ready) {
                found->loaded = one.data;
                found->ready = true;
            }
        }
    }
    for (const LoadNote& one : notes) {
        const schematic::Loaded& got = *one.result;
        if (!got.ok()) {
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
    }

    bool needLoad = false;
    std::vector<PrepEntry> nowEntries = snapshotVisible(needLoad);
    std::vector<schematic::Placement> placements = placementsOf(nowEntries);
    const std::vector<schematic::LayoutKey> key = schematic::layoutOf(placements);
    if (key != job->key) {
        log().info(L"Schematica: the layout changed while it was being prepared, so it is "
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
    m_cellsDirty.store(false, std::memory_order_relaxed);
    log().info(L"Schematica: {} blueprint(s) / {} cell(s){}{} - prepared in the background in "
               L"{} ms (read {} ms / cells {} ms)",
               m_live.size(),
               m_cells.size(),
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
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_cells.swap(cells->cells);
        m_cellAt.swap(cells->cellAt);
        m_boxes.swap(cells->boxes);
        m_chunkCells.swap(cells->chunkCells);
        m_cellsKeep.swap(cells->keep);
        std::swap(m_region, cells->region);
        m_cellsKey = cells->key;
    }
    postTrash(std::move(cells));
}

void Schematica::finishPrepared(unsigned purposes)
{
    if ((purposes & kPrepReload) != 0) {
        m_drawnUpTo = 0;
        m_drawnPlaced = 0;
        m_waitedLogged = false;
        m_firstLogged = false;
        m_anchorLogged = false;
        m_diffUpTo = 0;
        m_diffLaps = 0;
        m_diffLoggedAt = 0;
        for (std::size_t& one : m_diffTally) {
            one = 0;
        }
        m_diffTallyLaps.store(0, std::memory_order_relaxed);
        m_lapTagChunks.clear();
        m_lapColorChunks.clear();
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_chunkChanges.clear();
        }
        hooks::clearChunkBoxTries();
        if (m_placed.empty()) {
            if (enabled()) {
                m_drawPending.store(true, std::memory_order_relaxed);
            }
        } else {
            m_clearedUpTo = 0;
            m_clearPending.store(true, std::memory_order_relaxed);
        }
    }
    if ((purposes & kPrepRedraw) != 0) {
        m_redrawPending.store(true, std::memory_order_relaxed);
    }
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
        log().warn(L"Schematica: could not start the background loader thread (error {}), "
                   L"so schematics are prepared on the game thread",
                   GetLastError());
        return false;
    }
    m_workThread = thread;
    return true;
}

void Schematica::workerMain()
{
    for (;;) {
        std::unique_ptr<PrepareJob> job;
        std::vector<std::unique_ptr<schematic::CellSet>> trash;
        {
            std::unique_lock<std::mutex> lock(m_workMutex);
            m_workCv.wait(lock, [this] {
                return m_workStop || m_workJob != nullptr || !m_workTrash.empty();
            });
            trash.swap(m_workTrash);
            job = std::move(m_workJob);
            if (m_workStop && job == nullptr && trash.empty()) {
                return;
            }
        }
        trash.clear();
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

void Schematica::postTrash(std::unique_ptr<schematic::CellSet> trash)
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
    }
    CloseHandle(thread);
    std::unique_ptr<PrepareJob> job;
    std::unique_ptr<PrepareJob> done;
    std::vector<std::unique_ptr<schematic::CellSet>> trash;
    {
        std::lock_guard<std::mutex> guard(m_workMutex);
        job = std::move(m_workJob);
        done = std::move(m_workDone);
        trash.swap(m_workTrash);
    }
}

void Schematica::resolvePalette(const std::vector<schematic::Placement>& live)
{
    {
        bool anyData = false;
        for (const schematic::Placement& bp : live) {
            anyData = anyData || static_cast<bool>(bp.data);
        }
        if (!anyData && m_cells.empty() && m_placed.empty()) {
            m_paletteStale = false;
            return;
        }
    }
    std::vector<std::string> wanted;
    for (const schematic::Placement& bp : live) {
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
    if (std::find(wanted.begin(), wanted.end(), std::string{kGhostBlock}) == wanted.end()) {
        wanted.emplace_back(kGhostBlock);
    }
    if (std::find(wanted.begin(), wanted.end(), std::string{kPokeBlock}) == wanted.end()) {
        wanted.emplace_back(kPokeBlock);
    }

    blocks::Table table;
    if (!blocks::resolve(wanted, table)) {
        return;
    }

    log().info(L"Schematica: resolved {}/{} names (table has {} entries)",
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

    if (blocks::calibrateItemIds()) {
        std::unordered_map<std::string, std::int32_t> icons;
        std::size_t missingIcons = 0;
        for (const schematic::Placement& bp : live) {
            if (!bp.data) {
                continue;
            }
            for (const structure::PaletteEntry& entry : bp.data->palette) {
                if (entry.name.empty() || entry.name == "minecraft:air"
                    || icons.find(entry.name) != icons.end()) {
                    continue;
                }
                const auto found = table.find(entry.name);
                std::int32_t idAux = 0;
                if (found != table.end() && blocks::blockItemIdAux(found->second, idAux)) {
                    icons.emplace(entry.name, idAux);
                } else {
                    ++missingIcons;
                }
            }
        }
        {
            std::lock_guard<std::mutex> guard(m_filesMutex);
            for (const auto& one : icons) {
                m_iconIds[one.first] = one.second;
            }
            m_iconMissing = missingIcons;
        }
    }

    {
        std::unordered_map<std::string, int> stacks;
        std::size_t missingStacks = 0;
        for (const schematic::Placement& bp : live) {
            if (!bp.data) {
                continue;
            }
            for (const structure::PaletteEntry& entry : bp.data->palette) {
                if (entry.name.empty() || entry.name == "minecraft:air"
                    || stacks.find(entry.name) != stacks.end()) {
                    continue;
                }
                const auto found = table.find(entry.name);
                int size = 0;
                if (found != table.end()
                    && blocks::maxStackSizeOf(entry.name.c_str(), found->second, size)) {
                    const int guess = stackcount::maxStackSize(entry.name);
                    if (guess < size) {
                        size = guess;
                    }
                    stacks.emplace(entry.name, size);
                } else {
                    ++missingStacks;
                }
            }
        }
        if (!stacks.empty() || missingStacks != 0) {
            {
                std::lock_guard<std::mutex> guard(m_filesMutex);
                for (const auto& one : stacks) {
                    m_stackSizes[one.first] = one.second;
                }
                m_stackMissing = missingStacks;
            }
        }
    }

    const auto air = table.find("minecraft:air");
    const auto ghost = table.find(kGhostBlock);
    const auto poke = table.find(kPokeBlock);

    std::vector<const void*> merged;
    std::vector<std::string> mergedKeys;
    std::vector<std::string> mergedNames;
    std::size_t withStates = 0;
    std::size_t changed = 0;
    std::size_t unresolvedTotal = 0;
    std::size_t slots = 0;
    std::size_t rotatedPrints = 0;
    std::size_t rotatedTurned = 0;
    std::size_t rotatedSame = 0;
    std::size_t rotatedFailed = 0;
    bool baseDrift = false;
    for (const schematic::Placement& bp : live) {
        if (!bp.data) {
            continue;
        }
        const structure::Structure& loaded = *bp.data;
        const int quarters = bp.rotation & 3;
        if (quarters != 0) {
            ++rotatedPrints;
        }
        std::vector<const void*> resolved(loaded.palette.size(), nullptr);
        for (std::size_t i = 0; i < loaded.palette.size(); ++i) {
            const structure::PaletteEntry& entry = loaded.palette[i];
            const auto base = table.find(entry.name);
            if (base == table.end()) {
                continue;
            }
            resolved[i] = base->second;
            const auto turnSlot = [&](std::size_t at) {
                if (quarters == 0 || resolved[at] == nullptr) {
                    return;
                }
                bool ok = false;
                const void* const turned = blocks::rotatedBlock(resolved[at], quarters, &ok);
                if (!ok) {
                    ++rotatedFailed;
                } else if (turned == resolved[at]) {
                    ++rotatedSame;
                } else {
                    ++rotatedTurned;
                }
                resolved[at] = turned;
            };
            if (entry.states.empty()) {
                turnSlot(i);
                continue;
            }
            ++withStates;
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
            unresolvedTotal += missing;
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
                if (picked != base->second) {
                    ++changed;
                }
                resolved[i] = picked;
            }
            turnSlot(i);
        }
        slots += resolved.size();
        if (bp.paletteBase != merged.size()) {
            baseDrift = true;
        }
        merged.insert(merged.end(), resolved.begin(), resolved.end());
        for (const structure::PaletteEntry& entry : loaded.palette) {
            mergedKeys.push_back(quarters != 0
                                     ? entry.key() + " (rot " + std::to_string(quarters * 90) + ")"
                                     : entry.key());
            mergedNames.push_back(entry.name);
        }
    }
    if (baseDrift) {
        log().error(L"Schematica: the palette slots do not line up with the cell list "
                    L"(internal error) - some blocks may be drawn as the wrong kind");
    }

    std::lock_guard<std::mutex> guard(m_mutex);
    m_air = air == table.end() ? nullptr : air->second;
    m_ghost = ghost == table.end() ? nullptr : ghost->second;
    m_poke = poke == table.end() ? m_ghost : poke->second;
    m_paletteBlocks = std::move(merged);
    m_paletteKeys = std::move(mergedKeys);
    m_paletteNames = std::move(mergedNames);
    m_paletteLegacy.assign(m_paletteBlocks.size(), nullptr);
    for (std::size_t i = 0; i < m_paletteBlocks.size(); ++i) {
        m_paletteLegacy[i] = blocks::legacyOfFast(m_paletteBlocks[i]);
    }
    m_missingByEntry.assign(m_paletteBlocks.size(), 0);
    m_missingByEntryDone.assign(m_paletteBlocks.size(), 0);
    m_diffByEntry.assign(m_paletteBlocks.size() * blocks::kDiffKindCount, 0);
    m_diffByEntryDone.assign(m_paletteBlocks.size() * blocks::kDiffKindCount, 0);
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
            log().info(L"Schematica: {} known block(s) for the air check ({} kinds)",
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
    if (m_importBusy.exchange(true)) {
        log().info(L"Schematica: the file chooser is already open");
        return;
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
    CloseHandle(thread);
}

void Schematica::runImport(void* ownerWindow)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    wchar_t chosen[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(ownerWindow);
    ofn.lpstrFilter = L"Structure (*.mcstructure)\0*.mcstructure\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = chosen;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Tsukuyomi - import a .mcstructure";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    const bool picked = GetOpenFileNameW(&ofn) != FALSE;

    if (picked) {
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

std::wstring Schematica::currentFileName() const
{
    std::lock_guard<std::mutex> guard(m_filesMutex);
    if (m_selected < 0 || static_cast<std::size_t>(m_selected) >= m_files.size()) {
        return L"(none)";
    }
    return m_files[static_cast<std::size_t>(m_selected)];
}

void Schematica::selectNextFile()
{
    {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        if (m_files.empty()) {
            return;
        }
        const int count = static_cast<int>(m_files.size());
        m_selected = (m_selected < 0) ? 0 : ((m_selected + 1) % count);
    }
    m_reloadPending.store(true, std::memory_order_relaxed);
}

void Schematica::finishImport()
{
    std::wstring stem;
    {
        std::lock_guard<std::mutex> guard(m_importMutex);
        stem = m_importedStem;
    }
    scanFiles();
    std::size_t count = 0;
    {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        count = m_files.size();
        for (std::size_t i = 0; i < m_files.size(); ++i) {
            if (m_files[i] == stem) {
                m_selected = static_cast<int>(i);
                break;
            }
        }
    }
    m_reloadPending.store(true, std::memory_order_relaxed);
    log().info(L"Schematica: now using {} ({} file(s))", stem, count);

    uiprobe::refreshOwnPage();
}

void Schematica::onScansReady()
{
    scanFiles();
    std::lock_guard<std::mutex> guard(m_filesMutex);
    if (m_files.empty()) {
        log().info(L"Schematica: no .mcstructure found in {}",
                   paths::schematicsDir().wstring());
        m_selected = -1;
        return;
    }
    m_selected = 0;
    if (!m_pendingFile.empty()) {
        const std::wstring want = toUtf16(m_pendingFile);
        for (std::size_t i = 0; i < m_files.size(); ++i) {
            if (m_files[i] == want) {
                m_selected = static_cast<int>(i);
                break;
            }
        }
    }
    m_pendingFile.clear();
    log().info(L"Schematica: {} file(s) in {}", m_files.size(),
               paths::schematicsDir().wstring());
    if (enabled()) {
        m_reloadPending.store(true, std::memory_order_relaxed);
    }
}

void Schematica::shutdown()
{
    m_shuttingDown.store(true, std::memory_order_relaxed);
    stopWorker();
    if (!enabled()) {
        return;
    }
    m_enabledAtShutdown.store(true, std::memory_order_relaxed);
    setEnabled(false);

    constexpr int kWaitMs = 1500;
    constexpr int kStepMs = 5;
    bool done = false;
    for (int i = 0; i < kWaitMs / kStepMs; ++i) {
        if (!m_clearRequest.load(std::memory_order_relaxed)
            && !m_clearPending.load(std::memory_order_relaxed)) {
            done = true;
            break;
        }
        Sleep(kStepMs);
    }
    if (done) {
        return;
    }
    log().warn(L"Schematica: the cleanup did not finish in time; restoring directly");
    blocks::clearGhostRegion();
    const std::size_t back = blockwrite::restoreAll();
    if (back > 0) {
        log().info(L"Schematica: restored {} cell(s) directly", back);
    }
}

void Schematica::noteGhostCellFreed(std::int32_t x, std::int32_t y, std::int32_t z)
{
    {
        std::lock_guard<std::mutex> guard(m_freedMutex);
        if (m_freedCells.size() >= kFreedLimit) {
            m_redrawPending.store(true, std::memory_order_relaxed);
            return;
        }
        m_freedCells.push_back(blockwrite::BlockPos{x, y, z});
    }
    m_freedPending.store(true, std::memory_order_relaxed);
}

void Schematica::onEnabledChanged(bool enabled)
{
    m_drawPending.store(false, std::memory_order_relaxed);
    if (!enabled) {
        m_clearRequest.store(true, std::memory_order_relaxed);
        return;
    }
    m_reloadPending.store(true, std::memory_order_relaxed);
}

void Schematica::onUpdate()
{
    if (m_pageKey.triggered() && input::isInGameplay() && !writes::blocked("Schematica:page")) {
        m_pageRequested.store(true, std::memory_order_relaxed);
    }
    const bool up = m_layerUpKey.triggered();
    const bool down = m_layerDownKey.triggered();
    if (!enabled() || !input::isInGameplay()) {
        return;
    }
    if (up != down) {
        stepLayer(up ? 1 : -1);
        log().info(L"Schematica: layer moved {} (key)", up ? L"up" : L"down");
    }
}

void Schematica::onPlayerViewUpdate()
{
    perf::endFrame();
    perf::maybeReport();
    const perf::Scope perfAll{perf::Slot::Schematica};

    const bool dropped = blocks::takeWorldDropped();
    if (const std::uint64_t generation = blockwrite::regionGeneration();
        generation != m_regionGeneration || dropped) {
        const bool first = (m_regionGeneration == 0);
        m_regionGeneration = generation;
        if (!first || dropped) {
            {
                const std::lock_guard<std::mutex> guard(m_mutex);
                m_placed.clear();
                m_ghostChunks.clear();
                m_learnedKeys.clear();
                m_chunkChanges.clear();
                m_layerAskChunks.clear();
                m_layerAskAt = 0;
                m_layerAskTries = 0;
                m_earlyRegion.clear();
            }
            blocks::clearGhostRegion();
            blockwrite::forgetSubChunks();
            hooks::clearStorageMarks();
            hooks::clearChunkRebuilds();
            m_lapTagChunks.clear();
            m_lapColorChunks.clear();
            m_worldSettleUntil = GetTickCount64() + kWorldSettleMs;
            log().info(L"Schematica: the world changed, so what was remembered is dropped "
                       L"(generation {} / placing again after {} ms{})",
                       generation,
                       kWorldSettleMs,
                       dropped ? L" / the blocks had already stopped answering" : L"");
            m_paletteStale = true;
            if (enabled()) {
                m_reloadPending.store(true, std::memory_order_relaxed);
            }
        }
    }

    if (m_pageRequested.exchange(false, std::memory_order_relaxed)) {
        if (!uiprobe::openOwnPage()) {
            log().warn(L"Schematica: the settings page can only be opened from the "
                       L"settings screen for now");
        }
    }

    if (m_importDone.exchange(false, std::memory_order_acquire)) {
        finishImport();
    }

    if (m_clearRequest.exchange(false, std::memory_order_relaxed)) {
        m_clearedUpTo = 0;
        m_clearPending.store(true, std::memory_order_relaxed);
    }
    if (m_reloadPending.exchange(false, std::memory_order_relaxed)) {
        if (!m_prepBusy && m_prepWant == 0) {
            m_loadStartedAt = GetTickCount64();
        }
        requestPrepare(kPrepReload);
    }
    pollPrepare();
    m_ghostWanted.store(!m_cells.empty() || !m_placed.empty()
                            || (m_prepBusy && m_prepHasVisible),
                        std::memory_order_relaxed);
    if (const unsigned long long at = m_posChangedAt.load(std::memory_order_relaxed);
        at != 0 && GetTickCount64() - at >= kPosSettleMs) {
        m_posChangedAt.store(0, std::memory_order_relaxed);
        if (!m_prepBusy && m_prepWant == 0) {
            m_loadStartedAt = GetTickCount64();
        }
        requestPrepare(kPrepRedraw);
    }
    if (m_redrawPending.exchange(false, std::memory_order_relaxed) && enabled()) {
        m_clearedUpTo = 0;
        m_clearPending.store(true, std::memory_order_relaxed);
    }

    applyLayerFilter();

    {
        const bool wantBoxes = enabled() && blocks::ghostOn()
                               && m_diffBoxes.load(std::memory_order_relaxed);
        const bool xray = m_diffXray.load(std::memory_order_relaxed);
        boxes::setStyle(wantBoxes,
                        static_cast<float>(m_boxAlpha.load(std::memory_order_relaxed))
                            / 100.0F,
                        xray);
        const bool wantOverlay =
            enabled() && m_ghostOverMismatch.load(std::memory_order_relaxed);
        if (blocks::ghostOverMismatchOn() != wantOverlay) {
            blocks::setGhostOverMismatch(wantOverlay);
            m_boxModeChanged.store(true, std::memory_order_relaxed);
            log().info(L"Schematica: overlay on mismatched cells - {}",
                       wantOverlay ? L"on" : L"off");
        }
    }

    if (GetTickCount64() >= m_boxModeRetryAt
        && m_boxModeChanged.exchange(false, std::memory_order_relaxed) && enabled()
        && blocks::ghostOn()) {
        if (boxes::boxesOn()) {
            publishBoxes(true);
        }
        hooks::clearChunkBoxTries();
        const std::size_t poked = dirtyChunks(9);
        if (poked == 0) {
            m_boxModeChanged.store(true, std::memory_order_relaxed);
            m_boxModeRetryAt = GetTickCount64() + kBoxModeRetryMs;
        } else if (!m_shuttingDown.load(std::memory_order_relaxed)) {
            if (FreeCamera::instance().borrowForChunkReload()) {
                m_boxReloadWanted.store(false, std::memory_order_relaxed);
            } else if (!m_boxReloadWanted.exchange(true, std::memory_order_relaxed)) {
                m_boxReloadWantedAt.store(GetTickCount64(), std::memory_order_relaxed);
            }
        }
    }

    if (m_boxReloadWanted.load(std::memory_order_relaxed)) {
        const unsigned long long since =
            GetTickCount64() - m_boxReloadWantedAt.load(std::memory_order_relaxed);
        if (!enabled() || !blocks::ghostOn()
            || m_shuttingDown.load(std::memory_order_relaxed)) {
            m_boxReloadWanted.store(false, std::memory_order_relaxed);
        } else if (FreeCamera::instance().borrowForChunkReload()) {
            m_boxReloadWanted.store(false, std::memory_order_relaxed);
        } else if (since >= kBoxModeReloadSlowMs && !m_boxReloadSlowLogged) {
            m_boxReloadSlowLogged = true;
            log().warn(L"Schematica: chunk rebuilds have been asked for {} ms but the camera "
                       L"cannot be borrowed (FreeCamera is on, or the camera write site is "
                       L"missing)", since);
        }
    } else {
        m_boxReloadSlowLogged = false;
    }

    hooks::armPendingStoragePreds();

    if (m_clearPending.load(std::memory_order_relaxed)) {
        clearStep();
        return;
    }
    if (!m_drawPending.load(std::memory_order_relaxed)) {
        if (m_prunePending.load(std::memory_order_relaxed)) {
            if (m_pruneDelay > 0) {
                --m_pruneDelay;
            } else {
                std::lock_guard<std::mutex> guard(m_mutex);
                pruneStep();
            }
            return;
        }
        if (blocks::ghostOn()) {
            if (m_freedPending.exchange(false, std::memory_order_relaxed)) {
                restoreFreedCells();
            }
            {
                const unsigned long long now = GetTickCount64();
                if (now - m_learnAt >= kLearnMs) {
                    m_learnAt = now;
                    std::lock_guard<std::mutex> guard(m_mutex);
                    m_learnedLate += learnRegionSubChunks(true);
                    m_healAsked += healStaleChunks(now);
                    checkLayerRebuilds(now);
                    if (m_learnMissed != 0 && now - m_learnLogAt >= kLearnLogMs) {
                        m_learnLogAt = now;
                        log().info(
                            L"Schematica: the game does not keep the blocks of {} of the {} "
                            L"chunk(s) the schematic covers, so no color box can be decided "
                            L"there (the nearest one is {} block(s) from the camera at "
                            L"{},{},{}, the farthest readable one {}; the game answered \"no "
                            L"chunk\" for {} / skipped as the rest of those columns {} / "
                            L"outside the world {} / other {}; {} chunk(s) were only read "
                            L"because we asked again with the looser \"still being finished\" "
                            L"rule)",
                            m_learnMissed,
                            m_learnTotal,
                            m_learnMissedNear < 0.0 ? -1 : static_cast<int>(m_learnMissedNear),
                            m_learnMissedAt[0],
                            m_learnMissedAt[1],
                            m_learnMissedAt[2],
                            m_learnKeptFar < 0.0 ? -1 : static_cast<int>(m_learnKeptFar),
                            m_learnMissedNotLoaded,
                            m_learnMissedByColumn,
                            m_learnMissedOutside,
                            m_learnMissedOther,
                            blockwrite::findSubChunkLooseHits());
                    }
                    if (now - m_diagLogAt >= kDiagLogMs) {
                        m_diagLogAt = now;
                        logDrawDiag(now);
                    }
                }
            }
            {
                std::lock_guard<std::mutex> guard(m_mutex);
                diffStep();
                publishBoxesLive();
            }
            maybeRenudge();
            refreshAvailableCounts();
        }
        return;
    }
    if (m_worldSettleUntil != 0) {
        if (GetTickCount64() < m_worldSettleUntil) {
            return;
        }
        m_worldSettleUntil = 0;
    }
    if (blockwrite::region() == nullptr) {
        if (!m_waitedLogged) {
            m_waitedLogged = true;
            log().warn(L"Schematica: waiting for the game to write a block ({} chunk mesh "
                       L"threads)",
                       blocks::meshThreadCount());
        }
        return;
    }

    if (m_drawnUpTo == 0 && !HookManager::instance().groupEnabled(HookGroup::Ghost)) {
        return;
    }
    if (m_drawnUpTo == 0) {
        if (m_cellsDirty.exchange(false, std::memory_order_relaxed)) {
            requestPrepare(kPrepCells);
        }
        if (m_prepBusy) {
            return;
        }
    }
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_cells.empty() || m_palette.empty()) {
        m_drawPending.store(false, std::memory_order_relaxed);
        return;
    }

    if (m_drawnUpTo == 0) {
        const Box all = m_region;
        m_anchorX = all.x0;
        m_anchorY = all.y0;
        m_anchorZ = all.z0;
        m_regionSizeX = all.x1 - all.x0;
        m_regionSizeY = all.y1 - all.y0;
        m_regionSizeZ = all.z1 - all.z0;
        if (!m_anchorLogged) {
            m_anchorLogged = true;
            log().info(L"Schematica: anchored at {} {} {} (region {}x{}x{} / {} blueprint(s))",
                       m_anchorX,
                       m_anchorY,
                       m_anchorZ,
                       m_regionSizeX,
                       m_regionSizeY,
                       m_regionSizeZ,
                       m_boxes.size());
        }

        m_lastRenudgeAt = GetTickCount64();
        blockwrite::forgetSubChunks();
        m_learnedKeys.clear();
        m_earlyRegion.clear();
        hooks::clearStorageMarks();
        m_ghostChunks.clear();
        blocks::setGhostRegion(m_anchorX, m_anchorY, m_anchorZ,
                               m_regionSizeX, m_regionSizeY, m_regionSizeZ,
                               static_cast<float>(m_alpha.load()) / 100.0F);
        blocks::setGhostPalette(m_paletteBlocks);

        learnRegionSubChunks(false);
    }

    const std::size_t total = m_cells.size();
    std::size_t placed = 0;
    const perf::Scope perfDraw{perf::Slot::Draw};

    blockwrite::beginPlacement(m_air);

    LARGE_INTEGER frequency{};
    LARGE_INTEGER started{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);
    const long long budgetTicks =
        (frequency.QuadPart * static_cast<long long>(kDrawBudgetMs)) / 1000LL;
    std::size_t at = m_drawnUpTo;
    while (at < total) {
        const std::size_t batchEnd = std::min(total, at + kDrawBatch);
        for (; at < batchEnd; ++at) {
            if (drawCell(at)) {
                ++placed;
            }
        }
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (now.QuadPart - started.QuadPart >= budgetTicks) {
            break;
        }
    }
    const std::size_t stop = at;
    blockwrite::endPlacement();

    m_drawnUpTo = stop;
    const std::size_t placedTotal = m_drawnPlaced + placed;
    if (m_drawnUpTo >= total) {
        m_drawPending.store(false, std::memory_order_relaxed);
        publishBoxes(true);
        closeDiffTally();
        m_diffUpTo = 0;
        m_diffDropped = 0;
        m_diffRestored = 0;
        m_diffChanged = 0;
        m_diffWater = 0;
        m_diffWaterUnknown = 0;
        m_diffWorldWater = 0;
        m_diffWantWater = 0;
        m_lapTagChunks.clear();
        m_lapColorChunks.clear();
        rebuildGhostChunkList();
        std::size_t poked = dirtyChunks(1);
        poked += dirtyChunks(2);
        m_pruneDelay = kPruneDelayFrames;
        m_prunePending.store(true, std::memory_order_relaxed);
        log().info(L"Schematica: {} cell(s) / poked {} chunk(s) ({} {} {}) / {} chunk mesh "
                   L"threads",
                   placedTotal,
                   poked,
                   m_anchorX,
                   m_anchorY,
                   m_anchorZ,
                   blocks::meshThreadCount());
        m_drawnPlaced = 0;
        return;
    }
    m_drawnPlaced += placed;
}

bool Schematica::drawCell(std::size_t at)
{
    const Cell& cell = m_cells[at];

    if (cell.entry == kAirCell) {
        blocks::setGhostWantAir(cell.x, cell.y, cell.z);
        diffCell(at, false);
        return false;
    }
    const std::size_t entry = cell.entry;
    static const std::string kNoName;
    const std::string& name = cell.name != nullptr ? *cell.name : kNoName;
    const void* const block = blockFor(name, entry);
    if (block != nullptr) {
        blocks::setGhostWantCell(cell.x, cell.y, cell.z, entry);
        if (cell.entry2 >= 0) {
            blocks::setGhostWantCell(cell.x, cell.y, cell.z,
                                     static_cast<std::size_t>(cell.entry2), 1);
        }
    }
    const blocks::DiffKind kind = diffCell(at, false);
    if (kind != blocks::DiffKind::Missing || block == nullptr) {
        return false;
    }
    if (!m_firstLogged) {
        m_firstLogged = true;
        log().info(L"Schematica: first block {} at {} {} {} -> drawing only",
                   toUtf16(name),
                   cell.x,
                   cell.y,
                   cell.z);
    }
    return true;
}

blocks::DiffKind Schematica::diffCell(std::size_t at, bool early)
{
    const Cell& cell = m_cells[at];
    const bool wantAir = (cell.entry == kAirCell);
    const void* const want =
        (!wantAir && cell.entry < m_paletteBlocks.size()) ? m_paletteBlocks[cell.entry]
                                                          : nullptr;
    const std::int32_t wx = cell.x;
    const std::int32_t wy = cell.y;
    const std::int32_t wz = cell.z;

    const void* const real = blockwrite::readWorldAt(wx, wy, wz);

    const void* realExtra = nullptr;
    bool realExtraKnown = false;
    if (!wantAir) {
        realExtraKnown = blockwrite::readWorldExtraAt(wx, wy, wz, realExtra);
        if (!realExtraKnown) {
            if (!early) {
                ++m_diffWaterUnknown;
            }
        } else if (realExtra != nullptr && realExtra != m_air) {
            if (!early) {
                ++m_diffWorldWater;
            }
        }
        if (cell.entry2 >= 0) {
            if (!early) {
                ++m_diffWantWater;
            }
        }
    }
    const blocks::DiffKind kind =
        blocks::diffKindOf(real, want, wantAir, realExtra, realExtraKnown,
                           cell.entry2 >= 0);
    if (!early) {
        if (kind == blocks::DiffKind::State && real == want) {
            ++m_diffWater;
        }
        ++m_diffTally[static_cast<std::size_t>(kind)];
        if (kind == blocks::DiffKind::Missing && !wantAir
            && cell.entry < m_missingByEntry.size()) {
            ++m_missingByEntry[cell.entry];
        }
        if (!wantAir) {
            const std::size_t at2 =
                cell.entry * blocks::kDiffKindCount + static_cast<std::size_t>(kind);
            if (at2 < m_diffByEntry.size()) {
                ++m_diffByEntry[at2];
            }
        }
    }
    if (kind == blocks::DiffKind::UnknownWorld) {
        return kind;
    }
    if (kind == blocks::DiffKind::Missing) {
        if (blocks::restoreGhostCellFromWant(wx, wy, wz)) {
            noteChunkChange(wx, wy, wz);
            if (!early) {
                ++m_diffRestored;
                noteLapChunk(m_lapTagChunks, wx, wy, wz);
            }
        }
    }
    if (blocks::setDiffCell(wx, wy, wz, blocks::colorOfDiffKind(kind))) {
        if (!early) {
            ++m_diffChanged;
            noteLapChunk(m_lapColorChunks, wx, wy, wz);
        }
    }

    if (!wantAir && real != m_air && blocks::dropGhostCell(wx, wy, wz)) {
        noteChunkChange(wx, wy, wz);
        if (!early) {
            ++m_diffDropped;
            noteLapChunk(m_lapTagChunks, wx, wy, wz);
        }
    }
    return kind;
}

void Schematica::diffStep()
{
    const perf::Scope perfScope{perf::Slot::Diff};
    const std::size_t total = m_cells.size();
    if (m_air == nullptr || total == 0) {
        return;
    }
    const std::size_t stop = std::min(total, m_diffUpTo + kDiffPerFrame);
    for (std::size_t at = m_diffUpTo; at < stop; ++at) {
        diffCell(at, false);
    }
    m_diffUpTo = stop;
    if (m_diffUpTo < total) {
        return;
    }
    m_diffUpTo = 0;
    ++m_diffLaps;
    const bool boxesOn = m_diffBoxes.load(std::memory_order_relaxed);
    if (m_diffDropped > 0 || m_diffRestored > 0 || (boxesOn && m_diffChanged > 0)) {
        dirtyChunks(3);
        std::vector<std::array<std::int32_t, 3>> chunks;
        chunks.reserve(m_lapTagChunks.size() + (boxesOn ? m_lapColorChunks.size() : 0));
        const auto unpack = [](std::uint64_t key) {
            const auto part = [](std::uint64_t v) {
                const std::int32_t raw = static_cast<std::int32_t>(v & 0x1FFFFFu);
                return (raw & 0x100000) != 0 ? raw - 0x200000 : raw;
            };
            return std::array<std::int32_t, 3>{part(key >> 42), part(key >> 21), part(key)};
        };
        for (const std::uint64_t key : m_lapTagChunks) {
            chunks.push_back(unpack(key));
        }
        if (boxesOn) {
            for (const std::uint64_t key : m_lapColorChunks) {
                if (m_lapTagChunks.find(key) == m_lapTagChunks.end()) {
                    chunks.push_back(unpack(key));
                }
            }
        }
        hooks::requestChunkRebuilds(chunks);
    }
    m_lapTagChunks.clear();
    m_lapColorChunks.clear();

    const bool boxesMoved =
        (m_diffChanged > 0 || m_diffDropped > 0 || m_diffRestored > 0);
    m_diffDropped = 0;
    m_diffRestored = 0;
    m_diffChanged = 0;

    if (boxesMoved) {
        publishBoxes(false);
    }
    closeDiffTally();
    m_diffWater = 0;
    m_diffWaterUnknown = 0;
    m_diffWorldWater = 0;
    m_diffWantWater = 0;
}

void Schematica::publishBoxesLive()
{
    const std::size_t wrote = blocks::takeDiffCellChanges();
    if (wrote != 0) {
        m_boxLiveDirty = true;
    }
    double eye[3] = {};
    const bool haveEye = boxEye(eye);
    if (m_boxListCut && !m_boxLiveDirty && haveEye && !m_boxListEyeValid) {
        m_boxLiveDirty = true;
    } else if (m_boxListCut && !m_boxLiveDirty && haveEye) {
        double d2 = 0.0;
        for (int k = 0; k < 3; ++k) {
            const double d = eye[k] - m_boxListEye[k];
            d2 += d * d;
        }
        double away = 0.0;
        if (std::int32_t mn[3] = {}, mx[3] = {}; blocks::ghostBounds(mn, mx)) {
            double a2 = 0.0;
            for (int k = 0; k < 3; ++k) {
                const double lo = static_cast<double>(mn[k]);
                const double hi = static_cast<double>(mx[k]);
                const double d = eye[k] < lo ? lo - eye[k] : (eye[k] > hi ? eye[k] - hi : 0.0);
                a2 += d * d;
            }
            away = std::sqrt(a2);
        }
        const double step = (std::max)(kBoxRecenterBlocks, away * kBoxRecenterShare);
        if (d2 > step * step) {
            m_boxLiveDirty = true;
        }
    }
    if (!m_boxLiveDirty) {
        return;
    }
    const unsigned long long now = GetTickCount64();
    if (now - m_boxLiveAt < kBoxLiveMs) {
        return;
    }
    m_boxLiveAt = now;
    m_boxLiveDirty = false;
    publishBoxes(false, true);
}

void Schematica::publishBoxes(bool force, bool changed)
{
    const perf::Scope perfScope{perf::Slot::Publish};
    if (!force && !boxes::boxesOn()) {
        ++m_boxSkipped;
        m_boxListCut = false;
        return;
    }
    const std::size_t now = m_diffTally[1] * 1000003u + m_diffTally[2] * 1009u
                            + m_diffTally[3] + m_diffTally[6] * 7919u;
    const unsigned long long at = GetTickCount64();
    if (!force && !changed && now == m_boxSignature && at - m_boxPublishedAt < kBoxPublishMs) {
        ++m_boxSkipped;
        return;
    }
    m_boxSignature = now;
    m_boxPublishedAt = at;
    ++m_boxPublished;

    std::vector<blocks::DiffBox> list;
    std::size_t dropped = 0;
    double eye[3] = {};
    const bool haveEye = boxEye(eye);
    int keptRadius = -1;
    blocks::collectDiffBoxes(list, kBoxLimit, &dropped, haveEye ? eye : nullptr, &keptRadius);
    m_boxListCut = dropped != 0;
    m_boxListEyeValid = keptRadius >= 0;
    std::copy(eye, eye + 3, m_boxListEye);
    if (dropped != 0) {
        if (at - m_boxDropLoggedAt >= kBoxDropLogMs || m_boxDropLoggedAt == 0) {
            m_boxDropLoggedAt = at;
            if (keptRadius >= 0) {
                log().warn(L"Schematica: too many boxes, dropped {} (limit {}), keeping those "
                           L"within {} block(s) of the camera ({},{},{})",
                           dropped,
                           kBoxLimit,
                           keptRadius,
                           static_cast<int>(std::floor(eye[0])),
                           static_cast<int>(std::floor(eye[1])),
                           static_cast<int>(std::floor(eye[2])));
            } else {
                log().warn(L"Schematica: too many boxes, dropped {} (limit {})", dropped,
                           kBoxLimit);
            }
        }
    }
    boxes::setBoxes(std::move(list));
}

bool Schematica::boxEye(double out[3])
{
    if (FreeCamera::instance().borrowing()) {
        if (!m_boxLastEyeValid) {
            return false;
        }
        std::copy(m_boxLastEye, m_boxLastEye + 3, out);
        return true;
    }
    const auto keep = [this, out](double x, double y, double z) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            return false;
        }
        out[0] = x;
        out[1] = y;
        out[2] = z;
        std::copy(out, out + 3, m_boxLastEye);
        m_boxLastEyeValid = true;
        return true;
    };
    if (float eye[3] = {}, vp[16] = {}; boxes::cameraSnapshot(eye, vp)) {
        if (keep(eye[0], eye[1], eye[2])) {
            return true;
        }
    }
    if (GameData::instance().hasPlayerView()) {
        const PlayerView view = GameData::instance().playerView();
        return keep(view.x, view.y, view.z);
    }
    return false;
}

void Schematica::pruneStep()
{
    const perf::Scope perfScope{perf::Slot::Prune};
    m_prunePending.store(false, std::memory_order_relaxed);
    if (m_air == nullptr || m_cells.empty()) {
        return;
    }
    rebuildGhostChunkList();
    const std::size_t poked = dirtyChunks(5);
    {
        std::vector<std::array<std::int32_t, 3>> chunks;
        chunks.reserve(m_ghostChunks.size());
        for (const blockwrite::BlockPos& at : m_ghostChunks) {
            chunks.push_back({at.x >> 4, at.y >> 4, at.z >> 4});
        }
        hooks::requestChunkRebuilds(chunks);
    }
    const unsigned long long elapsed =
        m_loadStartedAt != 0 ? GetTickCount64() - m_loadStartedAt : 0;
    log().info(L"Schematica: ready in {} ms ({} cell(s) / cyan {} red {} orange {} pink {} / "
               L"world not read {} / {} chunk(s) asked again / {} chunk(s) remembered)",
               elapsed,
               m_cells.size(),
               m_diffTallyDone[1],
               m_diffTallyDone[2],
               m_diffTallyDone[3],
               m_diffTallyDone[6],
               m_diffTallyDone[4],
               poked,
               blockwrite::knownSubChunkCount());
}

std::size_t Schematica::dirtyChunks(int who, bool askAll)
{
    if (who > 0 && static_cast<std::size_t>(who) < kDirtyCallers) {
        ++m_dirtyFrom[static_cast<std::size_t>(who)];
    }
    const perf::Scope perfScope{perf::Slot::Dirty};
    if (m_air == nullptr) {
        return 0;
    }
    void* const region = blockwrite::renderRegion();
    if (region == nullptr) {
        m_nudgeFailed = !m_ghostChunks.empty();
        return 0;
    }
    const std::int32_t x0 = m_anchorX;
    const std::int32_t y0 = m_anchorY;
    const std::int32_t z0 = m_anchorZ;
    const std::int32_t x1 = x0 + m_regionSizeX;
    const std::int32_t y1 = y0 + m_regionSizeY;
    const std::int32_t z1 = z0 + m_regionSizeZ;

    (void)x0;
    (void)y0;
    (void)z0;
    (void)x1;
    (void)y1;
    (void)z1;

    std::size_t poked = 0;
    std::unordered_set<std::uint64_t> missedColumns;
    hooks::beginStorageArmBatch();
    for (const blockwrite::BlockPos& chunk : m_ghostChunks) {
        const std::uint64_t column =
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk.x)) << 32)
            | static_cast<std::uint32_t>(chunk.z);
        if (missedColumns.find(column) != missedColumns.end()) {
            continue;
        }
        void* const sub = blockwrite::findSubChunk(region, chunk.x, chunk.y, chunk.z);
        if (sub == nullptr) {
            if (blockwrite::lastFindWasMissingChunk()) {
                missedColumns.insert(column);
            }
            continue;
        }
        ++poked;
        hooks::armStorageHooks(sub, chunk.x, chunk.y, chunk.z);
        learnBothSides(chunk.x, chunk.y, chunk.z, region, sub);
    }
    hooks::endStorageArmBatch();

    if (askAll) {
        hooks::requestGhostChunkBuild();
    }

    m_nudgeFailed = !m_ghostChunks.empty() && poked == 0;
    if (m_nudgeFailed) {
        const unsigned long long now = GetTickCount64();
        if (now - m_nudgeFailLoggedAt >= kNudgeFailLogMs) {
            m_nudgeFailLoggedAt = now;
            std::size_t why[blockwrite::kFindWhyCount] = {};
            blockwrite::findSubChunkStats(why);
            log().warn(L"Schematica: could not reach the storage of {} chunk(s) "
                       L"(this is not a bug when almost all of them are \"chunk not "
                       L"loaded\": color boxes cannot be stacked into chunks the game "
                       L"has not loaded, so they only show up once you get closer) "
                       L"(renderer BlockSource {} / world dead {} / chunk not loaded {} "
                       L"/ outside the world {} / reached {} / faulted {} / other {})",
                       m_ghostChunks.size(),
                       blockwrite::renderRegion() == nullptr ? L"none" : L"present",
                       why[1], why[4], why[6], why[11], why[10],
                       why[0] + why[2] + why[3] + why[5] + why[7] + why[8] + why[9]);
        }
    }
    return poked;
}

void Schematica::earlyLearnFrame()
{
    if (!enabled() || !blocks::ghostOn() || m_shuttingDown.load(std::memory_order_relaxed)) {
        return;
    }
    if (GetCurrentThreadId() != blocks::simThread()) {
        return;
    }
    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        m_earlyBusy.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (m_air == nullptr || m_cells.empty() || m_chunkCells.empty()
        || m_regionSizeX <= 0 || m_regionSizeY <= 0 || m_regionSizeZ <= 0) {
        return;
    }
    void* const region = blockwrite::renderRegion();
    if (region == nullptr) {
        return;
    }
    const std::int32_t key[6] = {m_anchorX, m_anchorY, m_anchorZ,
                                 m_regionSizeX, m_regionSizeY, m_regionSizeZ};
    const std::uint64_t gen = blockwrite::regionGeneration();
    if (m_earlyRegion.empty() || std::memcmp(key, m_earlyRegionKey, sizeof(key)) != 0
        || gen != m_earlyRegionGen) {
        m_earlyRegion = regionChunkList();
        std::memcpy(m_earlyRegionKey, key, sizeof(key));
        m_earlyRegionGen = gen;
        m_earlyCursor = 0;
        m_earlyOutside.clear();
    }
    constexpr std::size_t kCallsPerFrame = 96;
    std::unordered_set<std::uint64_t> missedColumns;
    std::vector<std::array<std::int32_t, 3>> late;
    std::size_t calls = 0;
    std::size_t judged = 0;
    const std::size_t total = m_earlyRegion.size();
    std::size_t visited = 0;
    for (; visited < total && calls < kCallsPerFrame && judged < kEarlyCellBudget; ++visited) {
        const blockwrite::BlockPos& chunk = m_earlyRegion[(m_earlyCursor + visited) % total];
        const std::uint64_t chunkKey = packCell(chunk.x >> 4, chunk.y >> 4, chunk.z >> 4);
        if (m_learnedKeys.find(chunkKey) != m_learnedKeys.end()) {
            continue;
        }
        if (m_earlyOutside.find(chunkKey) != m_earlyOutside.end()) {
            continue;
        }
        const std::uint64_t column =
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk.x)) << 32)
            | static_cast<std::uint32_t>(chunk.z);
        if (missedColumns.find(column) != missedColumns.end()) {
            continue;
        }
        ++calls;
        void* const sub = blockwrite::findSubChunk(region, chunk.x, chunk.y, chunk.z);
        if (sub == nullptr) {
            if (blockwrite::lastFindWasMissingChunk()) {
                missedColumns.insert(column);
            } else if (blockwrite::lastFindWasOutsideWorld()) {
                m_earlyOutside.insert(chunkKey);
            }
            continue;
        }
        blockwrite::noteSubChunkAt(chunk.x, chunk.y, chunk.z, sub);
        m_learnedKeys[chunkKey] = sub;
        const auto cells = m_chunkCells.find(chunkKey);
        if (cells != m_chunkCells.end()) {
            for (const std::uint32_t at : cells->second) {
                if (at < m_cells.size()) {
                    diffCell(at, true);
                }
            }
            m_earlyCells += cells->second.size();
            judged += cells->second.size();
        }
        ++m_earlyChunks;
        if (hooks::chunkHasGeometryNow(chunk.x >> 4, chunk.y >> 4, chunk.z >> 4)) {
            late.push_back({chunk.x >> 4, chunk.y >> 4, chunk.z >> 4});
        }
    }
    m_earlyCalls += calls;
    if (!late.empty()) {
        m_earlyLate += late.size();
        hooks::requestChunkRebuilds(late);
    }
    m_earlyCursor = total != 0 ? (m_earlyCursor + visited) % total : 0;
}

void Schematica::maybeRenudge()
{
    const unsigned long long now = GetTickCount64();

    float px = 0.0F;
    float py = 0.0F;
    float pz = 0.0F;
    bool moved = false;
    if (GameData::instance().playerFeet(px, py, pz)) {
        const int cx = static_cast<int>(std::floor(px)) >> 4;
        const int cz = static_cast<int>(std::floor(pz)) >> 4;
        if (cx != m_lastChunkX || cz != m_lastChunkZ) {
            m_lastChunkX = cx;
            m_lastChunkZ = cz;
            m_chunkMovedAt = now;
            m_chunkSettling = true;
        } else if (m_chunkSettling && now - m_chunkMovedAt >= kChunkSettleMs) {
            m_chunkSettling = false;
            moved = true;
        }
    }
    const unsigned long long wait = m_nudgeFailed ? kRetryMs : kRenudgeMs;
    if (!moved && now - m_lastRenudgeAt < wait) {
        return;
    }
    m_lastRenudgeAt = now;
    std::lock_guard<std::mutex> guard(m_mutex);
    dirtyChunks(6);
}

void Schematica::restoreFreedCells()
{
    const perf::Scope perfScope{perf::Slot::Restore};
    if (m_drawPending.load(std::memory_order_relaxed)
        || m_clearPending.load(std::memory_order_relaxed)) {
        m_freedPending.store(true, std::memory_order_relaxed);
        return;
    }
    std::vector<blockwrite::BlockPos> cells;
    {
        std::lock_guard<std::mutex> guard(m_freedMutex);
        cells.swap(m_freedCells);
    }
    if (cells.empty()) {
        return;
    }
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_cells.empty() || !blocks::ghostOn()) {
        return;
    }
    std::size_t back = 0;
    std::size_t already = 0;
    std::size_t needRebuild = 0;
    for (const blockwrite::BlockPos& one : cells) {
        const std::size_t found = m_cellAt.find(one.x, one.y, one.z);
        if (found == schematic::CellIndex::kNone || found >= m_cells.size()) {
            continue;
        }
        const Cell& cell = m_cells[found];
        if (cell.entry == kAirCell) {
            continue;
        }
        const std::size_t entry = cell.entry;
        if (const void* const real = blockwrite::readWorldAt(one.x, one.y, one.z);
            real != nullptr && real != m_air) {
            continue;
        }
        static const std::string kNoName;
        const void* const block =
            blockFor(cell.name != nullptr ? *cell.name : kNoName, entry);
        if (blocks::ghostCell(one.x, one.y, one.z)) {
            ++already;
            if (block != nullptr && blocks::hasBlockEntity(block)) {
                ++needRebuild;
            }
            continue;
        }
        blocks::setGhostCellBlock(one.x, one.y, one.z, entry);
        {
            const std::int32_t second = cell.entry2;
            if (second >= 0) {
                blocks::setGhostCellBlock(one.x, one.y, one.z,
                                          static_cast<std::size_t>(second), 1);
            }
        }
        ++back;
    }
    if (back > 0 || needRebuild > 0) {
        rebuildGhostChunkList();
        dirtyChunks(7);
    } else if (already != 0) {
    }
}

std::vector<blockwrite::BlockPos> Schematica::regionChunkList() const
{
    std::vector<blockwrite::BlockPos> out;
    if (m_regionSizeX <= 0 || m_regionSizeY <= 0 || m_regionSizeZ <= 0) {
        return out;
    }
    const std::int32_t x0 = m_anchorX;
    const std::int32_t y0 = m_anchorY;
    const std::int32_t z0 = m_anchorZ;
    const std::int32_t x1 = x0 + m_regionSizeX;
    const std::int32_t y1 = y0 + m_regionSizeY;
    const std::int32_t z1 = z0 + m_regionSizeZ;
    for (std::int32_t cx = x0 >> 4; cx <= ((x1 - 1) >> 4); ++cx) {
        for (std::int32_t cy = y0 >> 4; cy <= ((y1 - 1) >> 4); ++cy) {
            for (std::int32_t cz = z0 >> 4; cz <= ((z1 - 1) >> 4); ++cz) {
                out.push_back(blockwrite::BlockPos{cx << 4, cy << 4, cz << 4});
            }
        }
    }
    return out;
}

std::size_t Schematica::learnRegionSubChunks(bool judgeNew)
{
    void* const region = blockwrite::renderRegion();
    if (region == nullptr) {
        return 0;
    }
    std::size_t learned = 0;
    std::size_t judged = 0;
    double eye[3] = {};
    const bool haveEye = boxEye(eye);
    std::size_t missed = 0;
    std::size_t seen = 0;
    std::size_t missedNotLoaded = 0;
    std::size_t missedByColumn = 0;
    std::size_t missedOutside = 0;
    std::size_t missedOther = 0;
    double missedNear = -1.0;
    double keptFar = -1.0;
    std::int32_t missedAt[3] = {0, 0, 0};
    const auto boxDistance = [&eye](const blockwrite::BlockPos& chunk) {
        double d2 = 0.0;
        const double lo[3] = {static_cast<double>(chunk.x), static_cast<double>(chunk.y),
                              static_cast<double>(chunk.z)};
        for (int k = 0; k < 3; ++k) {
            const double hi = lo[k] + 16.0;
            const double d = eye[k] < lo[k] ? lo[k] - eye[k] : (eye[k] > hi ? eye[k] - hi : 0.0);
            d2 += d * d;
        }
        return std::sqrt(d2);
    };
    const auto noteMissed = [&](const blockwrite::BlockPos& chunk) {
        ++missed;
        if (!haveEye) {
            return;
        }
        const double d = boxDistance(chunk);
        if (missedNear < 0.0 || d < missedNear) {
            missedNear = d;
            missedAt[0] = chunk.x;
            missedAt[1] = chunk.y;
            missedAt[2] = chunk.z;
        }
    };
    std::unordered_set<std::uint64_t> missedColumns;
    for (const blockwrite::BlockPos& chunk : regionChunkList()) {
        const std::uint64_t column =
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk.x)) << 32)
            | static_cast<std::uint32_t>(chunk.z);
        const std::uint64_t chunkKey = packCell(chunk.x >> 4, chunk.y >> 4, chunk.z >> 4);
        ++seen;
        if (missedColumns.find(column) != missedColumns.end()) {
            blockwrite::forgetSubChunkAt(chunk.x, chunk.y, chunk.z);
            m_learnedKeys.erase(chunkKey);
            ++missedByColumn;
            noteMissed(chunk);
            continue;
        }
        void* const sub = blockwrite::findSubChunk(region, chunk.x, chunk.y, chunk.z);
        if (sub == nullptr) {
            if (blockwrite::lastFindWasMissingChunk()) {
                missedColumns.insert(column);
                ++missedNotLoaded;
            } else if (blockwrite::lastFindWasOutsideWorld()) {
                ++missedOutside;
            } else {
                ++missedOther;
            }
            blockwrite::forgetSubChunkAt(chunk.x, chunk.y, chunk.z);
            m_learnedKeys.erase(chunkKey);
            noteMissed(chunk);
            continue;
        }
        if (haveEye) {
            keptFar = (std::max)(keptFar, boxDistance(chunk));
        }
        const auto [known, inserted] = m_learnedKeys.try_emplace(chunkKey, sub);
        const bool fresh = inserted || known->second != sub;
        known->second = sub;
        learnBothSides(chunk.x, chunk.y, chunk.z, region, sub);
        ++learned;
        if (judgeNew && fresh) {
            const auto cells = m_chunkCells.find(chunkKey);
            const std::size_t count = (cells != m_chunkCells.end()) ? cells->second.size() : 0;
            if (judged + count > kEarlyCellBudget && judged != 0) {
                m_learnedKeys.erase(chunkKey);
                continue;
            }
            if (cells != m_chunkCells.end()) {
                for (const std::uint32_t at : cells->second) {
                    if (at < m_cells.size()) {
                        diffCell(at, true);
                    }
                }
            }
            judged += count;
            ++m_relearnChunks;
        }
    }
    m_learnMissed = missed;
    m_learnTotal = seen;
    m_learnMissedNotLoaded = missedNotLoaded;
    m_learnMissedByColumn = missedByColumn;
    m_learnMissedOutside = missedOutside;
    m_learnMissedOther = missedOther;
    m_learnMissedNear = missedNear;
    m_learnKeptFar = keptFar;
    std::copy(missedAt, missedAt + 3, m_learnMissedAt);
    return learned;
}

void Schematica::rebuildGhostChunkList()
{
    std::vector<blockwrite::BlockPos> next;
    if (blocks::ghostOn()) {
        for (const blockwrite::BlockPos& at : regionChunkList()) {
            if (blocks::ghostSubChunkOccupied(at.x, at.y, at.z)) {
                next.push_back(at);
            }
        }
    }
    std::unordered_set<std::uint64_t> keep;
    keep.reserve(next.size() * 2);
    for (const blockwrite::BlockPos& n : next) {
        keep.insert(packCell(n.x, n.y, n.z));
    }
    for (const blockwrite::BlockPos& one : m_ghostChunks) {
        if (keep.find(packCell(one.x, one.y, one.z)) == keep.end()) {
            hooks::dropStorageMark(one.x, one.y, one.z);
        }
    }
    m_ghostChunks = std::move(next);
}

const void* Schematica::blockFor(const std::string& name, std::size_t entry) const
{
    if (entry < m_paletteBlocks.size() && m_paletteBlocks[entry] != nullptr) {
        return m_paletteBlocks[entry];
    }
    const auto found = m_palette.find(name);
    return found == m_palette.end() ? nullptr : found->second;
}

void Schematica::clearStep()
{
    const perf::Scope perfScope{perf::Slot::Clear};
    std::lock_guard<std::mutex> guard(m_mutex);

    const bool hadDrawing = !m_cells.empty();
    const bool hadGhosts = !m_ghostChunks.empty();
    blocks::clearGhostRegion();
    hooks::clearStorageMarks();
    if (hadDrawing) {
        const std::size_t poked = dirtyChunks(8);
        if (poked > 0) {
            log().info(L"Schematica: cleared the drawing-only blocks (poked {} chunks)", poked);
        }
    }
    if (m_placed.empty() && hadGhosts && m_air != nullptr
        && m_poke != nullptr && m_regionSizeX > 0) {
        bool poked = false;
        const std::int32_t x1 = m_anchorX + m_regionSizeX;
        const std::int32_t y1 = m_anchorY + m_regionSizeY;
        const std::int32_t z1 = m_anchorZ + m_regionSizeZ;
        for (std::int32_t y = m_anchorY; y < y1 && !poked; ++y) {
            for (std::int32_t x = m_anchorX; x < x1 && !poked; ++x) {
                for (std::int32_t z = m_anchorZ; z < z1; ++z) {
                    if (blockwrite::readWorldAt(x, y, z) != m_air) {
                        continue;
                    }
                    blockwrite::beginSelfWrite();
                    blockwrite::placeAt({x, y, z}, m_poke);
                    blockwrite::placeAt({x, y, z}, m_air);
                    blockwrite::endSelfWrite();
                    poked = true;
                    break;
                }
            }
        }
        if (!poked) {
            log().warn(L"Schematica: no free spot for the round trip that restores the picture "
                       L"(no empty cell)");
        }
    }

    m_ghostChunks.clear();

    const std::size_t back = blockwrite::restoreAll();
    if (back > 0) {
        log().info(L"Schematica: cleared {} block(s)", back);

        if (!m_placed.empty() && m_air != nullptr && m_ghost != nullptr) {
            blockwrite::beginSelfWrite();
            blockwrite::placeAt(m_placed.front(), m_ghost);
            blockwrite::placeAt(m_placed.front(), m_air);
            blockwrite::endSelfWrite();
        }
    }
    m_placed.clear();
    m_clearedUpTo = 0;
    m_prunePending.store(false, std::memory_order_relaxed);
    m_clearPending.store(false, std::memory_order_relaxed);

    if (enabled()) {
        m_drawnUpTo = 0;
        m_drawnPlaced = 0;
        m_firstLogged = false;
        m_diffUpTo = 0;
        for (std::size_t& one : m_diffTally) {
            one = 0;
        }
        m_diffWater = 0;
        m_diffWaterUnknown = 0;
        m_diffWorldWater = 0;
        m_diffWantWater = 0;
        m_drawPending.store(true, std::memory_order_relaxed);
        return;
    }

    if (hadGhosts && !m_shuttingDown.load(std::memory_order_relaxed)) {
        FreeCamera::instance().borrowForChunkReload();
    }
}

MenuItem Schematica::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(menu::back());
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
            L"Page key", [this] { return m_pageKey.combo(); },
            [this](std::vector<int> combo) {
                m_pageKey.set(std::move(combo));
                log().info(L"Schematica: page key set to {}", m_pageKey.name());
            });
        pageKey.hidden = writes::blocked("Schematica:page");
        children.push_back(std::move(pageKey));
    }

    children.push_back(menu::keybind(
        L"Layer up key", [this] { return m_layerUpKey.combo(); },
        [this](std::vector<int> combo) {
            m_layerUpKey.set(std::move(combo));
            log().info(L"Schematica: layer up key set to {}", m_layerUpKey.name());
        },
        {kLayerUpDefaultKey}));
    children.push_back(menu::keybind(
        L"Layer down key", [this] { return m_layerDownKey.combo(); },
        [this](std::vector<int> combo) {
            m_layerDownKey.set(std::move(combo));
            log().info(L"Schematica: layer down key set to {}", m_layerDownKey.name());
        },
        {kLayerDownDefaultKey}));

    {
        MenuItem item = menu::action(L"Import .mcstructure", [this] { startImport(); });
        item.onPage = true;
        children.push_back(std::move(item));
    }

    const auto numberRow = [&children](const wchar_t* label, std::atomic<int>& value, int low,
                                       int high, const std::function<void()>& after) {
        children.push_back(menu::text(
            label, [&value] { return std::format(L"{}", value.load()); },
            [&value, low, high, after](std::wstring typed) {
                int got = 0;
                if (!parseTypedInt(typed, got)) {
                    return;
                }
                value.store(std::clamp(got, low, high));
                if (after) {
                    after();
                }
            }));
        children.back().onPage = true;
    };

    {
        MenuItem item = menu::number(
            L"Opacity", [this] { return static_cast<float>(m_alpha.load()); },
            [this](float value) {
                m_alpha.store(std::clamp(static_cast<int>(value), kMinAlpha, kMaxAlpha));
                m_redrawPending.store(true, std::memory_order_relaxed);
            },
            true, static_cast<float>(kMinAlpha), static_cast<float>(kMaxAlpha));
        item.onPage = true;
        children.push_back(std::move(item));
    }
    {
        MenuItem item = menu::number(
            L"Box opacity", [this] { return static_cast<float>(m_boxAlpha.load()); },
            [this](float value) {
                m_boxAlpha.store(std::clamp(static_cast<int>(value), kMinAlpha, kMaxAlpha));
            },
            true, static_cast<float>(kMinAlpha), static_cast<float>(kMaxAlpha));
        item.onPage = true;
        item.hidden = writes::blocked("Schematica:boxes");
        children.push_back(std::move(item));
    }

    {
        MenuItem item = menu::toggle(
            L"Diff boxes", [this] { return m_diffBoxes.load(std::memory_order_relaxed); },
            [this] {
                const bool on = !m_diffBoxes.load(std::memory_order_relaxed);
                m_diffBoxes.store(on, std::memory_order_relaxed);
            });
        item.onPage = true;
        item.hidden = writes::blocked("Schematica:boxes");
        children.push_back(std::move(item));
    }
    {
        MenuItem item = menu::toggle(
            L"See through", [this] { return m_diffXray.load(std::memory_order_relaxed); },
            [this] {
                m_diffXray.store(!m_diffXray.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
            });
        item.onPage = true;
        item.hidden = writes::blocked("Schematica:boxes");
        children.push_back(std::move(item));
    }
    {
        MenuItem item = menu::toggle(
            L"Ghost over wrong blocks",
            [this] { return m_ghostOverMismatch.load(std::memory_order_relaxed); },
            [this] {
                m_ghostOverMismatch.store(!m_ghostOverMismatch.load(std::memory_order_relaxed),
                                          std::memory_order_relaxed);
            });
        item.onPage = true;
        children.push_back(std::move(item));
    }

    const auto replace = [this] {
        m_posChangedAt.store(GetTickCount64(), std::memory_order_relaxed);
    };
    {
        MenuItem item = menu::toggle(
            L"Show",
            [this] {
                const int at = editingIndex();
                return at >= 0 && blueprintVisible(static_cast<std::size_t>(at));
            },
            [this] {
                const int at = editingIndex();
                if (at < 0) {
                    return;
                }
                const std::size_t which = static_cast<std::size_t>(at);
                setBlueprintVisible(which, !blueprintVisible(which));
            });
        item.onPage = true;
        item.pageTab = 1;
        children.push_back(std::move(item));
    }

    const auto axisRow = [this, &children](const wchar_t* label, int axis) {
        children.push_back(menu::text(
            label,
            [this, axis] {
                const int at = editingIndex();
                return at < 0 ? std::wstring(L"-")
                              : std::format(L"{}", blueprintPos(
                                                       static_cast<std::size_t>(at), axis));
            },
            [this, axis](std::wstring typed) {
                const int at = editingIndex();
                if (at < 0) {
                    return;
                }
                int got = 0;
                if (!parseTypedInt(typed, got)) {
                    return;
                }
                setBlueprintPos(static_cast<std::size_t>(at), axis, got);
            }));
        children.back().onPage = true;
        children.back().pageTab = 1;
    };
    axisRow(L"X", 0);
    axisRow(L"Y", 1);
    axisRow(L"Z", 2);

    {
        MenuItem item = menu::action(L"Delete", [this] {
            const int at = editingIndex();
            if (at < 0) {
                return;
            }
            const unsigned long long now = GetTickCount64();
            const unsigned long long rest = m_deleteRestAt.load(std::memory_order_relaxed);
            if (rest != 0 && now < rest) {
                return;
            }
            if (m_deleteNeedsPick.load(std::memory_order_relaxed)) {
                return;
            }
            const std::wstring name = blueprintName(static_cast<std::size_t>(at));
            const unsigned long long armed = m_deleteArmedAt.load(std::memory_order_relaxed);
            const bool sameOne = (armed != 0 && m_deleteArmedName == name);
            const bool inWindow = (armed != 0 && now - armed < kDeleteArmMs);
            if (sameOne && inWindow && now - armed >= kDeleteMinMs) {
                m_deleteArmedAt.store(0, std::memory_order_relaxed);
                m_deleteArmedName.clear();
                m_deleteRestAt.store(now + kDeleteRestMs, std::memory_order_relaxed);
                m_deleteNeedsPick.store(true, std::memory_order_relaxed);
                log().info(L"Schematica: deleting {} (second press confirmed)", name);
                deleteBlueprint(static_cast<std::size_t>(at));
                return;
            }
            m_deleteArmedAt.store(now, std::memory_order_relaxed);
            m_deleteArmedName = name;
        });
        item.value = [this] {
            if (m_deleteNeedsPick.load(std::memory_order_relaxed)) {
                return std::wstring(L"pick a blueprint first");
            }
            if (!deleteArmed() || m_deleteArmedName.empty()) {
                return std::wstring{};
            }
            return L"press again: " + m_deleteArmedName;
        };
        item.onPage = true;
        item.pageTab = 1;
        children.push_back(std::move(item));
    }

    {
        MenuItem item = menu::cycle(
            L"Rotation",
            [this] {
                const int at = editingIndex();
                static constexpr const wchar_t* kNames[4] = {L"None", L"CW 90", L"CW 180",
                                                             L"CCW 90"};
                return at < 0 ? std::wstring(L"-")
                              : std::wstring(kNames[blueprintRotation(
                                    static_cast<std::size_t>(at))]);
            },
            [this] {
                const int at = editingIndex();
                if (at < 0) {
                    return;
                }
                setBlueprintRotation(static_cast<std::size_t>(at),
                                     blueprintRotation(static_cast<std::size_t>(at)) + 1);
            });
        item.onPage = true;
        item.pageTab = 1;
        children.push_back(std::move(item));
    }

    {
        MenuItem item = menu::action(L"Set to player", [this] {
            const int at = editingIndex();
            if (at < 0) {
                return;
            }
            int px = 0;
            int py = 0;
            int pz = 0;
            if (!playerBlockPos(px, py, pz)) {
                log().warn(L"Schematica: could not read the player position, so the "
                           L"coordinates are unchanged");
                return;
            }
            const std::size_t which = static_cast<std::size_t>(at);
            setBlueprintPos(which, 0, px);
            setBlueprintPos(which, 1, py);
            setBlueprintPos(which, 2, pz);
            log().info(L"Schematica: moved {} to the player position ({}, {}, {})",
                       blueprintName(which),
                       px,
                       py,
                       pz);
        });
        item.value = [] { return std::wstring(L"Copy"); };
        item.onPage = true;
        item.pageTab = 1;
        children.push_back(std::move(item));
    }

    const auto bumpLayer = [this] { m_layerVersion.fetch_add(1, std::memory_order_relaxed); };
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
            [this, bumpLayer] {
                const int next = (m_layerMode.load(std::memory_order_relaxed) + 1) % kLayerModeCount;
                if (next == kLayerRange && m_layerMin.load() == 0 && m_layerMax.load() == 0) {
                    m_layerMin.store(m_layerValue.load());
                    m_layerMax.store(m_layerValue.load());
                }
                m_layerMode.store(next, std::memory_order_relaxed);
                bumpLayer();
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
            [this, bumpLayer] {
                const int next = (m_layerAxis.load(std::memory_order_relaxed) + 1) % 3;
                m_layerAxis.store(next, std::memory_order_relaxed);
                m_layerValue.store(clampLayerValue(next, m_layerValue.load()));
                m_layerMin.store(clampLayerValue(next, m_layerMin.load()));
                m_layerMax.store(clampLayerValue(next, m_layerMax.load()));
                bumpLayer();
            });
        item.onPage = true;
        item.pageTab = 2;
        children.push_back(std::move(item));
    }
    const auto layerNumberRow = [&numberRow, &children, this, bumpLayer](
                                    const wchar_t* label, std::atomic<int>& value) {
        numberRow(label, value, kMinXZ, kMaxXZ, [this, &value, bumpLayer] {
            value.store(clampLayerValue(m_layerAxis.load(std::memory_order_relaxed), value.load()));
            m_layerTypedAt.store(GetTickCount64(), std::memory_order_relaxed);
            bumpLayer();
        });
        children.back().pageTab = 2;
    };
    layerNumberRow(L"Layer", m_layerValue);
    layerNumberRow(L"Range min", m_layerMin);
    layerNumberRow(L"Range max", m_layerMax);
    {
        MenuItem item = menu::action(L"Set to player", [this] { setLayerHere(); });
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
        item.value = [this, withKey] { return withKey(L"+1", m_layerUpKey); };
        item.onPage = true;
        item.pageTab = 2;
        children.push_back(std::move(item));
    }
    {
        MenuItem item = menu::action(L"Move down", [this] { stepLayer(-1); });
        item.value = [this, withKey] { return withKey(L"-1", m_layerDownKey); };
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

    m_alpha.store(std::clamp(Config::getInt(section, "opacity", 45), kMinAlpha, kMaxAlpha));
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
    m_layerAxis.store(std::clamp(Config::getInt(section, "layerAxis", 1), 0, 2));
    m_layerMode.store(std::clamp(Config::getInt(section, "layerMode", kLayerAll), 0,
                                 kLayerModeCount - 1));
    m_layerValue.store(clampLayerValue(m_layerAxis.load(), Config::getInt(section, "layerValue", 64)));
    m_layerMin.store(clampLayerValue(m_layerAxis.load(), Config::getInt(section, "layerMin", 0)));
    m_layerMax.store(clampLayerValue(m_layerAxis.load(), Config::getInt(section, "layerMax", 0)));
    m_layerVersion.fetch_add(1, std::memory_order_relaxed);

    m_diffBoxes.store(Config::getBool(section, "diffBoxes", true));
    m_diffXray.store(Config::getBool(section, "diffXray", true));
    m_ghostOverMismatch.store(Config::getBool(section, "ghostOverMismatch", true));
    m_boxAlpha.store(std::clamp(Config::getInt(section, "boxOpacity", 35), kMinAlpha, kMaxAlpha));

    m_posX.store(std::clamp(Config::getInt(section, "posX", 0), kMinXZ, kMaxXZ));
    m_posY.store(std::clamp(Config::getInt(section, "posY", 0), kMinY, kMaxY));
    m_posZ.store(std::clamp(Config::getInt(section, "posZ", 0), kMinXZ, kMaxXZ));

    m_selected = -1;
    if (const auto at = section.find("file"); at != section.end() && at->is_string()) {
        m_pendingFile = at->get<std::string>();
    }

    m_pendingBlueprints.clear();
    if (const auto at = section.find("blueprints"); at != section.end() && at->is_array()) {
        for (const auto& one : *at) {
            if (!one.is_object()) {
                continue;
            }
            Saved saved;
            const auto nameAt = one.find("name");
            if (nameAt == one.end() || !nameAt->is_string()) {
                continue;
            }
            saved.name = toUtf16(nameAt->get<std::string>());
            if (saved.name.empty()) {
                continue;
            }
            saved.visible = Config::getBool(one, "visible", false);
            saved.x = std::clamp(Config::getInt(one, "x", 0), kMinXZ, kMaxXZ);
            saved.y = std::clamp(Config::getInt(one, "y", 0), kMinY, kMaxY);
            saved.z = std::clamp(Config::getInt(one, "z", 0), kMinXZ, kMaxXZ);
            saved.rotation = rotation::normalize(Config::getInt(one, "rotation", 0));
            m_pendingBlueprints.push_back(std::move(saved));
        }
    } else if (!m_pendingFile.empty()) {
        Saved saved;
        saved.name = toUtf16(m_pendingFile);
        saved.visible = true;
        saved.x = m_posX.load();
        saved.y = m_posY.load();
        saved.z = m_posZ.load();
        m_pendingBlueprints.push_back(std::move(saved));
    }
}

void Schematica::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    if (m_enabledAtShutdown.load(std::memory_order_relaxed)) {
        section["enabled"] = true;
    }

    section["opacity"] = m_alpha.load();
    section["diffBoxes"] = m_diffBoxes.load();
    section["diffXray"] = m_diffXray.load();
    section["ghostOverMismatch"] = m_ghostOverMismatch.load();
    section["boxOpacity"] = m_boxAlpha.load();
    section["pageKeys"] = m_pageKey.combo();
    section["layerUpKeys"] = m_layerUpKey.combo();
    section["layerDownKeys"] = m_layerDownKey.combo();
    section["layerMode"] = m_layerMode.load();
    section["layerAxis"] = m_layerAxis.load();
    section["layerValue"] = m_layerValue.load();
    section["layerMin"] = m_layerMin.load();
    section["layerMax"] = m_layerMax.load();
    section["posX"] = m_posX.load();
    section["posY"] = m_posY.load();
    section["posZ"] = m_posZ.load();

    std::string chosen;
    std::lock_guard<std::mutex> guard(m_filesMutex);
    if (m_selected >= 0 && static_cast<std::size_t>(m_selected) < m_files.size()) {
        chosen = toUtf8(m_files[static_cast<std::size_t>(m_selected)]);
    }
    section["file"] = chosen;

    nlohmann::json list = nlohmann::json::array();
    for (const auto& one : m_blueprints) {
        nlohmann::json item;
        item["name"] = toUtf8(one->name);
        item["visible"] = one->visible.load();
        item["x"] = one->posX.load();
        item["y"] = one->posY.load();
        item["z"] = one->posZ.load();
        item["rotation"] = one->rotation.load() & 3;
        list.push_back(std::move(item));
    }
    section["blueprints"] = std::move(list);
}

}
