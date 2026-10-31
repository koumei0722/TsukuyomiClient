#include "game/SchematicWorld.h"

#include "game/BlockRegistry.h"
#include "game/BlockWrite.h"
#include "game/EasyPlaceLogic.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <set>
#include <unordered_set>

namespace tsukuyomi::schematicworld {
namespace {
using namespace schematic;
constexpr unsigned kCompareBudgetSweepUs = 8000, kCompareBudgetSteadyUs = 2000;
constexpr unsigned kCompareDirtyPerFrame = 4;
constexpr unsigned long long kLearnMs = 1000, kSnapshotMinMs = 50, kWorldSettleMs = 4000;

struct SectionLive {
    std::shared_ptr<const WantSection> want;
    bool wantBuilt = false;
    std::shared_ptr<const DiffSection> diff;
    std::unique_ptr<DiffSection> building;
    std::uint32_t compareCursor = 0;
    void* subChunk = nullptr;
    void* chunk = nullptr;
    bool outsideWorld = false, inRange = false, needCompare = false, dirty = false;
    bool emptySub = false;
    std::uint64_t generation = 0;
};
std::atomic<SectionIndex*> g_indexPtr{nullptr};
std::atomic<unsigned> g_indexReaders{0};
struct IndexReader {
    IndexReader() { g_indexReaders.fetch_add(1); }
    ~IndexReader() { g_indexReaders.fetch_sub(1); }
};
std::atomic<std::uint64_t> g_worldEpoch{0}, g_installedEpoch{0}, g_generation{0};
std::atomic<bool> g_stopping{false};
std::unique_ptr<SectionIndex> g_index;
std::vector<std::unique_ptr<SectionIndex>> g_retired;
std::mutex g_liveMutex;
std::vector<SectionLive> g_live;
std::shared_ptr<const CellSet> g_cells;
std::shared_ptr<const std::vector<const void*>> g_palette;
const void* g_air = nullptr;
std::vector<std::size_t> g_range;
std::set<std::pair<int, int>> g_missingColumns;
std::vector<SectionKey> g_rangeKeys;
std::unordered_set<SectionKey, SectionKeyHash> g_boxDirty;
std::mutex g_snapshotMutex;
std::shared_ptr<const RenderSnapshot> g_snapshot;
std::uint64_t g_serial = 0, g_handledEpoch = 0;
unsigned long long g_learnAt = 0, g_snapshotAt = 0, g_settleUntil = 0;
std::size_t g_learnCursor = 0;
bool g_learning = false, g_rangeValid = false, g_snapshotPending = false;
int g_cx = 0, g_cz = 0, g_radius = 0;
LayerFilter g_layer;
bool g_overMismatch = true;
thread_local const BuildWindow* t_window = nullptr;

std::mutex g_verifierMutex;
int g_verifierOwner = -2;
bool g_verifierRequested = false, g_verifierSettingsPending = false;
VerifierSelection g_verifierWanted, g_verifierSelection, g_highlightSelection;
std::vector<VerifierIgnored> g_verifierIgnoredWanted, g_verifierIgnored;
std::shared_ptr<const VerifierReport> g_verifierReport = std::make_shared<VerifierReport>();
std::uint64_t g_verifierSerial = 0, g_highlightGeneration = 0;
bool g_verifierChanged = true, g_highlightFiltered = false;
HighlightMap g_highlight;
std::unordered_map<const void*, std::string> g_verifierNames;
struct PairKey {
    DiffKind kind;
    std::uint16_t want;
    const void* real;
    bool operator==(const PairKey&) const = default;
};
struct PairHash {
    std::size_t operator()(const PairKey& key) const noexcept {
        return std::hash<const void*>{}(key.real) ^ (static_cast<std::size_t>(key.want) << 4)
            ^ static_cast<std::size_t>(key.kind);
    }
};
using PairMap = std::unordered_map<PairKey, VerifierPair, PairHash>;
struct VerifierWork {
    VerifierReport report;
    VerifierSelection selection;
    std::vector<std::size_t> range;
    std::size_t section = 0;
    std::uint32_t slot = 0;
    std::uint64_t sectionGeneration = 0;
    float eye[3]{};
    PairMap pairs, sectionPairs;
    HighlightMap highlight;
    std::shared_ptr<std::bitset<4096>> bits;
    bool filtered = false;
};
std::unique_ptr<VerifierWork> g_verifierWork;
std::atomic<unsigned long long> g_verifierInterestAt{0};
constexpr unsigned long long kVerifierInterestMs = 5 * 60 * 1000;

const std::string& verifierName(const void* block)
{
    auto [it, added] = g_verifierNames.try_emplace(block);
    if (added) {
        char name[512]{};
        if (block && blocks::nameOfBlock(block, name, sizeof(name))) it->second = name;
        else it->second = "?";
    }
    return it->second;
}

void addVerifierPair(PairMap& pairs, const PairKey& key, const VerifierPair& value)
{
    auto [it, added] = pairs.try_emplace(key, value);
    if (added) return;
    auto& pair = it->second;
    pair.count += value.count;
    if (value.nearestDist2 < pair.nearestDist2) {
        std::copy(std::begin(value.nearest), std::end(value.nearest), pair.nearest);
        pair.nearestDist2 = value.nearestDist2;
    }
}

void resetVerifierLocked()
{
    g_verifierWork.reset(); g_verifierChanged = true;
    g_highlight.clear(); g_highlightSelection = {}; g_highlightFiltered = false;
    g_verifierNames.clear(); ++g_highlightGeneration;
    std::lock_guard lock(g_verifierMutex);
    g_verifierWanted = {}; g_verifierSelection = {};
    auto empty = std::make_shared<VerifierReport>();
    empty->serial = ++g_verifierSerial; empty->scopeOwner = g_verifierOwner;
    g_verifierReport = std::move(empty);
}

void verifierFrame(const FrameInput& in)
{
    {
        std::lock_guard lock(g_verifierMutex);
        if (g_verifierRequested || g_verifierSettingsPending) {
            g_verifierWork.reset(); g_verifierChanged = true;
            g_verifierSelection = g_verifierWanted;
            g_verifierIgnored = g_verifierIgnoredWanted;
            g_verifierRequested = g_verifierSettingsPending = false;
            if ((!g_verifierSelection.active || (!g_verifierSelection.pair
                && g_verifierSelection.scopeOwner == -1 && g_verifierIgnored.empty()))
                && (g_highlightSelection != g_verifierSelection || g_highlightFiltered)) {
                g_highlightSelection = g_verifierSelection; g_highlight.clear(); g_highlightFiltered = false;
                ++g_highlightGeneration; g_snapshotPending = true; g_snapshotAt = 0;
            }
        }
    }
    if (!in.renderRegion || in.now < g_settleUntil) return;
    const bool interested = g_verifierSelection.active
        || [&] {
               const auto at = g_verifierInterestAt.load(std::memory_order_relaxed);
               return at != 0 && (in.now <= at || in.now - at < kVerifierInterestMs);
           }();
    if (!g_verifierWork && g_verifierChanged && interested) {
        auto work = std::make_unique<VerifierWork>();
        { std::lock_guard lock(g_verifierMutex); work->report.scopeOwner = g_verifierOwner; }
        work->selection = g_verifierSelection;
        work->range = g_range; work->report.sectionsTotal = g_range.size();
        std::copy(std::begin(in.eye), std::end(in.eye), work->eye);
        work->filtered = work->selection.active && (work->selection.pair
            || work->selection.scopeOwner != -1 || !g_verifierIgnored.empty());
        g_verifierWork = std::move(work); g_verifierChanged = false;
    }
    if (!g_verifierWork) return;
    auto& work = *g_verifierWork;
    LARGE_INTEGER started{}, now{}, frequency{};
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&started);
    const auto expired = [&] {
        QueryPerformanceCounter(&now);
        return (now.QuadPart - started.QuadPart) * 1000000 / frequency.QuadPart >= 2000;
    };
    while (work.section < work.range.size()) {
        const auto at = work.range[work.section];
        const auto& live = g_live[at];
        const auto key = g_index->key(at);
        if (work.slot && work.sectionGeneration != live.generation) {
            work.slot = 0; work.sectionPairs.clear(); work.bits.reset();
        }
        if (!live.diff || live.needCompare || (!live.subChunk && !live.emptySub)) {
            work.slot = 0; work.sectionPairs.clear(); work.bits.reset(); ++work.section;
            if (expired()) return;
            continue;
        }
        if (!work.slot) {
            work.sectionGeneration = live.generation;
            if (work.filtered) work.bits = std::make_shared<std::bitset<4096>>();
        }
        for (; work.slot < 4096; ++work.slot) {
            if ((work.slot & 63u) == 0 && expired()) return;
            const auto color = live.diff->color[work.slot];
            if (color == 0 || color > kDiffColorMax) continue;
            const auto kind = color == static_cast<std::uint8_t>(DiffColor::Extra) ? DiffKind::Extra
                : static_cast<DiffKind>(color);
            const auto x = key.cx * 16 + static_cast<std::int32_t>(work.slot >> 8);
            const auto y = key.cy * 16 + static_cast<std::int32_t>((work.slot >> 4) & 15);
            const auto z = key.cz * 16 + static_cast<std::int32_t>(work.slot & 15);
            const auto index = g_cells->find(x, y, z);
            Cell cell;
            if (index == CellSet::kNone || !g_cells->cellAt(index, cell)) continue;
            const bool counted = work.report.scopeOwner == -1 || work.report.scopeOwner == cell.owner;
            const auto& selection = work.selection;
            const bool selected = work.filtered && selection.kind == kind
                && (selection.scopeOwner == -1 || selection.scopeOwner == cell.owner);
            if (!counted && !selected) continue;
            const auto entry = kind == DiffKind::Extra ? std::uint16_t{0xFFFF}
                : (live.want ? live.want->want[work.slot] : std::uint16_t{0xFFFF});
            const void* real = live.emptySub ? g_air : blockwrite::readBlockOf(live.subChunk, 0,
                blockwrite::beIndexOf(static_cast<int>(work.slot >> 8),
                    static_cast<int>((work.slot >> 4) & 15), static_cast<int>(work.slot & 15)));
            if (!real) continue;
            const auto& realName = verifierName(real);
            static const std::string airName = "minecraft:air";
            const auto& wantName = kind == DiffKind::Extra ? airName
                : (cell.name ? *cell.name : verifierName(entry > 0 && entry <= g_palette->size() ? (*g_palette)[entry - 1] : nullptr));
            const bool ignored = std::any_of(g_verifierIgnored.begin(), g_verifierIgnored.end(), [&](const auto& value) {
                return value.kind == kind && value.wantName == wantName && value.realName == realName;
            });
            if (ignored) continue;
            if (selected
                && (!selection.pair || (selection.wantEntry == entry && selection.real == real))) work.bits->set(work.slot);
            if (!counted) continue;
            auto [found, added] = work.sectionPairs.try_emplace({kind, entry, real});
            auto& pair = found->second;
            if (added) {
                pair.kind = kind; pair.wantEntry = entry; pair.real = real;
                pair.wantName = wantName; pair.realName = realName;
            }
            ++pair.count;
            const double dx = x + 0.5 - work.eye[0], dy = y + 0.5 - work.eye[1], dz = z + 0.5 - work.eye[2];
            const double distance = dx * dx + dy * dy + dz * dz;
            if (added || distance < pair.nearestDist2) {
                pair.nearestDist2 = distance;
                pair.nearest[0] = x; pair.nearest[1] = y; pair.nearest[2] = z;
            }
        }
        for (const auto& [pairKey, pair] : work.sectionPairs) {
            addVerifierPair(work.pairs, pairKey, pair);
            work.report.counts[static_cast<std::size_t>(pair.kind)] += pair.count;
        }
        if (work.report.scopeOwner == -1)
            work.report.counts[static_cast<std::size_t>(DiffKind::Match)] += live.diff->counts[static_cast<std::size_t>(DiffKind::Match)];
        if (work.bits && work.bits->any()) work.highlight.emplace(key, std::move(work.bits));
        work.bits.reset(); work.sectionPairs.clear(); work.slot = 0;
        ++work.section; ++work.report.sectionsDone;
        if (expired()) return;
    }
    work.report.pairs.reserve(work.pairs.size());
    for (auto& [key, pair] : work.pairs) { (void)key; work.report.pairs.push_back(std::move(pair)); }
    std::sort(work.report.pairs.begin(), work.report.pairs.end(), [](const auto& a, const auto& b) {
        if (a.count != b.count) return a.count > b.count;
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.wantEntry != b.wantEntry) return a.wantEntry < b.wantEntry;
        return std::less<const void*>{}(a.real, b.real);
    });
    work.report.complete = true; work.report.serial = ++g_verifierSerial;
    { std::lock_guard lock(g_verifierMutex); g_verifierReport = std::make_shared<VerifierReport>(std::move(work.report)); }
    if (g_highlightSelection != work.selection || g_highlightFiltered != work.filtered) ++g_highlightGeneration;
    else if (work.filtered) {
        const auto mark = [](const SectionKey& key) {
            for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) for (int dz = -1; dz <= 1; ++dz)
                g_boxDirty.insert({key.cx + dx, key.cy + dy, key.cz + dz});
        };
        for (const auto& [key, bits] : g_highlight) {
            const auto it = work.highlight.find(key);
            if (it == work.highlight.end() || *bits != *it->second) mark(key);
        }
        for (const auto& [key, bits] : work.highlight) {
            (void)bits;
            if (!g_highlight.contains(key)) mark(key);
        }
    }
    g_highlight = std::move(work.highlight); g_highlightSelection = work.selection;
    g_highlightFiltered = work.filtered;
    g_snapshotPending = true; g_snapshotAt = 0;
    g_verifierWork.reset();
}

