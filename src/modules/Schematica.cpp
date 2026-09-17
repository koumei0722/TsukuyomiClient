#include "modules/Schematica.h"

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
#include "core/Paths.h"
#include "core/Perf.h"
#include "core/Strings.h"
#include "game/BlockRegistry.h"
#include "hooks/Detours.h"
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

int pitchFromStates(const std::vector<structure::StateValue>& states, bool fromEntity)
{
    for (const structure::StateValue& one : states) {
        if (one.tag == nbt::Tag::String) {
            if (one.name == "minecraft:facing_direction" || one.name == "facing") {
                if (one.text == "down") {
                    return 2;
                }
                if (one.text == "up") {
                    return 0;
                }
                return fromEntity ? 1 : 0;
            }
            continue;
        }
        if (one.name == "facing_direction" || one.name == "block_face"
            || one.name == "minecraft:block_face" || one.name == "facing") {
            if (one.number == 0) {
                return 2;
            }
            if (one.number == 1) {
                return 0;
            }
            if (one.number >= 2 && one.number <= 5) {
                return fromEntity ? 1 : 0;
            }
        }
    }
    return 0;
}

float yawFromStates(const std::vector<structure::StateValue>& states)
{
    for (const structure::StateValue& one : states) {
        if (one.tag == nbt::Tag::String) {
            if (one.name == "minecraft:cardinal_direction"
                || one.name == "minecraft:facing_direction" || one.name == "orientation"
                || one.name == "torch_facing_direction" || one.name == "facing") {
                if (one.text == "north") {
                    return 0.0F;
                }
                if (one.text == "south") {
                    return 180.0F;
                }
                if (one.text == "west") {
                    return 270.0F;
                }
                if (one.text == "east") {
                    return 90.0F;
                }
            }
            continue;
        }
        if (one.name == "facing_direction" || one.name == "coral_direction"
            || one.name == "block_face" || one.name == "minecraft:block_face"
            || one.name == "facing") {
            switch (one.number) {
            case 2:
                return 0.0F;
            case 3:
                return 180.0F;
            case 4:
                return 270.0F;
            case 5:
                return 90.0F;
            default:
                break;
            }
            continue;
        }
        if (one.name == "direction" || one.name == "weirdo_direction") {
            return static_cast<float>(one.number % 4) * 90.0F;
        }
        if (one.name == "ground_sign_direction" || one.name == "rotation") {
            const float deg = static_cast<float>(one.number % 16) * 22.5F;
            return deg >= 360.0F ? deg - 360.0F : deg;
        }
    }
    return 0.0F;
}

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
                    fresh->paletteBlocks = std::move(old->paletteBlocks);
                    fresh->paletteBase = old->paletteBase;
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
        } else {
            static std::atomic<unsigned> told{0};
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
        hooks::requestChunkRebuilds(ask);
    }
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
    const auto& src = m_blueprints[at]->materials;
    if (kinds != nullptr) {
        *kinds = src.size();
    }
    out.reserve(std::min(limit, src.size()));
    for (std::size_t i = 0; i < src.size() && i < limit; ++i) {
        MaterialRow row;
        row.name = toUtf16(src[i].first);
        row.count = src[i].second;
        if (const auto icon = m_iconIds.find(src[i].first); icon != m_iconIds.end()) {
            row.iconIdAux = icon->second;
            row.hasIcon = true;
        }
        if (const auto stack = m_stackSizes.find(src[i].first); stack != m_stackSizes.end()) {
            row.stackSize = stack->second;
        }
        out.push_back(std::move(row));
    }
    return out;
}

void Schematica::diffTally(std::size_t (&out)[6]) const
{
    for (std::size_t i = 0; i < 6; ++i) {
        out[i] = m_diffTally[i];
    }
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

std::uint64_t Schematica::packCell(std::int32_t x, std::int32_t y, std::int32_t z)
{
    const std::uint64_t ux = static_cast<std::uint32_t>(x) & 0x1FFFFFu;
    const std::uint64_t uy = static_cast<std::uint32_t>(y) & 0x1FFFFFu;
    const std::uint64_t uz = static_cast<std::uint32_t>(z) & 0x1FFFFFu;
    return (ux << 42) | (uy << 21) | uz;
}

void Schematica::loadVisible()
{
    const perf::Scope perfScope{perf::Slot::Load};
    std::vector<Blueprint*> want;
    {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        for (const auto& one : m_blueprints) {
            if (one->visible.load()) {
                want.push_back(one.get());
            } else if (one->ready) {
                one->loaded = structure::Structure{};
                one->paletteBlocks.clear();
                one->ready = false;
            }
        }
    }

    for (Blueprint* const one : want) {
        if (one->ready) {
            continue;
        }
        const std::filesystem::path path = paths::schematicsDir() / one->fileName;
        structure::LoadResult got = structure::loadFile(path);
        if (!got.ok()) {
            log().warn(L"Schematica: could not read {} ({})", one->name, toUtf16(got.why));
            one->loaded = structure::Structure{};
            one->ready = false;
            continue;
        }
        std::size_t solid = 0;
        std::vector<std::size_t> perEntry(got.value.palette.size(), 0);
        for (const std::int32_t entry : got.value.blocks) {
            if (entry >= 0 && static_cast<std::size_t>(entry) < got.value.palette.size()
                && got.value.palette[static_cast<std::size_t>(entry)].name
                       != "minecraft:air") {
                ++solid;
                ++perEntry[static_cast<std::size_t>(entry)];
            }
        }
        std::map<std::string, std::size_t> byName;
        for (std::size_t e = 0; e < perEntry.size(); ++e) {
            if (perEntry[e] != 0) {
                byName[got.value.palette[e].name] += perEntry[e];
            }
        }
        std::vector<std::pair<std::string, std::size_t>> materials(byName.begin(), byName.end());
        std::sort(materials.begin(), materials.end(),
                  [](const auto& a, const auto& b) {
                      return (a.second != b.second) ? (a.second > b.second) : (a.first < b.first);
                  });
        log().info(L"Schematica: loaded {} ({}x{}x{}, {} palette entries, {} solid blocks)",
                   one->name, got.value.sizeX, got.value.sizeY, got.value.sizeZ,
                   got.value.palette.size(), solid);
        one->sizeX.store(got.value.sizeX, std::memory_order_relaxed);
        one->sizeY.store(got.value.sizeY, std::memory_order_relaxed);
        one->sizeZ.store(got.value.sizeZ, std::memory_order_relaxed);
        one->solidCount.store(solid, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> guard(m_filesMutex);
            one->materials = std::move(materials);
        }
        one->loaded = std::move(got.value);
        one->ready = true;
    }

    resolvePalette();
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        rebuildCells();
    }
}