bool readersIdle(unsigned long long limit)
{
    const auto until = GetTickCount64() + limit;
    while (g_indexReaders.load() != 0) {
        if (GetTickCount64() >= until) return false;
        Sleep(1);
    }
    return true;
}

void detachIndex()
{
    g_indexPtr.store(nullptr);
    if (g_index) g_retired.push_back(std::move(g_index));
    if (readersIdle(200)) g_retired.clear();
}

void publishEmpty()
{
    auto next = std::make_shared<RenderSnapshot>();
    next->serial = ++g_serial;
    std::lock_guard lock(g_snapshotMutex);
    g_snapshot = std::move(next);
}

bool readSection(void* ctx, std::uint32_t index, const void*& block0, const void*& block1, bool& known)
{
    if (ctx == nullptr) {
        block0 = g_air; block1 = nullptr; known = true;
        return block0 != nullptr;
    }
    const unsigned beIndex = blockwrite::beIndexOf(static_cast<int>(index >> 8),
        static_cast<int>((index >> 4) & 15), static_cast<int>(index & 15));
    block0 = blockwrite::readBlockOf(ctx, 0, beIndex);
    known = blockwrite::readExtraOf(ctx, beIndex, block1);
    return block0 != nullptr;
}

void dropReal(std::size_t at)
{
    auto& live = g_live[at];
    const auto key = g_index->key(at);
    blockwrite::forgetSubChunkAt(key.cx * 16, key.cy * 16, key.cz * 16);
    live.subChunk = nullptr;
    live.emptySub = false;
    live.diff.reset();
    live.building.reset();
    live.compareCursor = 0;
    live.needCompare = true;
    live.dirty = false;
    ++live.generation;
    for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) for (int dz = -1; dz <= 1; ++dz)
        g_boxDirty.insert({key.cx + dx, key.cy + dy, key.cz + dz});
    g_snapshotPending = true;
    g_verifierChanged = true;
}
}

bool active()
{
    return !g_stopping.load() && g_worldEpoch.load() == g_installedEpoch.load()
        && g_indexPtr.load() != nullptr;
}

void install(std::shared_ptr<const schematic::CellSet> cells,
             std::shared_ptr<const std::vector<const void*>> palette, const void* air)
{
    std::lock_guard lock(g_liveMutex);
    const auto epoch = g_worldEpoch.load();
    detachIndex();
    g_live.clear();
    g_range.clear(); g_rangeKeys.clear(); g_boxDirty.clear();
    g_rangeValid = false; g_learning = false; g_learnAt = 0;
    g_cells = std::move(cells); g_palette = std::move(palette); g_air = air;
    resetVerifierLocked();
    blockwrite::forgetSubChunks();
    if (g_cells && !g_cells->empty() && g_palette) {
        std::vector<schematic::SectionKey> keys;
        keys.reserve(g_cells->chunks.size());
        for (const auto& key : g_cells->chunks) keys.push_back({key.x >> 4, key.y >> 4, key.z >> 4});
        g_index = std::make_unique<schematic::SectionIndex>(std::move(keys));
        g_live.resize(g_index->size());
        g_installedEpoch.store(epoch);
        g_indexPtr.store(g_index.get());
    }
    g_generation.fetch_add(1);
    g_snapshotPending = true; g_snapshotAt = 0;
    publishEmpty();
}