Schematica::Box Schematica::unionBox() const
{
    Box all;
    bool first = true;
    for (const Box& one : m_boxes) {
        if (first) {
            all = one;
            first = false;
            continue;
        }
        all.x0 = std::min(all.x0, one.x0);
        all.y0 = std::min(all.y0, one.y0);
        all.z0 = std::min(all.z0, one.z0);
        all.x1 = std::max(all.x1, one.x1);
        all.y1 = std::max(all.y1, one.y1);
        all.z1 = std::max(all.z1, one.z1);
    }
    return all;
}

void Schematica::rebuildCells()
{
    const perf::Scope perfScope{perf::Slot::Cells};
    m_cells.clear();
    m_cellAt.clear();
    m_boxes.clear();
    m_chunkCells.clear();

    std::vector<Blueprint*> live;
    {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        for (const auto& one : m_blueprints) {
            if (one->visible.load() && one->ready && one->loaded.valid()) {
                live.push_back(one.get());
            }
        }
    }
    if (live.empty()) {
        return;
    }

    std::size_t overlapped = 0;
    for (std::size_t which = 0; which < live.size(); ++which) {
        Blueprint& bp = *live[which];
        const std::int32_t x0 = bp.posX.load();
        const std::int32_t y0 = bp.posY.load();
        const std::int32_t z0 = bp.posZ.load();
        const int quarters = bp.rotation.load() & 3;
        {
            const rotation::Footprint foot =
                rotation::rotatedFootprint(quarters, x0, z0, bp.loaded.sizeX, bp.loaded.sizeZ);
            m_boxes.push_back(Box{foot.x0, y0, foot.z0, foot.x1, y0 + bp.loaded.sizeY, foot.z1});
        }

        const std::int32_t sizeY = bp.loaded.sizeY;
        const std::int32_t sizeZ = bp.loaded.sizeZ;
        const std::size_t total = bp.loaded.blocks.size();
        m_cells.reserve(m_cells.size() + total);
        m_cellAt.reserve(m_cellAt.size() + total);
        for (std::size_t at = 0; at < total; ++at) {
            const std::int32_t entry = bp.loaded.blocks[at];
            if (entry >= 0 && static_cast<std::size_t>(entry) >= bp.loaded.palette.size()) {
                continue;
            }
            const std::int32_t local = static_cast<std::int32_t>(at);
            const std::int32_t lz = local % sizeZ;
            const std::int32_t ly = (local / sizeZ) % sizeY;
            const std::int32_t lx = local / (sizeZ * sizeY);

            static const std::string kAirName{"minecraft:air"};
            const std::string& name =
                entry < 0 ? kAirName
                          : bp.loaded.palette[static_cast<std::size_t>(entry)].name;
            Cell cell;
            {
                std::int32_t rx = lx;
                std::int32_t rz = lz;
                rotation::rotateLocal(quarters, lx, lz, rx, rz);
                cell.x = x0 + rx;
                cell.y = y0 + ly;
                cell.z = z0 + rz;
            }
            cell.entry = (name == "minecraft:air")
                             ? kAirCell
                             : bp.paletteBase + static_cast<std::size_t>(entry);
            cell.owner = static_cast<int>(which);
            cell.name = &name;
            if (cell.entry != kAirCell && at < bp.loaded.blocks2.size()) {
                const std::int32_t second = bp.loaded.blocks2[at];
                if (second >= 0
                    && static_cast<std::size_t>(second) < bp.loaded.palette.size()
                    && bp.loaded.palette[static_cast<std::size_t>(second)].name
                           != "minecraft:air") {
                    cell.entry2 =
                        static_cast<std::int32_t>(bp.paletteBase)
                        + second;
                }
            }
            if (hooks::beModelsOn() && cell.entry != kAirCell
                && static_cast<std::size_t>(entry) < bp.loaded.palette.size()) {
                const auto& states = bp.loaded.palette[static_cast<std::size_t>(entry)].states;
                cell.yaw = yawFromStates(states);
                cell.pitch = pitchFromStates(states, false);
                const auto& fields = bp.loaded.entityAt(lx, ly, lz);
                if (!fields.empty()) {
                    const float beYaw = yawFromStates(fields);
                    const int bePitch = pitchFromStates(fields, true);
                    if (beYaw != 0.0F || bePitch != 0) {
                        cell.yaw = beYaw;
                        cell.pitch = bePitch;
                    }
                }
                if (quarters != 0) {
                    cell.yaw = std::fmod(cell.yaw + 90.0F * static_cast<float>(quarters), 360.0F);
                }
            }
            const std::uint64_t key = packCell(cell.x, cell.y, cell.z);
            if (m_cellAt.find(key) != m_cellAt.end()) {
                ++overlapped;
                continue;
            }
            m_cellAt.emplace(key, m_cells.size());
            m_chunkCells[packCell(cell.x >> 4, cell.y >> 4, cell.z >> 4)].push_back(
                static_cast<std::uint32_t>(m_cells.size()));
            m_cells.push_back(cell);
        }
    }
    m_region = unionBox();
    log().info(L"Schematica: {} blueprint(s) / {} cell(s){}",
               live.size(),
               m_cells.size(),
               overlapped != 0 ? std::format(L" ({} dropped as overlapping)", overlapped)
                               : std::wstring{});
}