void clear()
{
    install(nullptr, nullptr, nullptr);
}

void onWorldChanged()
{
    g_worldEpoch.fetch_add(1);
}

void frame(const FrameInput& in)
{
    std::lock_guard lock(g_liveMutex);
    const auto epoch = g_worldEpoch.load();
    if (g_handledEpoch != epoch) {
        g_handledEpoch = epoch;
        for (auto& live : g_live) {
            live.subChunk = nullptr; live.emptySub = false; live.chunk = nullptr; live.diff.reset(); live.building.reset();
            live.compareCursor = 0; live.outsideWorld = false; live.needCompare = true;
        }
        blockwrite::forgetSubChunks();
        g_settleUntil = in.now + kWorldSettleMs;
        g_learning = false; g_learnAt = 0;
        resetVerifierLocked();
        g_snapshotPending = true;
        publishEmpty();
    }
    if (!active() || !g_index) return;
    const int cx = static_cast<int>(std::floor(in.eye[0])) >> 4;
    const int cy = static_cast<int>(std::floor(in.eye[1])) >> 4;
    const int cz = static_cast<int>(std::floor(in.eye[2])) >> 4;
    if (!g_rangeValid || cx != g_cx || cz != g_cz || in.radiusChunks != g_radius) {
        const auto oldRange = g_range;
        schematic::sectionsInRange(*g_index, cx, cy, cz, in.radiusChunks, g_range);
        for (const auto at : oldRange) g_live[at].inRange = false;
        g_rangeKeys.clear();
        for (const auto at : g_range) {
            auto& live = g_live[at]; live.inRange = true; live.outsideWorld = false;
            if (!live.diff) live.needCompare = true;
            g_rangeKeys.push_back(g_index->key(at));
        }
        for (const auto at : oldRange) if (!g_live[at].inRange) {
            dropReal(at); g_live[at].chunk = nullptr;
        }
        g_cx = cx; g_cz = cz; g_radius = in.radiusChunks; g_rangeValid = true;
        g_learning = false; g_learnAt = 0; g_snapshotPending = true;
        g_verifierWork.reset(); g_verifierChanged = true;
    }
    if (g_layer.on != in.layer.on || g_layer.axis != in.layer.axis || g_layer.lo != in.layer.lo
        || g_layer.hi != in.layer.hi || g_overMismatch != in.ghostOverMismatch) {
        g_layer = in.layer; g_overMismatch = in.ghostOverMismatch; g_snapshotPending = true;
    }
    if (in.renderRegion && in.now >= g_settleUntil) {
        if (!g_learning && (g_learnAt == 0 || in.now - g_learnAt >= kLearnMs)) {
            std::map<std::pair<int, int>, std::vector<std::size_t>> columns;
            for (const auto at : g_range) {
                const auto key = g_index->key(at); columns[{key.cx, key.cz}].push_back(at);
            }
            for (const auto& [key, slots] : columns) {
                void* chunk = blockwrite::findChunk(in.renderRegion, key.first * 16, key.second * 16);
                for (const auto at : slots) if (g_live[at].chunk != chunk) {
                    dropReal(at); g_live[at].chunk = chunk;
                }
            }
            g_learning = true; g_learnCursor = 0; g_learnAt = in.now; g_missingColumns.clear();
        }
        unsigned learned = 0;
        while (g_learning && g_learnCursor < g_range.size() && learned < in.learnPerFrame) {
            const auto at = g_range[g_learnCursor++]; auto& live = g_live[at];
            const auto key = g_index->key(at);
            if (live.subChunk || live.outsideWorld || g_missingColumns.contains({key.cx, key.cz})) continue;
            ++learned;
            live.subChunk = blockwrite::findSubChunk(in.renderRegion, key.cx * 16, key.cy * 16, key.cz * 16);
            if (live.subChunk) {
                blockwrite::noteSubChunkAt(key.cx * 16, key.cy * 16, key.cz * 16, live.subChunk);
                if (live.emptySub) { live.building.reset(); live.compareCursor = 0; }
                live.emptySub = false;
                live.needCompare = true;
            } else if (blockwrite::lastFindWasOutsideWorld()) live.outsideWorld = true;
            else if (blockwrite::lastFindWasMissingChunk()) g_missingColumns.insert({key.cx, key.cz});
            else if (blockwrite::lastFindWasAboveTop() && !live.emptySub) {
                live.emptySub = true;
                live.needCompare = true;
            }
        }
        if (g_learnCursor == g_range.size()) g_learning = false;
    }
    std::vector<std::size_t> dirty;
    g_index->takeDirty(dirty);
    for (const auto at : dirty) if (g_live[at].inRange && (g_live[at].subChunk || g_live[at].emptySub)) {
        auto& live = g_live[at]; live.needCompare = true; live.dirty = true;
        if (in.renderRegion && in.now >= g_settleUntil) {
            const auto key = g_index->key(at);
            void* const sub = blockwrite::findSubChunk(in.renderRegion, key.cx * 16, key.cy * 16, key.cz * 16);
            if (sub) {
                blockwrite::noteSubChunkAt(key.cx * 16, key.cy * 16, key.cz * 16, sub);
                live.subChunk = sub;
                live.emptySub = false;
            }
        }
        live.building.reset(); live.compareCursor = 0;
    }
    std::vector<std::size_t> candidates;
    unsigned prioritized = 0;
    for (const auto at : g_range) if (g_live[at].dirty && prioritized < kCompareDirtyPerFrame) {
        candidates.push_back(at); ++prioritized;
    }
    for (const auto at : g_range) if (g_live[at].needCompare
        && std::find(candidates.begin(), candidates.begin() + prioritized, at) == candidates.begin() + prioritized)
        candidates.push_back(at);
    bool unseen = false;
    for (const auto at : g_range) if (!g_live[at].diff) { unseen = true; break; }
    const unsigned budget = in.compareBudgetUs ? in.compareBudgetUs
        : (unseen ? kCompareBudgetSweepUs : kCompareBudgetSteadyUs);
    LARGE_INTEGER started{}, frequency{}, now{};
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&started);
    for (const auto at : candidates) {
        auto& live = g_live[at];
        if ((!live.subChunk && !live.emptySub) || !in.renderRegion || in.now < g_settleUntil) continue;
        if (!live.wantBuilt) {
            auto want = std::make_shared<schematic::WantSection>();
            schematic::buildWantSection(*g_cells, g_index->key(at), *want);
            const bool allAir = !want->anyWater && std::all_of(std::begin(want->want), std::end(want->want),
                [](auto tag) { return tag == schematic::kWantAir; });
            if (!allAir) live.want = std::move(want);
            live.wantBuilt = true;
        }
        if (!live.building) live.building = std::make_unique<schematic::DiffSection>();
        do {
            const auto to = (std::min)(live.compareCursor + 512u, 4096u);
            schematic::compareSection(live.want.get(), *g_palette, g_air, readSection, live.emptySub ? nullptr : live.subChunk,
                blocks::legacyOfFast, live.compareCursor, to, *live.building);
            live.compareCursor = to;
            if (to == 4096) {
                schematic::finishDiffSection(*live.building);
                live.diff = std::move(live.building); live.compareCursor = 0;
                live.needCompare = false; live.dirty = false; ++live.generation;
                g_verifierChanged = true;
                const auto key = g_index->key(at);
                for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) for (int dz = -1; dz <= 1; ++dz)
                    g_boxDirty.insert({key.cx + dx, key.cy + dy, key.cz + dz});
                g_snapshotPending = true;
            }
            QueryPerformanceCounter(&now);
            if ((now.QuadPart - started.QuadPart) * 1000000 / frequency.QuadPart >= budget) goto publish;
        } while (live.building);
    }
publish:
    verifierFrame(in);
    if (g_snapshotPending && (g_snapshotAt == 0 || in.now - g_snapshotAt >= kSnapshotMinMs)) {
        auto next = std::make_shared<schematic::RenderSnapshot>();
        next->palette = g_palette; next->air = g_air; next->layer = g_layer;
        next->ghostOverMismatch = g_overMismatch; next->serial = ++g_serial;
        next->verifierActive = g_highlightSelection.active;
        next->selectedColor = g_highlightSelection.active ? colorOfDiffKind(g_highlightSelection.kind) : DiffColor::None;
        next->highlightFiltered = g_highlightFiltered; next->highlight = g_highlight;
        next->highlightGeneration = g_highlightGeneration;
        for (const auto at : g_range) {
            const auto& live = g_live[at];
            if (!live.wantBuilt || !live.diff) continue;
            const auto key = g_index->key(at);
            next->at.emplace(key, static_cast<std::uint32_t>(next->sections.size()));
            next->sections.push_back({key, live.want, live.diff, live.generation});
        }
        { std::lock_guard snapshotLock(g_snapshotMutex); g_snapshot = std::move(next); }
        g_snapshotAt = in.now; g_snapshotPending = false;
    }
}

std::shared_ptr<const schematic::RenderSnapshot> renderSnapshot()
{
    if (!active()) return nullptr;
    std::lock_guard lock(g_snapshotMutex);
    return g_snapshot;
}
const void* realBlockAt(const std::int32_t pos[3])
{
    if (!pos || !active()) return nullptr;
    void* const region = blockwrite::renderRegion();
    if (!region) return nullptr;
    void* const sub = blockwrite::findSubChunk(region, pos[0], pos[1], pos[2]);
    if (sub) return blockwrite::readBlockOf(sub, 0, blockwrite::beIndexOf(pos[0], pos[1], pos[2]));
    if (!blockwrite::lastFindWasAboveTop()) return nullptr;
    const auto snap = renderSnapshot();
    return snap ? snap->air : nullptr;
}
bool emptyBlock(const void* block)
{
    if (!block) return false;
    const auto snap = renderSnapshot();
    if (snap && snap->air == block) return true;
    bool motion = false, liquid = false;
    return blocks::materialFlags(block, motion, liquid) && liquid;
}
SchematicHit traceSchematic(const float eye[3], const float dir[3], double reach)
{
    SchematicHit hit;
    easyplace::RayCursor ray;
    if (!active() || !ray.begin(eye, dir, reach)) return hit;
    std::lock_guard lock(g_liveMutex);
    const auto snap = renderSnapshot();
    if (!g_cells || !snap || !snap->palette || snap->palette != g_palette) return hit;
    do {
        const void* const real = realBlockAt(ray.pos);
        if (!real || !emptyBlock(real)) return hit;
        if (!snap->layer.shows(ray.pos[0], ray.pos[1], ray.pos[2])) continue;
        const auto at = g_cells->find(ray.pos[0], ray.pos[1], ray.pos[2]);
        schematic::Cell cell;
        if (at == schematic::CellSet::kNone || !g_cells->cellAt(at, cell)
            || cell.entry == schematic::kAirCell || cell.entry >= snap->palette->size()) continue;
        const void* const want = (*snap->palette)[cell.entry];
        if (!want || want == snap->air) continue;
        hit.hit = true; std::copy(std::begin(ray.pos), std::end(ray.pos), hit.pos);
        hit.want = want; hit.owner = cell.owner; hit.dist = ray.distance; hit.realEmpty = true;
        return hit;
    } while (ray.next());
    return hit;
}
void requestVerifier(int scopeOwner)
{
    std::lock_guard lock(g_verifierMutex);
    g_verifierOwner = scopeOwner; g_verifierRequested = true;
}
std::shared_ptr<const VerifierReport> verifierReport()
{
    g_verifierInterestAt.store(GetTickCount64(), std::memory_order_relaxed);
    std::lock_guard lock(g_verifierMutex);
    return g_verifierReport;
}
void setVerifierSelection(const VerifierSelection& selection)
{
    std::lock_guard lock(g_verifierMutex);
    if (g_verifierWanted == selection) return;
    g_verifierWanted = selection; g_verifierSettingsPending = true;
}
void setVerifierIgnored(std::vector<VerifierIgnored> ignored)
{
    std::lock_guard lock(g_verifierMutex);
    g_verifierIgnoredWanted = std::move(ignored); g_verifierSettingsPending = true;
}
std::size_t takeBoxDirty(std::vector<schematic::SectionKey>& out)
{
    std::lock_guard lock(g_liveMutex);
    out.clear();
    if (!g_snapshotPending) {
        out.assign(g_boxDirty.begin(), g_boxDirty.end());
        g_boxDirty.clear();
    }
    return out.size();
}
std::uint64_t indexGeneration() { return g_generation.load(); }
const std::vector<schematic::SectionKey>& inRangeKeys() { return g_rangeKeys; }
void missingByOwner(std::map<int, std::map<std::uint16_t, std::size_t>>& out)
{
    out.clear();
    std::lock_guard lock(g_liveMutex);
    if (!active()) return;
    for (const auto at : g_range) {
        const auto& live=g_live[at];
        if (!live.diff || live.diff->missingByEntry.empty()) continue;
        const auto key=g_index->key(at);
        const auto missing=static_cast<std::uint8_t>(schematic::DiffColor::Missing);
        for (std::uint32_t slot=0; slot<4096; ++slot) {
            if (live.diff->color[slot]!=missing) continue;
            const auto x=key.cx*16+static_cast<std::int32_t>(slot>>8);
            const auto y=key.cy*16+static_cast<std::int32_t>((slot>>4)&15);
            const auto z=key.cz*16+static_cast<std::int32_t>(slot&15);
            if (!g_layer.shows(x,y,z)) continue;
            const auto index=g_cells->find(x,y,z);
            schematic::Cell cell;
            if (index==schematic::CellSet::kNone || !g_cells->cellAt(index,cell) || cell.entry==schematic::kAirCell) continue;
            const auto& sources=g_cells->sources;
            auto source=std::upper_bound(sources.begin(),sources.end(),index,
                [](std::size_t value,const auto& entry) { return value<entry.offset; });
            if (source==sources.begin()) continue;
            --source;
            out[cell.owner][static_cast<std::uint16_t>(cell.entry-source->placed.paletteBase)]++;
            if (cell.entry2 >= 0)
                out[cell.owner][static_cast<std::uint16_t>(static_cast<std::size_t>(cell.entry2)-source->placed.paletteBase)]++;
        }
    }
}
void onSubChunkWrite(void* subChunk, unsigned layer, unsigned index)
{
    if (layer > 1 || !active()) return;
    g_indexReaders.fetch_add(1);
    auto* const table = g_indexPtr.load();
    int pos[3]{};
    if (table && active() && blockwrite::worldPosOfSubChunkWrite(subChunk, index, pos))
        table->markDirty(schematic::sectionOf(pos[0], pos[1], pos[2]));
    g_indexReaders.fetch_sub(1);
}
void enterBuildWindow(const BuildWindow* window) { t_window = window; }
void leaveBuildWindow() { t_window = nullptr; }
bool answerForBuild(const std::int32_t pos[3], int layer, const void*& out)
{
    const BuildWindow* const w = t_window;
    if (!w || !w->snap || !pos || (layer != 0 && layer != 1) || !active()) return false;
    IndexReader reader;
    if (!active()) return false;
    const auto& snap = *w->snap;
    const auto it = snap.at.find(schematic::sectionOf(pos[0], pos[1], pos[2]));
    if (it == snap.at.end()) return false;
    const auto& v = snap.sections[it->second];
    const auto i = schematic::slotOf(pos[0], pos[1], pos[2]);
    if (!v.want) { if (layer == 1 || !snap.air) return false; out = snap.air; return true; }
    const auto tag = layer == 1 ? (v.want->anyWater ? v.want->want2[i] : 0) : v.want->want[i];
    if (tag == schematic::kWantOutside || tag == schematic::kWantUnresolved) return false;
    if (tag == schematic::kWantAir) { if (layer == 1 || !snap.air) return false; out = snap.air; return true; }
    if (!snap.palette || tag - 1u >= snap.palette->size() || (*snap.palette)[tag - 1u] == nullptr) return false;
    out = (*snap.palette)[tag - 1u]; return true;
}
void waitUntilIdle()
{
    g_stopping.store(true);
    readersIdle(200);
    Sleep(50);
}

}