void Schematica::resolvePalette()
{
    std::vector<Blueprint*> live;
    {
        std::lock_guard<std::mutex> guard(m_filesMutex);
        for (const auto& one : m_blueprints) {
            if (one->visible.load() && one->ready) {
                live.push_back(one.get());
            }
        }
    }

    std::vector<std::string> wanted;
    for (const Blueprint* const bp : live) {
        for (const structure::PaletteEntry& entry : bp->loaded.palette) {
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
    if (std::find(wanted.begin(), wanted.end(), std::string{kBoxBlock}) == wanted.end()) {
        wanted.emplace_back(kBoxBlock);
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
        for (const Blueprint* const bp : live) {
            for (const structure::PaletteEntry& entry : bp->loaded.palette) {
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
        for (const Blueprint* const bp : live) {
            for (const structure::PaletteEntry& entry : bp->loaded.palette) {
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
                        if (static std::atomic<int> told{0};
                            told.fetch_add(1, std::memory_order_relaxed) < 20) {
                        }
                        size = guess;
                    } else if (guess != size) {
                        if (static std::atomic<int> told2{0};
                            told2.fetch_add(1, std::memory_order_relaxed) < 20) {
                        }
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
    const auto box = table.find(kBoxBlock);

    std::vector<const void*> merged;
    std::vector<std::string> mergedKeys;
    std::size_t withStates = 0;
    std::size_t withYaw = 0;
    std::size_t changed = 0;
    std::size_t unresolvedTotal = 0;
    std::size_t slots = 0;
    std::size_t rotatedPrints = 0;
    std::size_t rotatedTurned = 0;
    std::size_t rotatedSame = 0;
    std::size_t rotatedFailed = 0;
    for (Blueprint* const bp : live) {
        const structure::Structure& loaded = bp->loaded;
        const int quarters = bp->rotation.load() & 3;
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
            float yaw = yawFromStates(states);
            if (quarters != 0) {
                yaw = std::fmod(yaw + 90.0F * static_cast<float>(quarters), 360.0F);
            }
            if (yaw != 0.0F) {
                blocks::noteGhostYaw(resolved[i], yaw);
                ++withYaw;
            }
        }
        slots += resolved.size();
        bp->paletteBase = merged.size();
        merged.insert(merged.end(), resolved.begin(), resolved.end());
        for (const structure::PaletteEntry& entry : loaded.palette) {
            mergedKeys.push_back(quarters != 0
                                     ? entry.key() + " (rot " + std::to_string(quarters * 90) + ")"
                                     : entry.key());
        }
        bp->paletteBlocks = std::move(resolved);
    }

    std::lock_guard<std::mutex> guard(m_mutex);
    m_air = air == table.end() ? nullptr : air->second;
    m_ghost = ghost == table.end() ? nullptr : ghost->second;
    m_poke = poke == table.end() ? m_ghost : poke->second;
    m_paletteBlocks = std::move(merged);
    m_paletteKeys = std::move(mergedKeys);
    m_palette = std::move(table);

    blocks::setAirBlock(m_air);

    blocks::setBoxBlock(box == table.end() ? nullptr : box->second);
    if (box == table.end()) {
        log().warn(L"Schematica: could not resolve {} - no color boxes will be shown",
                   toUtf16(kBoxBlock));
    }

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
}

void Schematica::shutdown()
{
    m_shuttingDown.store(true, std::memory_order_relaxed);
    if (!enabled()) {
        return;
    }
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
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        restoreGhostBlockEntities();
    }
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

void Schematica::noteActorCellTaken(std::int32_t x, std::int32_t y, std::int32_t z)
{
    if (m_actorLive.load(std::memory_order_relaxed) == 0) {
        return;
    }
    if (!blocks::ghostInside(x, y, z)) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(m_takenMutex);
        if (m_takenCells.size() >= kTakenLimit) {
            m_takenOverflow = true;
        } else {
            m_takenCells.push_back(blockwrite::BlockPos{x, y, z});
        }
    }
    m_takenPending.store(true, std::memory_order_relaxed);
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
    if (m_pageKey.triggered() && input::isInGameplay()) {
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

static bool boxSelfDrawAlways()
{
    static int mode = 0;
    if (mode == 0) {
        std::error_code ec;
        mode = std::filesystem::exists(paths::dataDir() / L"diag-box-self.txt", ec) ? 1 : 2;
    }
    return mode == 1;
}

static bool noGhostActors()
{
    if (!hooks::beModelsOn()) {
        return true;
    }
    static int mode = 0;
    if (mode == 0) {
        std::error_code ec;
        const bool none = std::filesystem::exists(paths::dataDir() / L"diag-noactor.txt", ec);
        mode = none ? 1 : 2;
    }
    return mode == 1;
}

static bool keepGhostActors()
{
    static int mode = 0;
    if (mode == 0) {
        std::error_code ec;
        mode = std::filesystem::exists(paths::dataDir() / L"diag-keepactor.txt", ec) ? 1 : 2;
    }
    return mode == 1;
}

void Schematica::onPlayerViewUpdate()
{
    perf::endFrame();
    perf::maybeReport();
    const perf::Scope perfAll{perf::Slot::Schematica};

    if (const std::uint64_t generation = blockwrite::regionGeneration();
        generation != m_regionGeneration) {
        const bool first = (m_regionGeneration == 0);
        m_regionGeneration = generation;
        if (!first) {
            {
                const std::lock_guard<std::mutex> guard(m_mutex);
                m_actorPlaced.clear();
                syncActorLive();
                m_actorRegion = nullptr;
                m_placed.clear();
                m_ghostChunks.clear();
                m_learnedKeys.clear();
                m_chunkChanges.clear();
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
                       L"(generation {} / placing again after {} ms)",
                       generation,
                       kWorldSettleMs);
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
        loadVisible();
        m_drawnUpTo = 0;
        m_drawnPlaced = 0;
        m_waitedLogged = false;
        m_firstLogged = false;
        m_anchorLogged = false;
        m_reloadAsked = false;
        m_boxReloadTries = 0;
        m_boxReloadAt = 0;
        m_boxesPlacedLogged = false;
        m_boxesGaveUpLogged = false;
        m_boxesPlacedAt = 0;
        m_diffUpTo = 0;
        m_diffLaps = 0;
        m_diffLoggedAt = 0;
        for (std::size_t& one : m_diffTally) {
            one = 0;
        }
        m_diffActors.clear();
        m_lapTagChunks.clear();
        m_lapColorChunks.clear();
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_chunkChanges.clear();
        }
        hooks::clearChunkBoxTries();
        if (m_placed.empty()) {
            m_drawPending.store(true, std::memory_order_relaxed);
        } else {
            m_clearedUpTo = 0;
            m_clearPending.store(true, std::memory_order_relaxed);
        }
    }
    if (const unsigned long long at = m_posChangedAt.load(std::memory_order_relaxed);
        at != 0 && GetTickCount64() - at >= kPosSettleMs) {
        m_posChangedAt.store(0, std::memory_order_relaxed);
        m_cellsDirty.store(true, std::memory_order_relaxed);
        m_redrawPending.store(true, std::memory_order_relaxed);
    }
    if (const unsigned long long learned = hooks::lastModelLearnedAt();
        learned != 0 && learned != m_modelRedrawnAt && enabled()
        && GetTickCount64() - learned >= kModelSettleMs) {
        m_modelRedrawnAt = learned;
        m_redrawPending.store(true, std::memory_order_relaxed);
    }
    if (m_redrawPending.exchange(false, std::memory_order_relaxed) && enabled()) {
        m_clearedUpTo = 0;
        m_clearPending.store(true, std::memory_order_relaxed);
    }

    applyLayerFilter();

    {
        static int armed = 0;
        if (armed == 0) {
            std::error_code ec;
            armed = std::filesystem::exists(paths::dataDir() / L"diag-xray.txt", ec) ? 1 : 2;
        }
        if (armed == 1) {
            static bool was = false;
            const bool down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
            if (down && !was) {
                const bool now = !m_diffXray.load(std::memory_order_relaxed);
                m_diffXray.store(now, std::memory_order_relaxed);
                log().info(L"Schematica: x-ray mode {}", now ? L"ON" : L"OFF");
                m_boxModeChanged.store(true, std::memory_order_relaxed);
            }
            was = down;
        }
    }

    {
        static int armed = 0;
        if (armed == 0) {
            std::error_code ec;
            armed = std::filesystem::exists(paths::dataDir() / L"diag-boxes.txt", ec) ? 1 : 2;
        }
        if (armed == 1) {
            static bool was = false;
            const bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
            if (down && !was) {
                const bool now = !m_diffBoxes.load(std::memory_order_relaxed);
                m_diffBoxes.store(now, std::memory_order_relaxed);
                log().info(L"Schematica: color boxes {}", now ? L"ON" : L"OFF");
            }
            was = down;
        }
    }

    {
        const bool wantBoxes = enabled() && blocks::ghostOn()
                               && m_diffBoxes.load(std::memory_order_relaxed);
        const bool xray = m_diffXray.load(std::memory_order_relaxed);
        const bool selfDrawAlways = boxSelfDrawAlways() || worldmesh::active(xray);
        boxes::setStyle(selfDrawAlways ? wantBoxes : (wantBoxes && xray),
                        static_cast<float>(m_boxAlpha.load(std::memory_order_relaxed))
                            / 100.0F,
                        xray);
        const bool wantMesh = !selfDrawAlways && wantBoxes && !xray;
        if (wantMesh) {
            m_meshBoxesArmed.store(true, std::memory_order_relaxed);
        }
        if (blocks::meshBoxesOn() != wantMesh) {
            blocks::setMeshBoxes(wantMesh);
            m_boxModeChanged.store(true, std::memory_order_relaxed);
        }
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
        if (m_actorPending.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> guard(m_mutex);
            placeGhostBlockEntities();
            return;
        }
        if (blocks::ghostOn()) {
            if (m_rebuiltPending.exchange(false, std::memory_order_relaxed)) {
                reviewRebuiltChunks();
            }
            if (m_freedPending.exchange(false, std::memory_order_relaxed)) {
                restoreFreedCells();
            }
            if (m_takenPending.exchange(false, std::memory_order_relaxed)) {
                fixTakenActorCells();
            }
            {
                const unsigned long long now = GetTickCount64();
                if (now - m_learnAt >= kLearnMs) {
                    m_learnAt = now;
                    std::lock_guard<std::mutex> guard(m_mutex);
                    m_learnedLate += learnRegionSubChunks(true);
                    m_healAsked += healStaleChunks(now);
                }
            }
            {
                std::lock_guard<std::mutex> guard(m_mutex);
                diffStep();
                publishBoxesLive();
            }
            maybeRenudge();
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

    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_drawnUpTo == 0 && m_cellsDirty.exchange(false, std::memory_order_relaxed)) {
        rebuildCells();
    }
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
        m_actorCells.clear();

        blocks::setGhostRegion(m_anchorX, m_anchorY, m_anchorZ,
                               m_regionSizeX, m_regionSizeY, m_regionSizeZ,
                               static_cast<float>(m_alpha.load()) / 100.0F);
        blocks::setGhostPalette(m_paletteBlocks);

        learnRegionSubChunks(false);
    }

    const std::size_t total = m_cells.size();
    const std::size_t stop = std::min(total, m_drawnUpTo + kPerFrame);
    std::size_t placed = 0;
    const perf::Scope perfDraw{perf::Slot::Draw};

    blockwrite::beginPlacement(m_air);

    for (std::size_t at = m_drawnUpTo; at < stop; ++at) {

        const Cell& cell = m_cells[at];
        const blockwrite::BlockPos where{cell.x, cell.y, cell.z};

        if (cell.entry == kAirCell) {
            blocks::setGhostWantAir(where.x, where.y, where.z);
            continue;
        }
        const std::size_t entry = cell.entry;
        static const std::string kNoName;
        const std::string& name = cell.name != nullptr ? *cell.name : kNoName;
        const void* const block = blockFor(name, entry);
        if (block == nullptr) {
            continue;
        }

        {
            blocks::setGhostWantCell(where.x, where.y, where.z, entry);
            if (cell.entry2 >= 0) {
                blocks::setGhostWantCell(where.x, where.y, where.z,
                                         static_cast<std::size_t>(cell.entry2), 1);
            }

            if (const void* const real = blockwrite::readWorldAt(where.x, where.y, where.z);
                real != nullptr && real != m_air) {
                continue;
            }
            blocks::setGhostCellBlock(where.x, where.y, where.z, entry);
            if (cell.yaw != 0.0F || cell.pitch != 0) {
                blocks::setGhostCellOrient(where.x, where.y, where.z, cell.yaw, cell.pitch);
            }
            if (cell.entry2 >= 0) {
                blocks::setGhostCellBlock(where.x, where.y, where.z,
                                          static_cast<std::size_t>(cell.entry2), 1);
            }
            if (blocks::hasBlockEntity(block)) {
                m_actorCells.emplace_back(where, block);
            }
            ++placed;
            if (!m_firstLogged) {
                m_firstLogged = true;
                log().info(L"Schematica: first block {} at {} {} {} -> drawing only",
                           toUtf16(name),
                           where.x,
                           where.y,
                           where.z);
            }
        }
    }

    blockwrite::endPlacement();

    m_drawnUpTo = stop;
    const std::size_t placedTotal = m_drawnPlaced + placed;
    if (m_drawnUpTo >= total) {
        m_drawPending.store(false, std::memory_order_relaxed);
        rebuildGhostChunkList();
        std::size_t poked = dirtyChunks(1);
        poked += dirtyChunks(2);
        m_prunedUpTo = 0;
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

Schematica::ActorPlace Schematica::placeOneGhostActor(void* region,
                                                      const blockwrite::BlockPos& where,
                                                      const void* block)
{
    if (!blocks::ghostCell(where.x, where.y, where.z)) {
        return ActorPlace::NoGhost;
    }
    if (!keepGhostActors() && hooks::modelBoxedForBlock(block)) {
        return ActorPlace::Learned;
    }
    for (const PlacedActor& one : m_actorPlaced) {
        if (one.at.x == where.x && one.at.y == where.y && one.at.z == where.z) {
            return ActorPlace::Already;
        }
    }
    blockwrite::StorageSpot spot;
    if (!blockwrite::placeGhostActor(region, where, block, m_air, &spot)) {
        return ActorPlace::Refused;
    }
    if (spot.subChunk == nullptr) {
        blockwrite::placeAtRegion(region, where, m_air);
        return ActorPlace::Leftover;
    }
    m_actorPlaced.push_back(PlacedActor{where, block});
    syncActorLive();
    m_actorRegion = region;
    return ActorPlace::Placed;
}

void Schematica::placeGhostBlockEntities()
{
    const perf::Scope perfScope{perf::Slot::PlaceActors};
    m_actorPending.store(false, std::memory_order_relaxed);
    if (noGhostActors()) {
        return;
    }
    if (m_actorCells.empty()) {
        return;
    }
    void* const region = blockwrite::renderRegion();
    if (region == nullptr) {
        m_actorPending.store(true, std::memory_order_relaxed);
        return;
    }
    if (m_air == nullptr) {
        log().warn(L"Schematica: not placing block entities (there is no air block object)");
        return;
    }
    std::size_t placed = 0;
    std::size_t dropped = 0;
    std::size_t refused = 0;
    std::size_t leftover = 0;
    std::size_t already = 0;
    std::size_t learned = 0;
    for (const auto& [where, block] : m_actorCells) {
        switch (placeOneGhostActor(region, where, block)) {
        case ActorPlace::Placed:
            ++placed;
            break;
        case ActorPlace::NoGhost:
            ++dropped;
            break;
        case ActorPlace::Refused:
            ++refused;
            break;
        case ActorPlace::Leftover:
            ++leftover;
            break;
        case ActorPlace::Already:
            ++already;
            break;
        case ActorPlace::Learned:
            ++learned;
            break;
        }
    }
}

std::size_t Schematica::forgetPlacedActors(
    const std::vector<std::pair<blockwrite::BlockPos, const void*>>& cells)
{
    std::size_t gone = 0;
    for (const auto& [where, block] : cells) {
        (void)block;
        const auto tail = std::remove_if(
            m_actorPlaced.begin(), m_actorPlaced.end(),
            [&where](const PlacedActor& one) {
                return one.at.x == where.x && one.at.y == where.y
                       && one.at.z == where.z;
            });
        gone += static_cast<std::size_t>(std::distance(tail, m_actorPlaced.end()));
        m_actorPlaced.erase(tail, m_actorPlaced.end());
    }
    syncActorLive();
    return gone;
}

void Schematica::restoreFreedActors(
    const std::vector<std::pair<blockwrite::BlockPos, const void*>>& cells)
{
    if (noGhostActors()) {
        return;
    }
    void* const region = blockwrite::renderRegion();
    if (region == nullptr || m_air == nullptr) {
        log().warn(L"Schematica: cannot put the {} broken ghost cell(s) back (renderer region "
                   L"{} / air {})",
                   cells.size(),
                   region != nullptr,
                   m_air != nullptr);
        return;
    }
    if (m_actorRegion != nullptr && region != m_actorRegion) {
        m_actorPlaced.clear();
        syncActorLive();
        m_actorRegion = nullptr;
    }
    forgetPlacedActors(cells);
    std::size_t placed = 0;
    std::size_t dropped = 0;
    std::size_t refused = 0;
    std::size_t leftover = 0;
    std::size_t already = 0;
    std::size_t learned = 0;
    for (const auto& [where, block] : cells) {
        switch (placeOneGhostActor(region, where, block)) {
        case ActorPlace::Placed:
            ++placed;
            break;
        case ActorPlace::NoGhost:
            ++dropped;
            break;
        case ActorPlace::Refused:
            ++refused;
            break;
        case ActorPlace::Leftover:
            ++leftover;
            break;
        case ActorPlace::Already:
            ++already;
            break;
        case ActorPlace::Learned:
            ++learned;
            break;
        }
    }
}

void Schematica::fixTakenActorCells()
{
    const perf::Scope perfScope{perf::Slot::FixTaken};
    std::vector<blockwrite::BlockPos> cells;
    bool all = false;
    {
        std::lock_guard<std::mutex> guard(m_takenMutex);
        cells.swap(m_takenCells);
        all = m_takenOverflow;
        m_takenOverflow = false;
    }
    if (cells.empty() && !all) {
        return;
    }
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_cells.empty() || !blocks::ghostOn() || m_actorPlaced.empty()) {
        return;
    }
    void* const region = blockwrite::renderRegion();
    if (region == nullptr || m_air == nullptr) {
        return;
    }
    if (m_actorRegion != nullptr && region != m_actorRegion) {
        m_actorPlaced.clear();
        syncActorLive();
        m_actorRegion = nullptr;
        return;
    }
    if (all) {
        cells.clear();
        cells.reserve(m_actorPlaced.size());
        for (const PlacedActor& one : m_actorPlaced) {
            cells.push_back(one.at);
        }
    }
    std::size_t fixed = 0;
    std::size_t notOurs = 0;
    std::size_t empty = 0;
    std::size_t sameKind = 0;
    std::size_t refused = 0;
    std::size_t lost = 0;
    for (const blockwrite::BlockPos& at : cells) {
        const auto found = std::find_if(
            m_actorPlaced.begin(), m_actorPlaced.end(), [&at](const PlacedActor& one) {
                return one.at.x == at.x && one.at.y == at.y && one.at.z == at.z;
            });
        if (found == m_actorPlaced.end()) {
            ++notOurs;
            continue;
        }
        const void* const ghost = found->block;
        const void* const real = blockwrite::readWorldAt(at.x, at.y, at.z);
        if (real == nullptr || real == m_air) {
            ++empty;
            continue;
        }
        if (const void* const legacy = blocks::legacyOfFast(real);
            legacy != nullptr && legacy == blocks::legacyOfFast(ghost)) {
            m_actorPlaced.erase(found);
            syncActorLive();
            ++sameKind;
            continue;
        }
        bool wroteGhost = false;
        if (!blocks::hasBlockEntityFast(real)) {
            if (ghost == nullptr || !blockwrite::placeAtRegion(region, at, ghost)) {
                ++refused;
                continue;
            }
            wroteGhost = true;
        }
        if (!blockwrite::placeAtRegion(region, at, m_air)) {
            if (wroteGhost) {
                blockwrite::placeAtRegion(region, at, real);
            }
            ++refused;
            continue;
        }
        if (!blockwrite::placeAtRegion(region, at, real)) {
            log().warn(L"Schematica: could not write the real block back ({},{},{})",
                       at.x,
                       at.y,
                       at.z);
            ++lost;
        }
        blocks::dropGhostCell(at.x, at.y, at.z);
        blocks::noteWorldBlockAt(at.x, at.y, at.z, real);
        m_actorPlaced.erase(found);
        syncActorLive();
        ++fixed;
    }
    if (fixed != 0 || sameKind != 0 || refused != 0 || lost != 0) {
    }
}

void Schematica::restoreGhostBlockEntities()
{
    m_actorPending.store(false, std::memory_order_relaxed);
    if (m_actorPlaced.empty()) {
        return;
    }
    void* const region = blockwrite::renderRegion();
    if (region != nullptr && region != m_actorRegion) {
        m_actorPlaced.clear();
        syncActorLive();
        m_actorRegion = nullptr;
        return;
    }
    if (region == nullptr || m_air == nullptr) {
        log().warn(L"Schematica: could not put {} block entit(ies) back (the renderer region "
                   L"is not there yet)",
                   m_actorPlaced.size());
        return;
    }
    std::size_t back = 0;
    std::size_t kept = 0;
    std::size_t fallback = 0;
    for (const PlacedActor& one : m_actorPlaced) {
        const bool ghost = blocks::ghostCell(one.at.x, one.at.y, one.at.z);
        const void* const real = blockwrite::readWorldAt(one.at.x, one.at.y, one.at.z);
        if (!ghost || (real != nullptr && real != m_air)) {
            ++kept;
            continue;
        }
        if (blockwrite::removeGhostActor(region, one.at, one.block, m_air)) {
            ++back;
        } else if (blockwrite::placeAtRegion(region, one.at, m_air)) {
            ++back;
            ++fallback;
        }
    }
    log().info(L"Schematica: put {} block entit(ies) back", back);
    m_actorPlaced.clear();
    syncActorLive();
    m_actorRegion = nullptr;
}

void Schematica::diffCell(std::size_t at, bool early)
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
    }
    if (kind == blocks::DiffKind::UnknownWorld) {
        return;
    }
    if (kind == blocks::DiffKind::Missing) {
        if (blocks::restoreGhostCellFromWant(wx, wy, wz)) {
            noteChunkChange(wx, wy, wz);
            if (!early) {
                ++m_diffRestored;
                noteLapChunk(m_lapTagChunks, wx, wy, wz);
            }
            if (want != nullptr && blocks::hasBlockEntity(want)) {
                m_diffActors.emplace_back(blockwrite::BlockPos{wx, wy, wz}, want);
            }
        }
    }
    if (blocks::setDiffCell(wx, wy, wz, blocks::colorOfDiffKind(kind))) {
        if (blocks::meshBoxesOn()) {
            noteChunkChange(wx, wy, wz);
        }
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
    const unsigned long long now = GetTickCount64();
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
    if (!m_diffActors.empty()) {
        restoreFreedActors(m_diffActors);
        m_diffActors.clear();
    }
    if (m_boxReloadTries < kBoxReloadTries && blocks::meshBoxesOn() && enabled()
        && blocks::ghostOn() && !m_shuttingDown.load(std::memory_order_relaxed)
        && (m_diffTally[1] + m_diffTally[2] + m_diffTally[3]) > 0
        && blocks::ghostHitCount(blocks::GhostHook::Box) == 0
        && (m_boxReloadAt == 0 || now - m_boxReloadAt >= kBoxReloadRetryMs)) {
        if (FreeCamera::instance().borrowForChunkReload()) {
            m_boxReloadAt = now;
            ++m_boxReloadTries;
        }
    }

    if (blocks::meshBoxesOn() && enabled() && blocks::ghostOn()) {
        const std::size_t placed = blocks::ghostHitCount(blocks::GhostHook::Box);
        const PlayerView view = GameData::instance().playerView();
        const bool got = GameData::instance().hasPlayerView();
        const float px = view.x;
        const float py = view.y;
        const float pz = view.z;
        if (m_boxesPlacedAt == 0) {
            m_boxesPlacedAt = now;
        }
        const bool waitedEnough = now - m_boxesPlacedAt >= kBoxesPlacedWaitMs;
        if (placed != 0 && !m_boxesPlacedLogged && (got || waitedEnough)) {
            m_boxesPlacedLogged = true;
        } else if (placed == 0 && m_boxReloadTries >= kBoxReloadTries
                   && !m_boxesGaveUpLogged && (got || waitedEnough)) {
            m_boxesGaveUpLogged = true;
            log().warn(L"Schematica: no color box landed after rebuilding the chunks {} times "
                       L"(blueprint ({},{},{}) / player ({},{},{}) / cyan {} red {} orange {})",
                       m_boxReloadTries,
                       m_anchorX,
                       m_anchorY,
                       m_anchorZ,
                       got ? static_cast<int>(px) : -9999,
                       got ? static_cast<int>(py) : -9999,
                       got ? static_cast<int>(pz) : -9999,
                       m_diffTally[1],
                       m_diffTally[2],
                       m_diffTally[3]);
        }
    }

    const bool boxesMoved =
        (m_diffChanged > 0 || m_diffDropped > 0 || m_diffRestored > 0);
    m_diffDropped = 0;
    m_diffRestored = 0;
    m_diffChanged = 0;

    if (boxesMoved) {
        publishBoxes(false);
    }
    for (std::size_t& one : m_diffTally) {
        one = 0;
    }
    m_diffWater = 0;
    m_diffWaterUnknown = 0;
    m_diffWorldWater = 0;
    m_diffWantWater = 0;
}

void Schematica::publishBoxesLive()
{
    const std::size_t wrote = blocks::takeDiffCellChanges();
    if (wrote != 0) {
        if (!m_boxLiveDirty) {
            m_boxLiveSince = GetTickCount64();
        }
        m_boxLiveDirty = true;
        m_boxLiveCells += wrote;
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
    m_boxLiveCells = 0;
    publishBoxes(false, true);
}

void Schematica::publishBoxes(bool force, bool changed)
{
    const perf::Scope perfScope{perf::Slot::Publish};
    if (!force && !boxes::boxesOn()) {
        ++m_boxSkipped;
        return;
    }
    const std::size_t now = m_diffTally[1] * 1000003u + m_diffTally[2] * 1009u
                            + m_diffTally[3];
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
    blocks::collectDiffBoxes(list, kBoxLimit, &dropped);
    if (dropped != 0) {
        log().warn(L"Schematica: too many boxes, dropped {} (limit {})", dropped, kBoxLimit);
    }
    boxes::setBoxes(std::move(list));
}

void Schematica::pruneStep()
{
    const perf::Scope perfScope{perf::Slot::Prune};
    const std::size_t total = m_cells.size();
    if (m_air == nullptr || total == 0) {
        m_prunePending.store(false, std::memory_order_relaxed);
        return;
    }
    const std::size_t stop = std::min(total, m_prunedUpTo + kPrunePerFrame);
    for (std::size_t at = m_prunedUpTo; at < stop; ++at) {
        const Cell& cell = m_cells[at];
        if (cell.entry == kAirCell) {
            continue;
        }
        const std::int32_t wx = cell.x;
        const std::int32_t wy = cell.y;
        const std::int32_t wz = cell.z;

        const void* const real = blockwrite::readWorldAt(wx, wy, wz);
        if (real != nullptr && real != m_air) {
            blocks::dropGhostCell(wx, wy, wz);
            ++m_prunedDropped;
        }
    }
    m_prunedUpTo = stop;
    if (m_prunedUpTo < total) {
        return;
    }
    m_prunePending.store(false, std::memory_order_relaxed);
    m_actorPending.store(true, std::memory_order_relaxed);
    std::size_t poked = m_prunedDropped > 0 ? dirtyChunks(4) : 0;
    rebuildGhostChunkList();
    poked += dirtyChunks(5);
    if (!m_reloadAsked) {
        m_reloadAsked = true;
        FreeCamera::instance().borrowForChunkReload();
    }
    log().info(L"Schematica: dropped {} cell(s) that are already filled ({} chunk(s) asked "
               L"again / {} chunk(s) remembered)",
               m_prunedDropped,
               poked,
               blockwrite::knownSubChunkCount());
    m_prunedDropped = 0;
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

void Schematica::noteChunkRebuilt(std::int32_t x, std::int32_t y, std::int32_t z)
{
    static std::atomic<int> told{0};
    const bool on = enabled();
    if (!on) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(m_rebuiltMutex);
        if (!m_rebuiltChunks.empty()) {
            const blockwrite::BlockPos& last = m_rebuiltChunks.back();
            if (last.x == x && last.y == y && last.z == z) {
                return;
            }
        }
        if (m_rebuiltChunks.size() >= kRebuiltLimit) {
            return;
        }
        m_rebuiltChunks.push_back(blockwrite::BlockPos{x, y, z});
    }
    m_rebuiltPending.store(true, std::memory_order_relaxed);
}

void Schematica::reviewRebuiltChunks()
{
    const perf::Scope perfScope{perf::Slot::Review};
    if (!hooks::beModelsOn()) {
        std::lock_guard<std::mutex> guard(m_rebuiltMutex);
        m_rebuiltChunks.clear();
        return;
    }
    std::vector<blockwrite::BlockPos> chunks;
    {
        std::lock_guard<std::mutex> guard(m_rebuiltMutex);
        chunks.swap(m_rebuiltChunks);
    }
    if (chunks.empty()) {
        return;
    }
    std::vector<blockwrite::BlockPos> cells;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (m_cells.empty() || !blocks::ghostOn()) {
            return;
        }
        static const std::string kNoName;
        for (const Cell& cell : m_cells) {
            if (cell.entry == kAirCell) {
                continue;
            }
            const std::int32_t cx = cell.x >> 4 << 4;
            const std::int32_t cy = cell.y >> 4 << 4;
            const std::int32_t cz = cell.z >> 4 << 4;
            bool hit = false;
            for (const blockwrite::BlockPos& one : chunks) {
                if (one.x == cx && one.y == cy && one.z == cz) {
                    hit = true;
                    break;
                }
            }
            if (!hit) {
                continue;
            }
            const void* const block =
                blockFor(cell.name != nullptr ? *cell.name : kNoName, cell.entry);
            if (block == nullptr || !blocks::hasBlockEntity(block)) {
                continue;
            }
            cells.push_back(blockwrite::BlockPos{cell.x, cell.y, cell.z});
        }
    }
    if (cells.empty()) {
        static std::atomic<int> toldEmpty{0};
        if (toldEmpty.fetch_add(1, std::memory_order_relaxed) < 12) {
            std::wstring got;
            for (const blockwrite::BlockPos& one : chunks) {
                got += std::format(L"({},{},{}) ", one.x, one.y, one.z);
            }
            std::wstring want;
            {
                std::lock_guard<std::mutex> guard(m_mutex);
                std::vector<blockwrite::BlockPos> seen;
                for (const Cell& c : m_cells) {
                    if (c.entry == kAirCell) {
                        continue;
                    }
                    const blockwrite::BlockPos o{c.x >> 4 << 4, c.y >> 4 << 4,
                                                 c.z >> 4 << 4};
                    bool dup = false;
                    for (const blockwrite::BlockPos& s : seen) {
                        if (s.x == o.x && s.y == o.y && s.z == o.z) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) {
                        seen.push_back(o);
                    }
                }
                for (const blockwrite::BlockPos& o : seen) {
                    want += std::format(L"({},{},{}) ", o.x, o.y, o.z);
                }
            }
        }
        return;
    }
    {
        std::lock_guard<std::mutex> guard(m_freedMutex);
        for (const blockwrite::BlockPos& one : cells) {
            if (m_freedCells.size() >= kFreedLimit) {
                m_redrawPending.store(true, std::memory_order_relaxed);
                break;
            }
            m_freedCells.push_back(one);
        }
    }
    m_freedPending.store(true, std::memory_order_relaxed);
    static std::atomic<int> told{0};
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
    std::vector<std::pair<blockwrite::BlockPos, const void*>> actors;
    for (const blockwrite::BlockPos& one : cells) {
        const auto found = m_cellAt.find(packCell(one.x, one.y, one.z));
        if (found == m_cellAt.end() || found->second >= m_cells.size()) {
            continue;
        }
        const Cell& cell = m_cells[found->second];
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
        if (block != nullptr && blocks::hasBlockEntity(block)) {
            actors.emplace_back(one, block);
        }
        if (blocks::ghostCell(one.x, one.y, one.z)) {
            ++already;
            if (block != nullptr && blocks::hasBlockEntity(block)) {
                ++needRebuild;
            }
            continue;
        }
        blocks::setGhostCellBlock(one.x, one.y, one.z, entry);
        if (found->second < m_cells.size()) {
            const Cell& c = m_cells[found->second];
            if (c.yaw != 0.0F || c.pitch != 0) {
                blocks::setGhostCellOrient(one.x, one.y, one.z, c.yaw, c.pitch);
            }
        }
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
    if (!actors.empty()) {
        restoreFreedActors(actors);
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
    std::unordered_set<std::uint64_t> missedColumns;
    for (const blockwrite::BlockPos& chunk : regionChunkList()) {
        const std::uint64_t column =
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk.x)) << 32)
            | static_cast<std::uint32_t>(chunk.z);
        const std::uint64_t chunkKey = packCell(chunk.x >> 4, chunk.y >> 4, chunk.z >> 4);
        if (missedColumns.find(column) != missedColumns.end()) {
            blockwrite::forgetSubChunkAt(chunk.x, chunk.y, chunk.z);
            m_learnedKeys.erase(chunkKey);
            continue;
        }
        void* const sub = blockwrite::findSubChunk(region, chunk.x, chunk.y, chunk.z);
        if (sub == nullptr) {
            if (blockwrite::lastFindWasMissingChunk()) {
                missedColumns.insert(column);
            }
            blockwrite::forgetSubChunkAt(chunk.x, chunk.y, chunk.z);
            m_learnedKeys.erase(chunkKey);
            continue;
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
    return learned;
}

void Schematica::rebuildGhostChunkList()
{
    std::vector<blockwrite::BlockPos> next;
    if (blocks::ghostOn()) {
        const bool boxes = blocks::meshBoxesOn();
        for (const blockwrite::BlockPos& at : regionChunkList()) {
            if (boxes || blocks::ghostSubChunkOccupied(at.x, at.y, at.z)) {
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
    const bool hadMeshBoxes = m_meshBoxesArmed.load(std::memory_order_relaxed);
    restoreGhostBlockEntities();
    blocks::clearGhostRegion();
    hooks::clearStorageMarks();
    if (hadMeshBoxes && hadDrawing && !hadGhosts) {
        m_ghostChunks = regionChunkList();
    }
    if (hadDrawing) {
        const std::size_t poked = dirtyChunks(8);
        if (poked > 0) {
            log().info(L"Schematica: cleared the drawing-only blocks (poked {} chunks)", poked);
        }
    }
    if (m_placed.empty() && (hadGhosts || hadMeshBoxes) && m_air != nullptr
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
    m_actorCells.clear();
    m_diffActors.clear();
    m_clearedUpTo = 0;
    m_prunePending.store(false, std::memory_order_relaxed);
    m_prunedUpTo = 0;
    m_prunedDropped = 0;
    m_clearPending.store(false, std::memory_order_relaxed);

    if (enabled()) {
        m_drawnUpTo = 0;
        m_drawnPlaced = 0;
        m_firstLogged = false;
        m_drawPending.store(true, std::memory_order_relaxed);
        return;
    }

    if ((hadGhosts || hadMeshBoxes) && !m_shuttingDown.load(std::memory_order_relaxed)) {
        FreeCamera::instance().borrowForChunkReload();
    }
    if (!enabled()) {
        m_meshBoxesArmed.store(false, std::memory_order_relaxed);
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
        open.value = [] { return std::wstring(L"Open"); };
        children.push_back(std::move(open));
    }

    children.push_back(menu::keybind(
        L"Page key", [this] { return m_pageKey.combo(); },
        [this](std::vector<int> combo) {
            m_pageKey.set(std::move(combo));
            log().info(L"Schematica: page key set to {}", m_pageKey.name());
        }));

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
                size_t at = 0;
                while (at < typed.size() && typed[at] == L' ') {
                    ++at;
                }
                const size_t begin = at;
                if (at < typed.size() && (typed[at] == L'-' || typed[at] == L'+')) {
                    ++at;
                }
                const size_t digits = at;
                while (at < typed.size() && typed[at] >= L'0' && typed[at] <= L'9') {
                    ++at;
                }
                if (at == digits) {
                    return;
                }
                const size_t last = at;
                while (at < typed.size() && typed[at] == L' ') {
                    ++at;
                }
                if (at != typed.size()) {
                    return;
                }
                int got = 0;
                try {
                    got = std::stoi(typed.substr(begin, last - begin));
                } catch (const std::exception&) {
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
                std::size_t pos = 0;
                while (pos < typed.size() && typed[pos] == L' ') {
                    ++pos;
                }
                const std::size_t begin = pos;
                if (pos < typed.size() && (typed[pos] == L'-' || typed[pos] == L'+')) {
                    ++pos;
                }
                const std::size_t digits = pos;
                while (pos < typed.size() && typed[pos] >= L'0' && typed[pos] <= L'9') {
                    ++pos;
                }
                if (pos == digits) {
                    return;
                }
                const std::size_t last = pos;
                while (pos < typed.size() && typed[pos] == L' ') {
                    ++pos;
                }
                if (pos != typed.size()) {
                    return;
                }
                try {
                    setBlueprintPos(static_cast<std::size_t>(at), axis,
                                    std::stoi(typed.substr(begin, last - begin)));
                } catch (const std::exception&) {
                    return;
                }
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
