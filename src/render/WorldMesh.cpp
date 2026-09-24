#include "render/WorldMesh.h"

#include "core/Logger.h"
#include "core/Strings.h"
#include "game/BlockRegistry.h"
#include "hooks/HookManager.h"
#include "hooks/HookCount.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "memory/Signatures.h"
#include "modules/FreeCamera.h"
#include "modules/Zoom.h"
#include "render/BoxMesher.h"
#include "render/BoxRenderer.h"
#include "render/Overlay.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tsukuyomi::worldmesh {
namespace {

using NameTagStageFn = void(__fastcall*)(void* lrp, void* ctx, void* view, void* extra);
using TessBeginFn = void(__fastcall*)(void* tess, void* unused, std::uint8_t mode, int reserve,
                                      bool faceData);
using TessVertexFn = void(__fastcall*)(void* tess, float x, float y, float z);
using TessColorFn = void(__fastcall*)(void* tess, float r, float g, float b, float a);
using RenderMeshFn = void(__fastcall*)(void* ctx, void* tess, const void* material,
                                       void* texture);
using MaterialCtorFn = void*(__fastcall*)(void* out, void* group, const void* name);

NameTagStageFn g_original = nullptr;
using TessEndFn = void(__fastcall*)(void* tess, void* outMesh, int usage, const void* name,
                                    unsigned int flag);
using TessClearFn = void(__fastcall*)(void* buffers);
using RendererFlagFn = unsigned int(__fastcall*)();
using MeshRenderFn = void(__fastcall*)(void* mesh, void* ctxPlus10, const void* material,
                                       void* params, void* a5, void* a6, void* texture, void* a8,
                                       void* optional);
using MeshDestroyFn = void(__fastcall*)(void* mesh);
TessEndFn g_meshEnd = nullptr;
TessClearFn g_tessClear = nullptr;
RendererFlagFn g_rendererFlag = nullptr;
MeshRenderFn g_meshRender = nullptr;
MeshDestroyFn g_meshDestroy = nullptr;
std::atomic<bool> g_keptBroken{false};
using StageHostFn = void*(__fastcall*)(void*, void*, void*, void*);
constexpr std::ptrdiff_t kHostCtxAt = 0x28;
constexpr std::ptrdiff_t kViewMainFlag = 0x81;
StageHostFn g_hostOriginal = nullptr;
std::atomic<unsigned long long> g_hostCalls{0};
std::atomic<int> g_drawAt{0};
std::atomic<bool> g_drewThisFrame{false};
TessBeginFn g_begin = nullptr;
TessVertexFn g_vertex = nullptr;
TessColorFn g_color = nullptr;
RenderMeshFn g_render = nullptr;
MaterialCtorFn g_matCtor = nullptr;
void* g_matGroup = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_teardown{false};

constexpr std::ptrdiff_t kCtxColor = 0x30;
constexpr std::ptrdiff_t kCtxMatrices = 0x18;
constexpr std::ptrdiff_t kCtxTess = 0xb8;
constexpr std::ptrdiff_t kTessLimit = 0x29c;
constexpr std::ptrdiff_t kTessBuilding = 0x2a0;
constexpr std::ptrdiff_t kTessVoid = 0x255;
constexpr std::ptrdiff_t kTessNoColor = 0x254;
constexpr std::ptrdiff_t kTessOffset = 0x1cc;
constexpr std::ptrdiff_t kTessScale = 0x1d8;
constexpr std::ptrdiff_t kTessUseMatrix = 0x210;
constexpr std::size_t kTessReadable = kTessBuilding + 4;
constexpr std::ptrdiff_t kLrpCamera = 0x660;

std::uint64_t fnv1(std::string_view text)
{
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (const char c : text) {
        h *= 0x100000001B3ULL;
        h ^= static_cast<unsigned char>(c);
    }
    return h;
}

struct MaterialPtr {
    void* info = nullptr;
    void* ctrl = nullptr;
};

struct Choice {
    int mode;
    const char* name;
    std::ptrdiff_t lrpOffset;
    bool multiply;
};
constexpr Choice kChoices[] = {
    {1, "selection_overlay", 0x1090, true},
    {2, "name_tag", 0, false},
    {3, "selection_overlay_opaque", 0x10a0, true},
    {4, "selection_overlay_double_sided", 0x10b0, true},
    {5, "name_tag_depth_tested", 0, false},
    {6, "selection_box", 0x10f0, false},
};
constexpr int kDefaultFlat = 1;
constexpr int kDefaultXray = 2;

constexpr int kLineMode = 6;
constexpr std::uint8_t kPrimitiveQuads = 1;
constexpr std::uint8_t kPrimitiveLines = 4;
constexpr float kLineAlpha = 1.0F;

const Choice* choiceOf(int mode)
{
    for (const Choice& one : kChoices) {
        if (one.mode == mode) {
            return &one;
        }
    }
    return nullptr;
}

struct Named {
    MaterialPtr ptr{};
    bool tried = false;
    bool ok = false;
};
Named g_named[std::size(kChoices)];

std::atomic<bool> g_modeBroken[8]{};
constexpr int kDefaultMaxBoxes = 30000;
std::atomic<std::uint32_t> g_lastCalls{0};

std::atomic<unsigned long long> g_calls{0};
std::atomic<unsigned long long> g_drawn{0};
std::atomic<unsigned long long> g_failed{0};
std::atomic<unsigned long long> g_capped{0};
std::atomic<unsigned long long> g_noMaterial{0};
std::atomic<unsigned long long> g_noQuads{0};
std::atomic<unsigned long long> g_badCtx{0};
std::atomic<unsigned long long> g_camShifted{0};
std::atomic<unsigned long long> g_lines{0};
std::atomic<unsigned long long> g_noLineMaterial{0};
std::atomic<std::size_t> g_quadCount{0};
std::atomic<std::size_t> g_edgeCount{0};
std::atomic<std::size_t> g_cellCount{0};
std::atomic<int> g_quadRadius{-1};
std::atomic<unsigned long long> g_mergeMsLast{0};
std::atomic<unsigned long long> g_mergeMsMax{0};
std::atomic<std::size_t> g_mergeCount{0};
std::atomic<std::size_t> g_selectCount{0};
std::atomic<unsigned long long> g_selectUsMax{0};
std::atomic<std::size_t> g_sortCount{0};
std::atomic<unsigned long long> g_sortUsMax{0};
std::atomic<std::uint32_t> g_lastLines{0};
std::atomic<std::size_t> g_lastCap{0};
std::atomic<unsigned long long> g_keptFrames{0};
std::atomic<std::size_t> g_keptRegionCount{0};
std::atomic<std::size_t> g_keptMeshCount{0};
std::atomic<std::size_t> g_keptFaceCount{0};
std::atomic<std::size_t> g_keptLineCount{0};
std::atomic<std::uint32_t> g_keptLastDrawn{0};
std::atomic<std::uint32_t> g_keptLastCulled{0};
std::atomic<unsigned long long> g_keptDrawUsLast{0};
std::atomic<unsigned long long> g_keptDrawUsMax{0};
std::atomic<unsigned long long> g_keptRebuilt{0};
std::atomic<unsigned long long> g_keptRebuildUsMax{0};
std::atomic<unsigned long long> g_keptRegionUsMax{0};
std::atomic<std::size_t> g_keptWaiting{0};
std::atomic<std::size_t> g_groupCount{0};
std::atomic<unsigned long long> g_groupMsLast{0};
std::atomic<unsigned long long> g_groupMsMax{0};
std::atomic<std::uint32_t> g_lastFaces[boxmesh::kMaxColor + 1]{};
std::atomic<unsigned long long> g_edgesSkipped{0};
constexpr std::size_t kEdgeCellLimit = 120000;
std::atomic<unsigned long long> g_microsMax{0};
std::atomic<bool> g_inside{false};
std::atomic<unsigned> g_failLogs{0};

__declspec(noinline) bool callMaterialCtor(MaterialPtr* out, void* group, const void* name)
{
    __try {
        g_matCtor(out, group, name);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) void callOriginal(void* lrp, void* ctx, void* view, void* extra)
{
    g_original(lrp, ctx, view, extra);
}

bool infoMatches(const void* info, std::uint64_t hash)
{
    if (info == nullptr || !memory::isReadable(info, 0x18)) {
        return false;
    }
    std::uint64_t got = 0;
    std::memcpy(&got, static_cast<const unsigned char*>(info) + 0x10, sizeof(got));
    return got == hash;
}

struct HashedStringBuf {
    std::uint64_t hash;
    char text[16];
    std::uint64_t size;
    std::uint64_t capacity;
    std::uint64_t extra;
};
static_assert(sizeof(HashedStringBuf) == 0x30);

char g_longNames[std::size(kChoices)][64]{};

void makeHashed(std::size_t slot, std::string_view name, HashedStringBuf& out)
{
    std::memset(&out, 0, sizeof(out));
    out.hash = fnv1(name);
    out.size = name.size();
    if (name.size() <= 15) {
        std::memcpy(out.text, name.data(), name.size());
        out.capacity = 15;
    } else {
        const std::size_t n = (std::min)(name.size(), sizeof(g_longNames[0]) - 1);
        std::memcpy(g_longNames[slot], name.data(), n);
        g_longNames[slot][n] = 0;
        char* const at = g_longNames[slot];
        std::memcpy(out.text, &at, sizeof(at));
        out.size = n;
        out.capacity = sizeof(g_longNames[0]) - 1;
    }
}

bool resolveNamed(std::size_t slot)
{
    Named& named = g_named[slot];
    if (named.tried) {
        return named.ok;
    }
    named.tried = true;
    const Choice& choice = kChoices[slot];
    const std::string_view name{choice.name};
    const std::uint64_t hash = fnv1(name);

    if (g_matCtor != nullptr && g_matGroup != nullptr) {
        alignas(16) HashedStringBuf buf{};
        makeHashed(slot, name, buf);
        MaterialPtr made{};
        const bool called = callMaterialCtor(&made, g_matGroup, &buf);
        named.ok = called && infoMatches(made.info, hash);
        named.ptr = made;
        return named.ok;
    }

    log().warn(L"WorldMesh: material {} could not be resolved (the constructor site was not found)",
               toUtf16(name));
    return false;
}

const void* materialFor(void* lrp, int mode)
{
    const Choice* const choice = choiceOf(mode);
    if (choice == nullptr) {
        return nullptr;
    }
    if (choice->lrpOffset != 0) {
        auto* const at = static_cast<unsigned char*>(lrp) + choice->lrpOffset;
        if (!memory::isReadable(at, sizeof(MaterialPtr))) {
            return nullptr;
        }
        void* info = nullptr;
        std::memcpy(&info, at, sizeof(info));
        if (!infoMatches(info, fnv1(choice->name))) {
            static std::atomic<unsigned> told{0};
            if (told.fetch_add(1, std::memory_order_relaxed) < 3) {
                log().warn(L"WorldMesh: the material at LRP{:+#x} is not {} (object {:#x}). "
                           L"This view falls back to the original drawing path",
                           choice->lrpOffset,
                           toUtf16(choice->name),
                           reinterpret_cast<std::uintptr_t>(info));
            }
            g_modeBroken[mode & 7].store(true, std::memory_order_relaxed);
            return nullptr;
        }
        return at;
    }
    const auto slot = static_cast<std::size_t>(choice - kChoices);
    if (!resolveNamed(slot)) {
        g_modeBroken[mode & 7].store(true, std::memory_order_relaxed);
        return nullptr;
    }
    return &g_named[slot].ptr;
}

struct Segment {
    std::uint8_t color = 0;
    bool line = false;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

struct RegionOut {
    boxmesh::RegionKey key;
    std::uint64_t generation = 0;
    std::shared_ptr<const boxmesh::RegionGeometry> geometry;
};

struct Prepared {
    unsigned long long version = ~0ULL;
    bool xray = false;
    std::vector<boxmesh::Quad> quads;
    std::size_t begin[boxmesh::kMaxColor + 1] = {};
    std::size_t end[boxmesh::kMaxColor + 1] = {};
    std::vector<boxmesh::Edge> edges;
    std::size_t edgeBegin[boxmesh::kMaxColor + 1] = {};
    std::size_t edgeEnd[boxmesh::kMaxColor + 1] = {};
    std::size_t cells = 0;
    std::size_t allQuads = 0;
    std::size_t allEdges = 0;
    std::size_t budget = 0;
    float quadRadius = -1.0F;
    std::vector<Segment> segments;
    bool mixed = false;
    std::size_t faceBudget = 0;
    std::vector<std::uint32_t> mixedOrder;
    bool retained = false;
    std::vector<RegionOut> regions;
    std::size_t regionFaces = 0;
    std::size_t regionLines = 0;
};
Prepared g_prepared;
std::uint64_t g_preparedSerial = 0;

bool keptAvailable()
{
    return g_meshEnd != nullptr && g_tessClear != nullptr && g_rendererFlag != nullptr
           && g_meshRender != nullptr && g_meshDestroy != nullptr
           && !g_keptBroken.load(std::memory_order_relaxed);
}

constexpr std::size_t kMixedFacesPerCall = 16383;
constexpr double kEdgeReach = 0.5;
constexpr double kRibbonPixels = 1.5;
constexpr double kRibbonNearW = 0.05;

constexpr double kReorderBlocks = 4.0;
constexpr double kResortBlocks = 1.0;

struct Merged {
    bool valid = false;
    unsigned long long version = ~0ULL;
    bool xray = false;
    std::vector<boxmesh::Quad> quads;
    std::vector<boxmesh::Edge> edges;
    std::size_t cells = 0;
};

struct Selection {
    bool valid = false;
    unsigned long long version = ~0ULL;
    bool xray = false;
    std::size_t faceBudget = 0;
    double edgeReach = 0.0;
    double eye[3] = {};
    std::vector<boxmesh::Quad> quads;
    std::vector<boxmesh::Edge> edges;
    float quadRadius = -1.0F;
    double keepRadius = -1.0;
};

struct MeshRequest {
    unsigned long long version = ~0ULL;
    bool xray = false;
    std::shared_ptr<const std::vector<blocks::DiffBox>> list;
    double eye[3] = {};
    std::size_t budget = 0;
    std::size_t faceBudget = 0;
    bool mixed = false;
    bool retained = false;
};
std::mutex g_workLock;
std::condition_variable g_workCv;
bool g_workHas = false;
bool g_workStop = false;
MeshRequest g_work;
bool g_resultReady = false;
Prepared g_result;
HANDLE g_worker = nullptr;
std::atomic<bool> g_workerRunning{false};
unsigned long long g_requestedVersion = ~0ULL;
bool g_requestedXray = false;
double g_requestedEye[3] = {};
std::size_t g_requestedBudget = 0;
std::size_t g_requestedFaceBudget = 0;
bool g_requestedMixed = false;
bool g_requestedRetained = false;
Merged g_merged;
Selection g_selection;

void mergeList(const MeshRequest& request, Merged& out)
{
    out.valid = true;
    out.version = request.version;
    out.xray = request.xray;
    out.quads.clear();
    out.edges.clear();
    out.cells = 0;
    if (!request.list || request.list->empty()) {
        return;
    }
    std::vector<boxmesh::Cell> cells;
    cells.reserve(request.list->size());
    for (const blocks::DiffBox& box : *request.list) {
        const auto color = static_cast<std::uint8_t>(box.color);
        if (color == 0 || color > boxmesh::kMaxColor) {
            continue;
        }
        cells.push_back(boxmesh::Cell{box.x, box.y, box.z, color, box.covered});
    }
    out.cells = cells.size();
    out.quads = boxmesh::build(cells, !request.xray);
    if (cells.size() > kEdgeCellLimit) {
        g_edgesSkipped.fetch_add(1, std::memory_order_relaxed);
        out.edges = boxmesh::edgesFromQuads(out.quads);
    } else {
        out.edges = boxmesh::buildEdges(
            cells, request.xray ? boxmesh::EdgeStyle::BlockAll : boxmesh::EdgeStyle::BlockSurface);
    }
}

void buildSegments(Prepared& out);

void selectPrepared(const Merged& merged, const MeshRequest& request, Selection& selection,
                    Prepared& out)
{
    out.version = request.version;
    out.xray = request.xray;
    out.cells = merged.cells;
    out.allQuads = merged.quads.size();
    out.allEdges = merged.edges.size();
    out.budget = request.budget;
    out.faceBudget = request.faceBudget;
    out.mixed = request.mixed;
    out.mixedOrder.clear();
    LARGE_INTEGER t0{};
    LARGE_INTEGER t1{};
    LARGE_INTEGER freq{};
    QueryPerformanceCounter(&t0);
    bool sortOnly = false;
    if (request.mixed) {
        const double reach = kEdgeReach;
        double moved = 0.0;
        for (int k = 0; k < 3; ++k) {
            const double d = request.eye[k] - selection.eye[k];
            moved += d * d;
        }
        const double resort = selection.keepRadius < 0.0
                                  ? kReorderBlocks
                                  : (std::min)(kReorderBlocks, selection.keepRadius * 0.25);
        sortOnly = selection.valid && selection.version == request.version
                   && selection.xray == request.xray && selection.faceBudget == request.faceBudget
                   && selection.edgeReach == reach && moved <= resort * resort;
        if (sortOnly) {
            out.quads = selection.quads;
            out.edges = selection.edges;
            out.quadRadius = selection.quadRadius;
            boxmesh::paintOrder(out.quads, out.edges, request.eye, request.faceBudget, reach,
                                out.mixedOrder);
        } else {
            out.quadRadius =
                boxmesh::copyNearest(merged.quads, request.eye, request.faceBudget, out.quads);
            boxmesh::copyNearest(merged.edges, request.eye, request.faceBudget, out.edges);
            const float radius = boxmesh::paintOrder(out.quads, out.edges, request.eye,
                                                     request.faceBudget, reach, out.mixedOrder);
            if (radius >= 0.0F) {
                out.quadRadius = radius;
            }
            selection.valid = true;
            selection.version = request.version;
            selection.xray = request.xray;
            selection.faceBudget = request.faceBudget;
            selection.edgeReach = reach;
            std::copy(request.eye, request.eye + 3, selection.eye);
            selection.quads = out.quads;
            selection.edges = out.edges;
            selection.quadRadius = out.quadRadius;
            double keep = -1.0;
            if (out.quads.size() < merged.quads.size()) {
                double farthest = 0.0;
                for (const boxmesh::Quad& quad : out.quads) {
                    farthest = (std::max)(farthest, boxmesh::distanceSq(quad, request.eye));
                }
                keep = std::sqrt(farthest);
            }
            if (reach > 0.0 && !out.edges.empty() && out.edges.size() < merged.edges.size()) {
                double farthest = 0.0;
                for (const boxmesh::Edge& edge : out.edges) {
                    farthest = (std::max)(farthest, boxmesh::distanceSq(edge, request.eye));
                }
                const double edgeKeep = std::sqrt(farthest) / reach;
                keep = keep < 0.0 ? edgeKeep : (std::min)(keep, edgeKeep);
            }
            selection.keepRadius = keep;
        }
    } else {
        out.quadRadius =
            boxmesh::copyNearest(merged.quads, request.eye, request.faceBudget, out.quads);
        boxmesh::copyNearest(merged.edges, request.eye, request.budget, out.edges);
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    const auto micros = static_cast<unsigned long long>(
        (t1.QuadPart - t0.QuadPart) * 1000000LL / (freq.QuadPart != 0 ? freq.QuadPart : 1));
    std::atomic<std::size_t>& count = sortOnly ? g_sortCount : g_selectCount;
    std::atomic<unsigned long long>& longest = sortOnly ? g_sortUsMax : g_selectUsMax;
    count.fetch_add(1, std::memory_order_relaxed);
    unsigned long long was = longest.load(std::memory_order_relaxed);
    while (micros > was && !longest.compare_exchange_weak(was, micros, std::memory_order_relaxed)) {
    }
    for (std::size_t c = 0; c <= boxmesh::kMaxColor; ++c) {
        out.begin[c] = 0;
        out.end[c] = 0;
        out.edgeBegin[c] = 0;
        out.edgeEnd[c] = 0;
    }
    for (std::size_t i = 0; i < out.quads.size(); ++i) {
        const std::size_t c = out.quads[i].color;
        if (c > boxmesh::kMaxColor) {
            continue;
        }
        if (out.end[c] == 0) {
            out.begin[c] = i;
        }
        out.end[c] = i + 1;
    }
    for (std::size_t i = 0; i < out.edges.size(); ++i) {
        const std::size_t c = out.edges[i].color;
        if (c > boxmesh::kMaxColor) {
            continue;
        }
        if (out.edgeEnd[c] == 0) {
            out.edgeBegin[c] = i;
        }
        out.edgeEnd[c] = i + 1;
    }
    buildSegments(out);
}

void buildSegments(Prepared& out)
{
    out.segments.clear();
    constexpr std::uint8_t kColorOrder[] = {2, 3, 4, 1};
    if (out.mixed) {
        if (!out.mixedOrder.empty()) {
            out.segments.push_back(
                Segment{0, false, 0, static_cast<std::uint32_t>(out.mixedOrder.size())});
        }
        return;
    }
    for (const std::uint8_t color : kColorOrder) {
        if (out.end[color] > out.begin[color]) {
            out.segments.push_back(Segment{color, false,
                                           static_cast<std::uint32_t>(out.begin[color]),
                                           static_cast<std::uint32_t>(out.end[color]
                                                                      - out.begin[color])});
        }
        if (out.edgeEnd[color] > out.edgeBegin[color]) {
            out.segments.push_back(Segment{color, true,
                                           static_cast<std::uint32_t>(out.edgeBegin[color]),
                                           static_cast<std::uint32_t>(out.edgeEnd[color]
                                                                      - out.edgeBegin[color])});
        }
    }
}

struct Regions {
    unsigned long long version = ~0ULL;
    bool xray = false;
    boxmesh::RegionSet set;
};
Regions g_regions;

void buildRegions(const MeshRequest& request, Regions& state, Prepared& out)
{
    out.version = request.version;
    out.xray = request.xray;
    out.retained = true;
    out.mixed = false;
    out.budget = request.budget;
    out.faceBudget = request.faceBudget;
    out.quads.clear();
    out.edges.clear();
    out.segments.clear();
    out.mixedOrder.clear();
    out.allQuads = 0;
    out.allEdges = 0;
    if (state.version != request.version || state.xray != request.xray) {
        const unsigned long long began = GetTickCount64();
        std::vector<boxmesh::Cell> cells;
        if (request.list) {
            cells.reserve(request.list->size());
            for (const blocks::DiffBox& box : *request.list) {
                const auto color = static_cast<std::uint8_t>(box.color);
                if (color == 0 || color > boxmesh::kMaxColor) {
                    continue;
                }
                cells.push_back(boxmesh::Cell{box.x, box.y, box.z, color, box.covered});
            }
        }
        state.set.update(cells, request.xray);
        state.version = request.version;
        state.xray = request.xray;
        const unsigned long long took = GetTickCount64() - began;
        g_groupMsLast.store(took, std::memory_order_relaxed);
        unsigned long long was = g_groupMsMax.load(std::memory_order_relaxed);
        while (took > was && !g_groupMsMax.compare_exchange_weak(was, took, std::memory_order_relaxed)) {
        }
        g_groupCount.fetch_add(1, std::memory_order_relaxed);
    }
    out.regions.clear();
    out.regions.reserve(state.set.entries().size());
    out.regionFaces = 0;
    out.regionLines = 0;
    for (const auto& [key, entry] : state.set.entries()) {
        out.regions.push_back(RegionOut{key, entry.generation, entry.geometry});
        out.regionFaces += entry.geometry->quads.size();
        out.regionLines += entry.geometry->edges.size();
    }
}

void buildPrepared(const MeshRequest& request, Merged& merged, Selection& selection, Regions& regions,
                   Prepared& out)
{
    if (request.retained) {
        buildRegions(request, regions, out);
        return;
    }
    out.retained = false;
    out.regions.clear();
    out.regionFaces = 0;
    out.regionLines = 0;
    if (!merged.valid || merged.version != request.version || merged.xray != request.xray) {
        const unsigned long long began = GetTickCount64();
        mergeList(request, merged);
        const unsigned long long took = GetTickCount64() - began;
        g_mergeMsLast.store(took, std::memory_order_relaxed);
        unsigned long long was = g_mergeMsMax.load(std::memory_order_relaxed);
        while (took > was
               && !g_mergeMsMax.compare_exchange_weak(was, took, std::memory_order_relaxed)) {
        }
        g_mergeCount.fetch_add(1, std::memory_order_relaxed);
    }
    selectPrepared(merged, request, selection, out);
}

bool buildPreparedSafely(const MeshRequest& request, Merged& merged, Selection& selection, Regions& regions,
                         Prepared& out)
{
    try {
        buildPrepared(request, merged, selection, regions, out);
        return true;
    } catch (const std::exception&) {
        merged = Merged{};
        selection = Selection{};
        regions = Regions{};
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) {
            log().warn(L"WorldMesh: preparing the boxes ran out of memory ({} box(es) in the list); nothing is "
                       L"drawn until the list changes",
                       request.list ? request.list->size() : 0);
        }
        return false;
    }
}

Prepared emptyPrepared(const MeshRequest& request)
{
    Prepared out;
    out.version = request.version;
    out.xray = request.xray;
    out.budget = request.budget;
    out.faceBudget = request.faceBudget;
    out.mixed = request.retained ? false : request.mixed;
    out.retained = request.retained;
    return out;
}

DWORD WINAPI workerLoop(LPVOID)
{
    for (;;) {
        MeshRequest request;
        {
            std::unique_lock<std::mutex> lock(g_workLock);
            g_workCv.wait(lock, [] { return g_workStop || g_workHas; });
            if (g_workStop) {
                return 0;
            }
            request = std::move(g_work);
            g_workHas = false;
        }
        Prepared made;
        if (!buildPreparedSafely(request, g_merged, g_selection, g_regions, made)) {
            made = emptyPrepared(request);
        }
        {
            std::lock_guard<std::mutex> lock(g_workLock);
            g_result = std::move(made);
            g_resultReady = true;
        }
    }
}

std::size_t frameBudget()
{
    return static_cast<std::size_t>((std::max)(1, kDefaultMaxBoxes));
}

bool needReorder(const double eye[3])
{
    if (g_prepared.retained) {
        return false;
    }
    if (g_prepared.allQuads == 0 && g_prepared.allEdges == 0) {
        return false;
    }
    const bool fits =
        g_prepared.allQuads <= g_prepared.faceBudget && g_prepared.allEdges <= g_prepared.budget;
    if (fits && !g_prepared.mixed) {
        return false;
    }
    double step = kReorderBlocks;
    if (g_prepared.mixed) {
        step = kResortBlocks;
    } else if (g_prepared.quadRadius >= 0.0F) {
        step = std::clamp(static_cast<double>(g_prepared.quadRadius) * 0.25, 0.5, kReorderBlocks);
    }
    double d2 = 0.0;
    for (int k = 0; k < 3; ++k) {
        const double d = eye[k] - g_requestedEye[k];
        d2 += d * d;
    }
    return d2 > step * step;
}

void prepare(bool xray, const double cam[3], bool borrowed, std::size_t quadsPerCall)
{
    const unsigned long long version = boxes::boxVersion();
    const Choice* const flatChoice = choiceOf(kDefaultFlat);
    const Choice* const xrayChoice = choiceOf(kDefaultXray);
    const bool retained =
        keptAvailable()
        && (xray ? (xrayChoice != nullptr && !xrayChoice->multiply && xrayChoice->mode != kLineMode)
                 : (flatChoice != nullptr && flatChoice->multiply));
    const std::size_t budget = frameBudget();
    const bool mixed = xray;
    const std::size_t faceBudget =
        mixed ? (std::min)(budget, (std::min)(quadsPerCall, kMixedFacesPerCall)) : budget;
    double eye[3] = {cam[0], cam[1], cam[2]};
    if (borrowed) {
        std::copy(g_requestedEye, g_requestedEye + 3, eye);
    }
    const bool reorder = !borrowed && needReorder(eye);
    if (!g_workerRunning.load(std::memory_order_acquire)) {
        static Merged merged;
        static Selection selection;
        static Regions regions;
        if (version != g_prepared.version || xray != g_prepared.xray
            || budget != g_prepared.budget || faceBudget != g_prepared.faceBudget
            || (!retained && mixed != g_prepared.mixed) || retained != g_prepared.retained || reorder) {
            MeshRequest request;
            request.version = version;
            request.xray = xray;
            request.list = boxes::boxSnapshot();
            std::copy(eye, eye + 3, request.eye);
            request.budget = budget;
            request.faceBudget = faceBudget;
            request.mixed = mixed;
            request.retained = retained;
            std::copy(eye, eye + 3, g_requestedEye);
            if (!buildPreparedSafely(request, merged, selection, regions, g_prepared)) {
                g_prepared = emptyPrepared(request);
            }
            ++g_preparedSerial;
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_workLock);
        if (g_resultReady) {
            g_prepared = std::move(g_result);
            g_resultReady = false;
            ++g_preparedSerial;
        }
    }
    if (version != g_requestedVersion || xray != g_requestedXray || budget != g_requestedBudget
        || faceBudget != g_requestedFaceBudget || mixed != g_requestedMixed
        || retained != g_requestedRetained || reorder) {
        g_requestedVersion = version;
        g_requestedXray = xray;
        g_requestedBudget = budget;
        g_requestedFaceBudget = faceBudget;
        g_requestedMixed = mixed;
        g_requestedRetained = retained;
        std::copy(eye, eye + 3, g_requestedEye);
        {
            std::lock_guard<std::mutex> lock(g_workLock);
            g_work.version = version;
            g_work.xray = xray;
            g_work.list = boxes::boxSnapshot();
            std::copy(eye, eye + 3, g_work.eye);
            g_work.budget = budget;
            g_work.faceBudget = faceBudget;
            g_work.mixed = mixed;
            g_work.retained = retained;
            g_workHas = true;
        }
        g_workCv.notify_one();
    }
}

void startWorker()
{
    g_worker = CreateThread(nullptr, 0, &workerLoop, nullptr, 0, nullptr);
    if (g_worker == nullptr) {
        log().warn(L"WorldMesh: could not start the merge thread (merging on the render thread "
                   L"instead)");
        return;
    }
    g_workerRunning.store(true, std::memory_order_release);
}

void stopWorker()
{
    if (!g_workerRunning.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_workLock);
        g_workStop = true;
    }
    g_workCv.notify_one();
    if (g_worker != nullptr) {
        if (WaitForSingleObject(g_worker, 10000) != WAIT_OBJECT_0) {
            log().warn(L"WorldMesh: the merge thread did not stop within 10 seconds");
        }
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
}

struct Job {
    void* ctx;
    void* tess;
    const void* material;
    double cam[3];
    const boxmesh::Quad* quads;
    std::size_t count;
    const boxmesh::Edge* edges;
    std::size_t edgeCount;
    float rgba[4];
    float inflate;
    bool vertexColor;
    bool flip;
    std::uint32_t limit;
    bool perQuadColor;
    float palette[boxmesh::kMaxColor + 1][4];
    std::uint32_t facesOf[boxmesh::kMaxColor + 1];
    const std::uint32_t* order;
    float linePalette[boxmesh::kMaxColor + 1][4];
    double ribbonScale;
    double forward[3];
    double forwardW;
    bool haveForward;
    std::uint32_t ribbons;
    std::uint32_t batches;
    std::uint32_t faces;
    bool busy;
    bool failed;
    DWORD code;
};

void emitRect(const Job& job, const boxmesh::Quad& quad)
{
    static constexpr float kNormal[6][3] = {
        {0.0F, -1.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, -1.0F},
        {0.0F, 0.0F, 1.0F},  {-1.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F},
    };
    std::int32_t c[4][3] = {};
    boxmesh::corners(quad, c);
    const float e = job.inflate;
    const float* const n = kNormal[quad.dir % 6];
    float v[4][3] = {};
    for (int i = 0; i < 4; ++i) {
        for (int k = 0; k < 3; ++k) {
            const std::int64_t twice = static_cast<std::int64_t>(c[0][k]) + c[2][k];
            const std::int64_t mine = 2LL * c[i][k];
            const float spread = mine > twice ? 1.0F : (mine < twice ? -1.0F : 0.0F);
            const double at = static_cast<double>(c[i][k]) + static_cast<double>(n[k] * e + spread * e);
            v[i][k] = static_cast<float>(at - job.cam[k]);
        }
    }
    void* const tess = job.tess;
    if (!job.flip) {
        for (int i = 0; i < 4; ++i) {
            g_vertex(tess, v[i][0], v[i][1], v[i][2]);
        }
    } else {
        for (int i = 3; i >= 0; --i) {
            g_vertex(tess, v[i][0], v[i][1], v[i][2]);
        }
    }
}

bool emitRibbon(const Job& job, const boxmesh::Edge& edge)
{
    std::int32_t ends[2][3] = {};
    boxmesh::edgeEnds(edge, ends);
    double p[2][3] = {};
    for (int i = 0; i < 2; ++i) {
        for (int k = 0; k < 3; ++k) {
            p[i][k] = static_cast<double>(ends[i][k]) - job.cam[k];
        }
    }
    double v[4][3] = {};
    if (!boxmesh::ribbonCorners(p[0], p[1], job.haveForward ? job.forward : nullptr, job.forwardW,
                                job.ribbonScale, kRibbonNearW, v)) {
        return false;
    }
    void* const tess = job.tess;
    if (job.flip) {
        for (int i = 0; i < 4; ++i) {
            g_vertex(tess, static_cast<float>(v[i][0]), static_cast<float>(v[i][1]),
                     static_cast<float>(v[i][2]));
        }
    } else {
        for (int i = 3; i >= 0; --i) {
            g_vertex(tess, static_cast<float>(v[i][0]), static_cast<float>(v[i][1]),
                     static_cast<float>(v[i][2]));
        }
    }
    return true;
}

__declspec(noinline) void runJob(Job& job)
{
    __try {
        auto* const tess = static_cast<unsigned char*>(job.tess);
        float* color = nullptr;
        std::memcpy(&color, static_cast<unsigned char*>(job.ctx) + kCtxColor, sizeof(color));
        std::size_t at = 0;
        while (at < job.count) {
            if (tess[kTessBuilding] != 0 || tess[kTessVoid] != 0) {
                job.busy = true;
                return;
            }
            g_begin(tess, nullptr, kPrimitiveQuads, 0, false);
            if (tess[kTessBuilding] == 0) {
                job.busy = true;
                return;
            }
            const float zero[3] = {0.0F, 0.0F, 0.0F};
            const float one[3] = {1.0F, 1.0F, 1.0F};
            std::memcpy(tess + kTessOffset, zero, sizeof(zero));
            std::memcpy(tess + kTessScale, one, sizeof(one));
            tess[kTessUseMatrix] = 0;
            tess[kTessNoColor] = (job.vertexColor || job.perQuadColor) ? 0 : 1;
            if (job.vertexColor) {
                g_color(tess, job.rgba[0], job.rgba[1], job.rgba[2], job.rgba[3]);
            }
            std::uint8_t current = 0xFF;
            std::uint32_t used = 0;
            for (; at < job.count; ++at) {
                if (used + 4U > job.limit && used != 0) {
                    break;
                }
                if (job.perQuadColor) {
                    const std::uint32_t item = job.order[at];
                    const bool isEdge = (item & boxmesh::kEdgeItem) != 0;
                    const std::uint32_t index = item & ~boxmesh::kEdgeItem;
                    const std::uint8_t raw = isEdge ? job.edges[index].color : job.quads[index].color;
                    const std::uint8_t quadColor = raw <= boxmesh::kMaxColor ? raw : 0;
                    const auto key = static_cast<std::uint8_t>(quadColor | (isEdge ? 0x80U : 0U));
                    if (key != current) {
                        const float* const rgba =
                            isEdge ? job.linePalette[quadColor] : job.palette[quadColor];
                        g_color(tess, rgba[0], rgba[1], rgba[2], rgba[3]);
                        current = key;
                    }
                    if (isEdge) {
                        if (emitRibbon(job, job.edges[index])) {
                            used += 4U;
                            ++job.ribbons;
                        }
                    } else {
                        emitRect(job, job.quads[index]);
                        used += 4U;
                        ++job.faces;
                        ++job.facesOf[quadColor];
                    }
                    continue;
                }
                emitRect(job, job.quads[at]);
                used += 4U;
                ++job.faces;
            }
            if (used == 0) {
                for (int i = 0; i < 4; ++i) {
                    g_vertex(tess, 0.0F, 0.0F, 0.0F);
                }
            }
            if (color != nullptr) {
                std::memcpy(color, job.rgba, sizeof(job.rgba));
                reinterpret_cast<unsigned char*>(color)[0x10] = 1;
            }
            alignas(16) unsigned char texture[0x40] = {};
            g_render(job.ctx, tess, job.material, texture);
            ++job.batches;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job.failed = true;
        job.code = GetExceptionCode();
    }
}

void emitEdge(const Job& job, const boxmesh::Edge& edge)
{
    std::int32_t ends[2][3] = {};
    boxmesh::edgeEnds(edge, ends);
    for (int i = 0; i < 2; ++i) {
        g_vertex(job.tess,
                 static_cast<float>(static_cast<double>(ends[i][0]) - job.cam[0]),
                 static_cast<float>(static_cast<double>(ends[i][1]) - job.cam[1]),
                 static_cast<float>(static_cast<double>(ends[i][2]) - job.cam[2]));
    }
}

__declspec(noinline) void runLineJob(Job& job)
{
    __try {
        auto* const tess = static_cast<unsigned char*>(job.tess);
        float* color = nullptr;
        std::memcpy(&color, static_cast<unsigned char*>(job.ctx) + kCtxColor, sizeof(color));
        const std::uint32_t limit = job.limit & ~1U;
        std::size_t at = 0;
        while (at < job.edgeCount) {
            if (tess[kTessBuilding] != 0 || tess[kTessVoid] != 0) {
                job.busy = true;
                return;
            }
            g_begin(tess, nullptr, kPrimitiveLines, 0, false);
            if (tess[kTessBuilding] == 0) {
                job.busy = true;
                return;
            }
            const float zero[3] = {0.0F, 0.0F, 0.0F};
            const float one[3] = {1.0F, 1.0F, 1.0F};
            std::memcpy(tess + kTessOffset, zero, sizeof(zero));
            std::memcpy(tess + kTessScale, one, sizeof(one));
            tess[kTessUseMatrix] = 0;
            tess[kTessNoColor] = job.vertexColor ? 0 : 1;
            if (job.vertexColor) {
                g_color(tess, job.rgba[0], job.rgba[1], job.rgba[2], job.rgba[3]);
            }
            std::uint32_t used = 0;
            for (; at < job.edgeCount; ++at) {
                if (used + 2U > limit && used != 0) {
                    break;
                }
                emitEdge(job, job.edges[at]);
                used += 2U;
                ++job.faces;
            }
            if (color != nullptr) {
                std::memcpy(color, job.rgba, sizeof(job.rgba));
                reinterpret_cast<unsigned char*>(color)[0x10] = 1;
            }
            alignas(16) unsigned char texture[0x40] = {};
            g_render(job.ctx, tess, job.material, texture);
            ++job.batches;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job.failed = true;
        job.code = GetExceptionCode();
    }
}

struct TessSaved {
    unsigned char offsetScale[24];
    unsigned char useMatrix;
    unsigned char noColor;
    float color[4];
    unsigned char colorDirty;
    bool haveColor;
};

__declspec(noinline) bool saveState(void* ctx, void* tess, TessSaved& saved)
{
    __try {
        auto* const t = static_cast<unsigned char*>(tess);
        std::memcpy(saved.offsetScale, t + kTessOffset, sizeof(saved.offsetScale));
        saved.useMatrix = t[kTessUseMatrix];
        saved.noColor = t[kTessNoColor];
        float* color = nullptr;
        std::memcpy(&color, static_cast<unsigned char*>(ctx) + kCtxColor, sizeof(color));
        saved.haveColor = color != nullptr;
        if (color != nullptr) {
            std::memcpy(saved.color, color, sizeof(saved.color));
            saved.colorDirty = reinterpret_cast<unsigned char*>(color)[0x10];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) void restoreState(void* ctx, void* tess, const TessSaved& saved)
{
    __try {
        auto* const t = static_cast<unsigned char*>(tess);
        std::memcpy(t + kTessOffset, saved.offsetScale, sizeof(saved.offsetScale));
        t[kTessUseMatrix] = saved.useMatrix;
        t[kTessNoColor] = saved.noColor;
        if (saved.haveColor) {
            float* color = nullptr;
            std::memcpy(&color, static_cast<unsigned char*>(ctx) + kCtxColor, sizeof(color));
            if (color != nullptr) {
                std::memcpy(color, saved.color, sizeof(saved.color));
                reinterpret_cast<unsigned char*>(color)[0x10] = 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

__declspec(noinline) bool readTopMatrix(void* ctx, float out[16])
{
    __try {
        unsigned char* stack = nullptr;
        std::memcpy(&stack, static_cast<unsigned char*>(ctx) + kCtxMatrices, sizeof(stack));
        if (stack == nullptr) {
            return false;
        }
        std::uint64_t map = 0;
        std::uint64_t mapSize = 0;
        std::uint64_t first = 0;
        std::uint64_t size = 0;
        std::memcpy(&map, stack + 0x48, 8);
        std::memcpy(&mapSize, stack + 0x50, 8);
        std::memcpy(&first, stack + 0x58, 8);
        std::memcpy(&size, stack + 0x60, 8);
        if (map == 0 || mapSize == 0 || size == 0 || (mapSize & (mapSize - 1)) != 0) {
            return false;
        }
        const std::uint64_t index = (first + size - 1) & (mapSize - 1);
        const float* top = nullptr;
        std::memcpy(&top, reinterpret_cast<const unsigned char*>(map) + index * 8, sizeof(top));
        if (top == nullptr) {
            return false;
        }
        std::memcpy(out, top, sizeof(float) * 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct alignas(16) MeshStorage {
    unsigned char bytes[0x400];
};
struct KeptMesh {
    std::unique_ptr<MeshStorage> storage;
    std::uint8_t color = 0;
    bool line = false;
    std::uint32_t count = 0;
};
struct KeptRegion {
    std::uint64_t generation = 0;
    std::int32_t origin[3] = {};
    bool xray = false;
    std::vector<KeptMesh> meshes;
    std::shared_ptr<const boxmesh::RegionGeometry> geometry;
    std::int32_t refBlock[3] = {};
    std::int8_t refSide[3] = {};
    double refDistance = 0.0;
    double refDir[3] = {};
    std::uint64_t palette = 0;
    double ribbonScale = 0.0;
};
std::unordered_map<boxmesh::RegionKey, KeptRegion, boxmesh::RegionKeyHash>& g_kept =
    *new std::unordered_map<boxmesh::RegionKey, KeptRegion, boxmesh::RegionKeyHash>();
std::unordered_map<boxmesh::RegionKey, RegionOut, boxmesh::RegionKeyHash>& g_keptPending =
    *new std::unordered_map<boxmesh::RegionKey, RegionOut, boxmesh::RegionKeyHash>();
std::uint64_t g_keptSyncedSerial = ~0ULL;
std::size_t g_keptMeshes = 0;
std::size_t g_keptFaces = 0;
std::size_t g_keptLines = 0;
constexpr unsigned long long kKeptRebuildBudgetUs = 3000;
constexpr double kKeptNearOrderBlocks = 20.0;
constexpr double kKeptWidthRatio = 1.5;
constexpr double kKeptTurnCos = 0.9659;
constexpr double kKeptSlabOrderBlocks = 64.0;
constexpr double kKeptScaleRatio = 1.25;
constexpr double kVpDirectionBlocks = 4.0;
constexpr double kSortBase = 1.0;
constexpr double kSortSpan = 32.0;
constexpr unsigned kKeptStallFrames = 600;
unsigned g_keptStall = 0;
float g_keptInflate = 0.0F;
bool g_keptFlip = false;
float g_keptXrayPalette[2][boxmesh::kMaxColor + 1][4] = {};
std::uint64_t g_keptPaletteVersion = 1;
std::atomic<bool> g_keptReleaseAsked{false};
std::atomic<bool> g_keptReleased{false};
enum class KeptWhy : std::uint8_t {
    Released,
    BoxesOff,
    ModeOff,
    Rebake,
    Broken,
    NotKept,
};
std::atomic<std::size_t> g_keptLastReturned{0};
std::atomic<KeptWhy> g_keptLastWhy{KeptWhy::Released};
std::atomic<unsigned long long> g_keptReordered{0};
std::atomic<unsigned long long> g_keptUnordered{0};
std::atomic<bool> g_keptLastXray{false};

struct MeshName {
    const char* text;
    std::size_t length;
};
MeshName g_meshName{nullptr, 0};

alignas(16) float g_sortMatrix[16] = {};
struct TopMatrix {
    float* top = nullptr;
    float* pushed = nullptr;
    float savedTop[16] = {};
    float savedPushed[16] = {};
    unsigned char* stack = nullptr;
    float* slot = nullptr;
    float savedSlot[3] = {};
    std::uint64_t slotIndex = 0;
    std::uint64_t savedPushedIndex = 0;
    unsigned char savedFlag = 0;
    bool sortControl = false;
    std::uint64_t* mapEntry = nullptr;
    std::uint64_t savedMapEntry = 0;
    std::uint64_t mapBase = 0;
    std::uint64_t stackSize = 0;
    std::uint64_t mapSize = 0;
};

__declspec(noinline) bool findTopMatrix(void* ctx, TopMatrix& m)
{
    __try {
        unsigned char* stack = nullptr;
        std::memcpy(&stack, static_cast<unsigned char*>(ctx) + kCtxMatrices, sizeof(stack));
        if (stack == nullptr) {
            return false;
        }
        std::uint64_t map = 0;
        std::uint64_t mapSize = 0;
        std::uint64_t first = 0;
        std::uint64_t size = 0;
        std::uint64_t pushedIndex = 0;
        std::memcpy(&map, stack + 0x48, 8);
        std::memcpy(&mapSize, stack + 0x50, 8);
        std::memcpy(&first, stack + 0x58, 8);
        std::memcpy(&size, stack + 0x60, 8);
        std::memcpy(&pushedIndex, stack + 0x68, 8);
        if (map == 0 || mapSize == 0 || size == 0 || (mapSize & (mapSize - 1)) != 0) {
            return false;
        }
        const std::uint64_t mask = mapSize - 1;
        const std::uint64_t topIndex = (first + size - 1) & mask;
        std::memcpy(&m.top, reinterpret_cast<const unsigned char*>(map) + topIndex * 8, sizeof(m.top));
        m.pushed = nullptr;
        if (stack[0x70] == 1) {
            const std::uint64_t index = (first + pushedIndex) & mask;
            std::memcpy(&m.pushed, reinterpret_cast<const unsigned char*>(map) + index * 8,
                        sizeof(m.pushed));
            if (m.pushed == m.top) {
                m.pushed = nullptr;
            }
        }
        if (m.top == nullptr || !memory::isWritable(m.top, sizeof(float) * 16)
            || (m.pushed != nullptr && !memory::isWritable(m.pushed, sizeof(float) * 16))) {
            return false;
        }
        std::memcpy(m.savedTop, m.top, sizeof(m.savedTop));
        if (m.pushed != nullptr) {
            std::memcpy(m.savedPushed, m.pushed, sizeof(m.savedPushed));
        }
        m.stack = stack;
        m.savedFlag = stack[0x70];
        m.savedPushedIndex = pushedIndex;
        m.sortControl = false;
        m.slot = nullptr;
        m.mapEntry = nullptr;
        m.mapBase = map;
        m.stackSize = size;
        m.mapSize = mapSize;
        if (size < mapSize) {
            auto* const entry =
                reinterpret_cast<std::uint64_t*>(static_cast<std::uintptr_t>(map) + ((first + size) & mask) * 8);
            if (memory::isWritable(entry, sizeof(std::uint64_t))) {
                m.mapEntry = entry;
                m.savedMapEntry = *entry;
                std::memcpy(g_sortMatrix, m.savedTop, sizeof(g_sortMatrix));
                m.slot = g_sortMatrix;
                m.slotIndex = size;
                m.sortControl = true;
            }
        }
        if (!m.sortControl && size >= 2) {
            m.slotIndex = size - 2;
            std::memcpy(&m.slot, reinterpret_cast<const unsigned char*>(map) + ((first + m.slotIndex) & mask) * 8,
                        sizeof(m.slot));
            if (m.slot != nullptr && m.slot != m.top && memory::isWritable(m.slot, sizeof(float) * 16)) {
                std::memcpy(m.savedSlot, m.slot + 12, sizeof(m.savedSlot));
                m.sortControl = true;
            } else {
                m.slot = nullptr;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) void restoreSortSlot(TopMatrix& m)
{
    __try {
        if (m.mapEntry == nullptr) {
            std::memcpy(m.slot + 12, m.savedSlot, sizeof(m.savedSlot));
            return;
        }
        std::uint64_t map = 0;
        std::uint64_t mapSize = 0;
        std::memcpy(&map, m.stack + 0x48, 8);
        std::memcpy(&mapSize, m.stack + 0x50, 8);
        if (map == m.mapBase && mapSize == m.mapSize) {
            *m.mapEntry = m.savedMapEntry;
            return;
        }
        const auto mine = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(g_sortMatrix));
        auto* const entries = reinterpret_cast<std::uint64_t*>(static_cast<std::uintptr_t>(map));
        if (map != 0 && mapSize != 0 && mapSize <= (1ULL << 20)
            && memory::isWritable(entries, static_cast<std::size_t>(mapSize) * sizeof(std::uint64_t))) {
            for (std::uint64_t i = 0; i < mapSize; ++i) {
                if (entries[i] == mine) {
                    entries[i] = m.savedMapEntry;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

__declspec(noinline) void restoreSortFlag(TopMatrix& m)
{
    __try {
        m.stack[0x70] = m.savedFlag;
        std::memcpy(m.stack + 0x68, &m.savedPushedIndex, sizeof(m.savedPushedIndex));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

__declspec(noinline) void restoreTranslation(TopMatrix& m)
{
    __try {
        std::memcpy(m.top + 12, m.savedTop + 12, sizeof(float) * 3);
        if (m.pushed != nullptr) {
            std::memcpy(m.pushed + 12, m.savedPushed + 12, sizeof(float) * 3);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void restoreTopMatrix(TopMatrix& m)
{
    if (m.sortControl) {
        restoreSortSlot(m);
        restoreSortFlag(m);
    }
    restoreTranslation(m);
}

struct KeptBuild {
    void* tess;
    double origin[3];
    double eye[3];
    float inflate;
    bool flip;
    int kind;
    const boxmesh::Quad* quads;
    const boxmesh::Edge* edges;
    const std::uint32_t* order;
    std::uint32_t first;
    std::uint32_t count;
    float palette[boxmesh::kMaxColor + 1][4];
    float linePalette[boxmesh::kMaxColor + 1][4];
    double ribbonScale;
    std::uint32_t made;
};

bool emitRibbonAround(const Job& job, const boxmesh::Edge& edge, const double eye[3], double scale)
{
    std::int32_t ends[2][3] = {};
    boxmesh::edgeEnds(edge, ends);
    double p[2][3] = {};
    for (int i = 0; i < 2; ++i) {
        for (int k = 0; k < 3; ++k) {
            p[i][k] = static_cast<double>(ends[i][k]) - eye[k];
        }
    }
    double v[4][3] = {};
    if (!boxmesh::ribbonCornersAround(p[0], p[1], scale, v)) {
        return false;
    }
    void* const tess = job.tess;
    const auto put = [&](int i) {
        g_vertex(tess, static_cast<float>(v[i][0] + eye[0] - job.cam[0]),
                 static_cast<float>(v[i][1] + eye[1] - job.cam[1]),
                 static_cast<float>(v[i][2] + eye[2] - job.cam[2]));
    };
    if (job.flip) {
        for (int i = 0; i < 4; ++i) {
            put(i);
        }
    } else {
        for (int i = 3; i >= 0; --i) {
            put(i);
        }
    }
    return true;
}

__declspec(noinline) bool makeKeptMesh(KeptBuild& b, void* outMesh, bool& busy)
{
    __try {
        auto* const t = static_cast<unsigned char*>(b.tess);
        if (t[kTessBuilding] != 0 || t[kTessVoid] != 0) {
            busy = true;
            return false;
        }
        g_begin(b.tess, nullptr, b.kind == 1 ? kPrimitiveLines : kPrimitiveQuads, 0, false);
        if (t[kTessBuilding] == 0) {
            busy = true;
            return false;
        }
        const float zero[3] = {0.0F, 0.0F, 0.0F};
        const float one[3] = {1.0F, 1.0F, 1.0F};
        std::memcpy(t + kTessOffset, zero, sizeof(zero));
        std::memcpy(t + kTessScale, one, sizeof(one));
        t[kTessUseMatrix] = 0;
        t[kTessNoColor] = b.kind == 2 ? 0 : 1;
        Job job{};
        job.tess = b.tess;
        job.cam[0] = b.origin[0];
        job.cam[1] = b.origin[1];
        job.cam[2] = b.origin[2];
        job.inflate = b.inflate;
        job.flip = b.flip;
        b.made = 0;
        const std::uint32_t end = b.first + b.count;
        if (b.kind == 0) {
            for (std::uint32_t i = b.first; i < end; ++i) {
                emitRect(job, b.quads[i]);
                ++b.made;
            }
        } else if (b.kind == 1) {
            for (std::uint32_t i = b.first; i < end; ++i) {
                emitEdge(job, b.edges[i]);
                ++b.made;
            }
        } else {
            std::uint8_t current = 0xFF;
            for (std::uint32_t i = b.first; i < end; ++i) {
                const std::uint32_t item = b.order[i];
                const bool isEdge = (item & boxmesh::kEdgeItem) != 0;
                const std::uint32_t index = item & ~boxmesh::kEdgeItem;
                const std::uint8_t raw = isEdge ? b.edges[index].color : b.quads[index].color;
                const std::uint8_t color = raw <= boxmesh::kMaxColor ? raw : 0;
                const auto key = static_cast<std::uint8_t>(color | (isEdge ? 0x80U : 0U));
                if (key != current) {
                    const float* const rgba = isEdge ? b.linePalette[color] : b.palette[color];
                    g_color(b.tess, rgba[0], rgba[1], rgba[2], rgba[3]);
                    current = key;
                }
                if (isEdge) {
                    if (emitRibbonAround(job, b.edges[index], b.eye, b.ribbonScale)) {
                        ++b.made;
                    }
                } else {
                    emitRect(job, b.quads[index]);
                    ++b.made;
                }
            }
        }
        if (b.made == 0) {
            const int n = b.kind == 1 ? 2 : 4;
            for (int i = 0; i < n; ++i) {
                g_vertex(b.tess, 0.0F, 0.0F, 0.0F);
            }
        }
        MeshName name = g_meshName;
        const unsigned int flag = g_rendererFlag() & 0xFFU;
        g_meshEnd(b.tess, outMesh, 0, &name, flag);
        std::uint32_t zero32 = 0;
        std::uint64_t zero64 = 0;
        std::memcpy(t + 0x298, &zero32, sizeof(zero32));
        t[0x255] = 0;
        t[0x2a0] = 0;
        t[0x1ca] = 0;
        g_tessClear(t + 8);
        t[0] = 0;
        std::memcpy(t + 0x170, &zero64, sizeof(zero64));
        t[0x1b4] = 0;
        t[0x188] = 0;
        t[0x1ba] = 0;
        t[0x168] = 0;
        t[0x194] = 0;
        t[0x1a0] = 0;
        t[0x1ac] = 0;
        t[0x1be] = 0;
        t[0x1c9] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) void resetTessellator(void* tess)
{
    __try {
        auto* const t = static_cast<unsigned char*>(tess);
        t[kTessBuilding] = 0;
        t[0x255] = 0;
        g_tessClear(t + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

__declspec(noinline) void destroyKeptMesh(void* mesh)
{
    __try {
        g_meshDestroy(mesh);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

__declspec(noinline) bool renderKeptMesh(void* ctx, const void* material, TopMatrix& m, void* mesh,
                                         const float rgba[4], const float position[3], const float sortPos[3])
{
    __try {
        float* color = nullptr;
        std::memcpy(&color, static_cast<unsigned char*>(ctx) + kCtxColor, sizeof(color));
        if (color != nullptr) {
            std::memcpy(color, rgba, sizeof(float) * 4);
            reinterpret_cast<unsigned char*>(color)[0x10] = 1;
        }
        for (int k = 0; k < 3; ++k) {
            m.top[12 + k] = m.savedTop[12 + k] + position[0] * m.savedTop[k]
                            + position[1] * m.savedTop[4 + k] + position[2] * m.savedTop[8 + k];
        }
        if (m.sortControl) {
            if (m.mapEntry != nullptr) {
                *m.mapEntry = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(m.slot));
            }
            for (int k = 0; k < 3; ++k) {
                m.slot[12 + k] = m.savedTop[12 + k] + sortPos[0] * m.savedTop[k]
                                 + sortPos[1] * m.savedTop[4 + k] + sortPos[2] * m.savedTop[8 + k];
            }
            m.stack[0x70] = 1;
            std::memcpy(m.stack + 0x68, &m.slotIndex, sizeof(m.slotIndex));
        } else if (m.pushed != nullptr) {
            for (int k = 0; k < 3; ++k) {
                m.pushed[12 + k] = m.savedPushed[12 + k] + position[0] * m.savedPushed[k]
                                   + position[1] * m.savedPushed[4 + k] + position[2] * m.savedPushed[8 + k];
            }
        }
        alignas(16) unsigned char params[0x150] = {};
        alignas(16) unsigned char optional[0x50] = {};
        alignas(16) unsigned char texture[0x40] = {};
        g_meshRender(mesh, static_cast<unsigned char*>(ctx) + 0x10, material, params, nullptr, nullptr,
                     texture, nullptr, optional);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void destroyKeptRegion(KeptRegion& region)
{
    for (KeptMesh& mesh : region.meshes) {
        if (mesh.storage) {
            destroyKeptMesh(mesh.storage->bytes);
        }
        if (mesh.line) {
            g_keptLines -= (std::min)(g_keptLines, static_cast<std::size_t>(mesh.count));
        } else {
            g_keptFaces -= (std::min)(g_keptFaces, static_cast<std::size_t>(mesh.count));
        }
    }
    g_keptMeshes -= (std::min)(g_keptMeshes, region.meshes.size());
    region.meshes.clear();
}

void destroyKept(KeptWhy why)
{
    if (g_keptMeshes != 0) {
        g_keptLastReturned.store(g_keptMeshes, std::memory_order_relaxed);
        g_keptLastWhy.store(why, std::memory_order_relaxed);
    }
    for (auto& [key, region] : g_kept) {
        destroyKeptRegion(region);
    }
    g_kept.clear();
    g_keptPending.clear();
    g_keptSyncedSerial = ~0ULL;
    g_keptStall = 0;
    g_keptMeshes = 0;
    g_keptFaces = 0;
    g_keptLines = 0;
    g_keptRegionCount.store(0, std::memory_order_relaxed);
    g_keptMeshCount.store(0, std::memory_order_relaxed);
    g_keptFaceCount.store(0, std::memory_order_relaxed);
    g_keptLineCount.store(0, std::memory_order_relaxed);
}

void keptBroke(const wchar_t* where)
{
    if (!g_keptBroken.exchange(true)) {
        log().warn(L"WorldMesh: a kept mesh failed ({}). Boxes go back to the immediate drawing", where);
    }
    destroyKept(KeptWhy::Broken);
}

double regionBoxDistance(const std::int32_t origin[3], const double cam[3])
{
    double d2 = 0.0;
    for (int k = 0; k < 3; ++k) {
        const double lo = origin[k];
        const double hi = origin[k] + static_cast<double>(boxmesh::kRegionSize);
        const double d = cam[k] < lo ? lo - cam[k] : (cam[k] > hi ? cam[k] - hi : 0.0);
        d2 += d * d;
    }
    return std::sqrt(d2);
}

void regionSide(const std::int32_t origin[3], const double cam[3], std::int8_t out[3])
{
    for (int k = 0; k < 3; ++k) {
        out[k] = cam[k] < origin[k]
                     ? static_cast<std::int8_t>(-1)
                     : (cam[k] >= origin[k] + static_cast<double>(boxmesh::kRegionSize) ? static_cast<std::int8_t>(1)
                                                                                     : static_cast<std::int8_t>(0));
    }
}

void regionDirection(const std::int32_t origin[3], const double cam[3], double out[3])
{
    double v[3] = {};
    double n = 0.0;
    for (int k = 0; k < 3; ++k) {
        v[k] = cam[k] - (origin[k] + boxmesh::kRegionSize * 0.5);
        n += v[k] * v[k];
    }
    n = std::sqrt(n);
    for (int k = 0; k < 3; ++k) {
        out[k] = n > 1e-9 ? v[k] / n : 0.0;
    }
}

void syncKept()
{
    if (g_keptSyncedSerial == g_preparedSerial) {
        return;
    }
    g_keptSyncedSerial = g_preparedSerial;
    std::unordered_set<boxmesh::RegionKey, boxmesh::RegionKeyHash> alive;
    alive.reserve(g_prepared.regions.size());
    g_keptPending.clear();
    for (const RegionOut& region : g_prepared.regions) {
        alive.insert(region.key);
        const auto it = g_kept.find(region.key);
        if (it == g_kept.end() || it->second.generation != region.generation
            || it->second.xray != g_prepared.xray) {
            g_keptPending[region.key] = region;
        } else if (it->second.geometry != region.geometry) {
            it->second.geometry = region.geometry;
        }
    }
    for (auto it = g_kept.begin(); it != g_kept.end();) {
        if (alive.find(it->first) == alive.end()) {
            destroyKeptRegion(it->second);
            it = g_kept.erase(it);
        } else {
            ++it;
        }
    }
}

struct KeptFrame {
    bool xray = false;
    double cam[3] = {};
    float inflate = 0.0F;
    bool flip = false;
    std::uint32_t limit = 0;
    float palette[boxmesh::kMaxColor + 1][4] = {};
    float linePalette[boxmesh::kMaxColor + 1][4] = {};
    double ribbonScale = 0.0;
    bool scaleKnown = false;
};

bool keptOrderStale(const KeptRegion& region, const KeptFrame& frame)
{
    const double* const cam = frame.cam;
    std::int8_t side[3] = {};
    regionSide(region.origin, cam, side);
    for (int k = 0; k < 3; ++k) {
        if (side[k] != region.refSide[k]) {
            return true;
        }
    }
    const double d = regionBoxDistance(region.origin, cam);
    for (int k = 0; k < 3; ++k) {
        const bool watch = d < kKeptNearOrderBlocks || (side[k] == 0 && d < kKeptSlabOrderBlocks);
        if (watch && static_cast<std::int32_t>(std::floor(cam[k])) != region.refBlock[k]) {
            return true;
        }
    }
    const double center[3] = {std::floor(cam[0]) + 0.5, std::floor(cam[1]) + 0.5, std::floor(cam[2]) + 0.5};
    double dir[3] = {};
    regionDirection(region.origin, center, dir);
    const double turn = dir[0] * region.refDir[0] + dir[1] * region.refDir[1] + dir[2] * region.refDir[2];
    const bool haveDirs = (dir[0] != 0.0 || dir[1] != 0.0 || dir[2] != 0.0)
                          && (region.refDir[0] != 0.0 || region.refDir[1] != 0.0 || region.refDir[2] != 0.0);
    if (haveDirs && turn < kKeptTurnCos) {
        return true;
    }
    if (region.xray && region.palette != g_keptPaletteVersion) {
        return true;
    }
    if (region.xray) {
        const double now = (std::max)(d, 4.0);
        const double then = (std::max)(region.refDistance, 4.0);
        if (now > then * kKeptWidthRatio || then > now * kKeptWidthRatio) {
            return true;
        }
        if (frame.scaleKnown && region.ribbonScale > 0.0) {
            const double ratio = frame.ribbonScale / region.ribbonScale;
            if (ratio > kKeptScaleRatio || ratio * kKeptScaleRatio < 1.0) {
                return true;
            }
        }
    }
    return false;
}

enum class KeptMade { Done, Busy, Failed };

KeptMade buildKeptRegionBody(const RegionOut& source, void* tess, const KeptFrame& frame, KeptRegion& fresh)
{
    fresh.generation = source.generation;
    fresh.geometry = source.geometry;
    fresh.xray = frame.xray;
    boxmesh::regionOrigin(source.key, fresh.origin);
    for (int k = 0; k < 3; ++k) {
        fresh.refBlock[k] = static_cast<std::int32_t>(std::floor(frame.cam[k]));
    }
    regionSide(fresh.origin, frame.cam, fresh.refSide);
    fresh.refDistance = regionBoxDistance(fresh.origin, frame.cam);
    fresh.palette = g_keptPaletteVersion;
    fresh.ribbonScale = frame.ribbonScale;
    const double eye[3] = {fresh.refBlock[0] + 0.5, fresh.refBlock[1] + 0.5, fresh.refBlock[2] + 0.5};
    regionDirection(fresh.origin, eye, fresh.refDir);
    const boxmesh::RegionGeometry& g = *source.geometry;
    const std::uint32_t quadsPerMesh = (std::max)(1U, frame.limit / 4U);
    const std::uint32_t linesPerMesh = (std::max)(1U, frame.limit / 2U);
    bool busy = false;
    bool ok = true;
    KeptBuild b{};
    b.tess = tess;
    b.origin[0] = fresh.origin[0];
    b.origin[1] = fresh.origin[1];
    b.origin[2] = fresh.origin[2];
    std::copy(eye, eye + 3, b.eye);
    b.inflate = frame.inflate;
    b.flip = frame.flip;
    const auto add = [&](int kind, std::uint8_t color, std::uint32_t total, std::uint32_t per) {
        for (std::uint32_t at = 0; at < total && ok; at += per) {
            KeptMesh mesh;
            mesh.storage = std::make_unique<MeshStorage>();
            mesh.color = color;
            mesh.line = kind == 1;
            b.kind = kind;
            b.first = at;
            b.count = (std::min)(total - at, per);
            if (!makeKeptMesh(b, mesh.storage->bytes, busy)) {
                ok = false;
                return;
            }
            mesh.count = b.made;
            try {
                fresh.meshes.push_back(std::move(mesh));
            } catch (...) {
                destroyKeptMesh(mesh.storage->bytes);
                throw;
            }
        }
    };
    std::vector<boxmesh::Quad> quads;
    std::vector<boxmesh::Edge> edges;
    std::vector<std::uint32_t> order;
    if (!frame.xray) {
        constexpr std::uint8_t kFaceOrder[] = {1, 2, 3, 4};
        for (const std::uint8_t color : kFaceOrder) {
            quads.clear();
            for (const boxmesh::Quad& q : g.quads) {
                if (q.color == color) {
                    quads.push_back(q);
                }
            }
            if (quads.empty()) {
                continue;
            }
            boxmesh::sortUnitFacesNearFirst(quads, eye);
            b.quads = quads.data();
            add(0, color, static_cast<std::uint32_t>(quads.size()), quadsPerMesh);
            if (!ok) {
                break;
            }
        }
        for (std::size_t i = 0; i < g.edges.size() && ok;) {
            std::size_t j = i;
            while (j < g.edges.size() && g.edges[j].color == g.edges[i].color) {
                ++j;
            }
            b.edges = g.edges.data() + i;
            add(1, g.edges[i].color, static_cast<std::uint32_t>(j - i), linesPerMesh);
            i = j;
        }
    } else {
        quads = g.quads;
        std::vector<std::int32_t> cuts;
        for (const boxmesh::Edge& edge : g.edges) {
            boxmesh::ribbonPieces(edge, eye, cuts);
            for (std::size_t i = 1; i < cuts.size(); ++i) {
                boxmesh::Edge piece = edge;
                piece.t0 = cuts[i - 1];
                piece.t1 = cuts[i];
                edges.push_back(piece);
            }
        }
        boxmesh::paintOrder(quads, edges, eye, quads.size() + edges.size() + 1, 1.0, order);
        std::memcpy(b.palette, frame.palette, sizeof(b.palette));
        std::memcpy(b.linePalette, frame.linePalette, sizeof(b.linePalette));
        b.ribbonScale = frame.ribbonScale;
        b.quads = quads.data();
        b.edges = edges.data();
        b.order = order.data();
        add(2, 0, static_cast<std::uint32_t>(order.size()), quadsPerMesh);
    }
    if (!ok) {
        for (KeptMesh& mesh : fresh.meshes) {
            destroyKeptMesh(mesh.storage->bytes);
        }
        fresh.meshes.clear();
        if (busy) {
            return KeptMade::Busy;
        }
        resetTessellator(tess);
        return KeptMade::Failed;
    }
    return KeptMade::Done;
}

KeptMade buildKeptRegion(const RegionOut& source, void* tess, const KeptFrame& frame, KeptRegion& fresh)
{
    try {
        return buildKeptRegionBody(source, tess, frame, fresh);
    } catch (...) {
        for (KeptMesh& mesh : fresh.meshes) {
            if (mesh.storage) {
                destroyKeptMesh(mesh.storage->bytes);
            }
        }
        fresh.meshes.clear();
        throw;
    }
}

bool rebuildKept(void* tess, const KeptFrame& frame, bool& waiting)
{
    struct Candidate {
        double distance;
        boxmesh::RegionKey key;
        bool content;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(g_keptPending.size() + 16);
    for (const auto& [key, out] : g_keptPending) {
        std::int32_t origin[3] = {};
        boxmesh::regionOrigin(key, origin);
        candidates.push_back(Candidate{regionBoxDistance(origin, frame.cam), key, true});
    }
    for (const auto& [key, region] : g_kept) {
        const bool orderMatters =
            region.geometry && (region.xray ? !(region.geometry->quads.empty() && region.geometry->edges.empty())
                                            : !region.geometry->quads.empty());
        if (orderMatters && g_keptPending.find(key) == g_keptPending.end()
            && keptOrderStale(region, frame)) {
            candidates.push_back(Candidate{regionBoxDistance(region.origin, frame.cam), key, false});
        }
    }
    waiting = !candidates.empty();
    if (candidates.empty()) {
        return true;
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
    LARGE_INTEGER t0{};
    LARGE_INTEGER now{};
    LARGE_INTEGER freq{};
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&freq);
    const auto elapsedUs = [&]() {
        QueryPerformanceCounter(&now);
        return static_cast<unsigned long long>((now.QuadPart - t0.QuadPart) * 1000000LL
                                               / (freq.QuadPart != 0 ? freq.QuadPart : 1));
    };
    std::size_t done = 0;
    std::size_t reordered = 0;
    for (const Candidate& candidate : candidates) {
        if (done != 0 && elapsedUs() >= kKeptRebuildBudgetUs) {
            break;
        }
        RegionOut source;
        if (candidate.content) {
            source = g_keptPending[candidate.key];
        } else {
            const KeptRegion& region = g_kept[candidate.key];
            source = RegionOut{candidate.key, region.generation, region.geometry};
        }
        KeptRegion fresh;
        const unsigned long long before = elapsedUs();
        const KeptMade made = buildKeptRegion(source, tess, frame, fresh);
        const unsigned long long one = elapsedUs() - before;
        unsigned long long longest = g_keptRegionUsMax.load(std::memory_order_relaxed);
        while (one > longest && !g_keptRegionUsMax.compare_exchange_weak(longest, one, std::memory_order_relaxed)) {
        }
        if (made == KeptMade::Busy) {
            break;
        }
        if (made == KeptMade::Failed) {
            keptBroke(L"make");
            return false;
        }
        auto it = g_kept.find(candidate.key);
        if (it != g_kept.end()) {
            destroyKeptRegion(it->second);
        } else {
            try {
                it = g_kept.try_emplace(candidate.key).first;
            } catch (...) {
                for (KeptMesh& mesh : fresh.meshes) {
                    if (mesh.storage) {
                        destroyKeptMesh(mesh.storage->bytes);
                    }
                }
                throw;
            }
        }
        it->second = std::move(fresh);
        for (const KeptMesh& mesh : it->second.meshes) {
            ++g_keptMeshes;
            if (mesh.line) {
                g_keptLines += mesh.count;
            } else {
                g_keptFaces += mesh.count;
            }
        }
        if (candidate.content) {
            g_keptPending.erase(candidate.key);
        } else {
            ++reordered;
        }
        ++done;
    }
    g_keptRebuilt.fetch_add(done - reordered, std::memory_order_relaxed);
    g_keptReordered.fetch_add(reordered, std::memory_order_relaxed);
    const unsigned long long took = elapsedUs();
    unsigned long long was = g_keptRebuildUsMax.load(std::memory_order_relaxed);
    while (took > was && !g_keptRebuildUsMax.compare_exchange_weak(was, took, std::memory_order_relaxed)) {
    }
    return done != 0;
}

bool regionVisible(const std::int32_t origin[3], const double cam[3], const float vp[16], bool haveVp)
{
    if (!haveVp) {
        return true;
    }
    if (regionBoxDistance(origin, cam) <= static_cast<double>(boxmesh::kRegionSize)) {
        return true;
    }
    int outside[5] = {};
    for (int i = 0; i < 8; ++i) {
        const double x = origin[0] - 1.0 + ((i & 1) != 0 ? boxmesh::kRegionSize + 2.0 : 0.0) - cam[0];
        const double y = origin[1] - 1.0 + ((i & 2) != 0 ? boxmesh::kRegionSize + 2.0 : 0.0) - cam[1];
        const double z = origin[2] - 1.0 + ((i & 4) != 0 ? boxmesh::kRegionSize + 2.0 : 0.0) - cam[2];
        const double cx = x * vp[0] + y * vp[4] + z * vp[8] + vp[12];
        const double cy = x * vp[1] + y * vp[5] + z * vp[9] + vp[13];
        const double cw = x * vp[3] + y * vp[7] + z * vp[11] + vp[15];
        outside[0] += cx < -cw ? 1 : 0;
        outside[1] += cx > cw ? 1 : 0;
        outside[2] += cy < -cw ? 1 : 0;
        outside[3] += cy > cw ? 1 : 0;
        outside[4] += cw < 0.05 ? 1 : 0;
    }
    for (const int n : outside) {
        if (n == 8) {
            return false;
        }
    }
    return true;
}

void drawKeptFrameBody(void* lrp, void* ctx, void* tess, const float camF[3], std::uint32_t limit, bool xray,
                       const void* flatMaterial, const void* xrayMaterial, bool borrowed)
{
    KeptFrame frame;
    frame.xray = xray;
    frame.cam[0] = camF[0];
    frame.cam[1] = camF[1];
    frame.cam[2] = camF[2];
    frame.inflate = static_cast<float>(0) / 1024.0F;
    frame.flip = false;
    frame.limit = limit;
    const float faceAlpha = boxes::boxFaceAlpha();
    float faceRgba[boxmesh::kMaxColor + 1][4] = {};
    float lineRgba[boxmesh::kMaxColor + 1][4] = {};
    bool shown[boxmesh::kMaxColor + 1] = {};
    for (std::size_t c = 1; c <= boxmesh::kMaxColor; ++c) {
        float rgb[3] = {};
        float alpha = faceAlpha;
        if (!boxes::boxStyle(static_cast<blocks::DiffColor>(c), rgb, &alpha)) {
            frame.palette[c][3] = 0.0F;
            frame.linePalette[c][3] = 0.0F;
            continue;
        }
        shown[c] = true;
        lineRgba[c][0] = rgb[0];
        lineRgba[c][1] = rgb[1];
        lineRgba[c][2] = rgb[2];
        lineRgba[c][3] = kLineAlpha;
        std::memcpy(frame.linePalette[c], lineRgba[c], sizeof(lineRgba[c]));
        frame.linePalette[c][3] = alpha > 0.0F ? kLineAlpha : 0.0F;
        frame.palette[c][0] = rgb[0];
        frame.palette[c][1] = rgb[1];
        frame.palette[c][2] = rgb[2];
        frame.palette[c][3] = alpha;
        const float k = std::clamp(alpha, 0.0F, 1.0F);
        for (int i = 0; i < 3; ++i) {
            faceRgba[c][i] = 0.5F + (rgb[i] - 0.5F) * k;
        }
        faceRgba[c][3] = 1.0F;
    }
    if (std::memcmp(g_keptXrayPalette[0], frame.palette, sizeof(frame.palette)) != 0
        || std::memcmp(g_keptXrayPalette[1], frame.linePalette, sizeof(frame.linePalette)) != 0) {
        ++g_keptPaletteVersion;
    }
    if (!g_kept.empty() && (frame.inflate != g_keptInflate || frame.flip != g_keptFlip)) {
        destroyKept(KeptWhy::Rebake);
    }
    g_keptInflate = frame.inflate;
    g_keptFlip = frame.flip;
    std::memcpy(g_keptXrayPalette[0], frame.palette, sizeof(frame.palette));
    std::memcpy(g_keptXrayPalette[1], frame.linePalette, sizeof(frame.linePalette));
    float eye[3] = {};
    float vp[16] = {};
    const bool snapshot = boxes::cameraSnapshot(eye, vp);
    bool haveVp = snapshot;
    bool haveVpDir = snapshot;
    if (snapshot) {
        for (int k = 0; k < 3; ++k) {
            const double gap = std::fabs(static_cast<double>(eye[k]) - frame.cam[k]);
            if (gap > 0.5) {
                haveVp = false;
            }
            if (!(gap <= kVpDirectionBlocks)) {
                haveVpDir = false;
            }
        }
    }
    {
        double yScale = 1.0 / std::tan(35.0 * 3.14159265358979 / 180.0);
        if (haveVpDir) {
            const double col1 = std::sqrt(static_cast<double>(vp[1]) * vp[1] + static_cast<double>(vp[5]) * vp[5]
                                          + static_cast<double>(vp[9]) * vp[9]);
            if (std::isfinite(col1) && col1 > 0.05 && col1 < 100.0) {
                yScale = col1;
                frame.scaleKnown = true;
            }
        }
        const render::Viewport view = render::overlayViewport();
        const double height = view.valid && view.height >= 64.0F ? view.height : 1080.0;
        frame.ribbonScale = kRibbonPixels * 2.0 / (yScale * height);
    }
    double forward[3] = {0.0, 0.0, 0.0};
    bool haveForward = false;
    if (haveVpDir) {
        const double f[3] = {vp[3], vp[7], vp[11]};
        const double n = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (std::isfinite(n) && n > 1e-6) {
            for (int k = 0; k < 3; ++k) {
                forward[k] = f[k] / n;
            }
            haveForward = true;
        }
    }
    TessSaved saved{};
    if (!saveState(ctx, tess, saved)) {
        return;
    }
    syncKept();
    bool waiting = false;
    const bool progressed = borrowed ? true : rebuildKept(tess, frame, waiting);
    if (g_keptBroken.load(std::memory_order_relaxed)) {
        restoreState(ctx, tess, saved);
        return;
    }
    const void* const lineMaterial = materialFor(lrp, kLineMode);
    if (lineMaterial == nullptr) {
        g_noLineMaterial.fetch_add(1, std::memory_order_relaxed);
    }
    std::vector<std::pair<double, const KeptRegion*>> visible;
    visible.reserve(g_kept.size());
    std::uint32_t culled = 0;
    for (const auto& [key, region] : g_kept) {
        if (!regionVisible(region.origin, frame.cam, vp, haveVp)) {
            ++culled;
            continue;
        }
        visible.emplace_back(boxmesh::manhattanKey(region.origin, boxmesh::kRegionSize, frame.cam), &region);
    }
    if (xray) {
        std::sort(visible.begin(), visible.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    } else {
        std::sort(visible.begin(), visible.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    std::size_t total = 0;
    for (const auto& [key, region] : visible) {
        total += region->meshes.size();
    }
    TopMatrix m;
    std::uint32_t drawnMeshes = 0;
    bool failed = false;
    bool matrixOk = false;
    bool ordered = false;
    LARGE_INTEGER t0{};
    LARGE_INTEGER t1{};
    LARGE_INTEGER freq{};
    QueryPerformanceCounter(&t0);
    if (findTopMatrix(ctx, m)) {
        matrixOk = true;
        {
            static std::atomic<bool> told{false};
            if (!told.exchange(true)) {
                log().info(L"WorldMesh: drawing order {} (matrix stack size {}, map {})",
                           m.sortControl ? (m.mapEntry != nullptr ? L"is set through a spare slot of the matrix stack"
                                                                  : L"is set through the matrix below the top")
                                         : L"cannot be set",
                           m.stackSize, m.mapSize);
            }
        }
        if (!haveForward) {
            m.sortControl = false;
            static std::atomic<bool> toldForward{false};
            if (!toldForward.exchange(true)) {
                log().info(L"WorldMesh: the camera direction was not available for a frame (camera snapshot {}); "
                           L"that frame is drawn without setting the order",
                           !snapshot ? L"missing" : (haveVpDir ? L"odd" : L"away from the camera"));
            }
        }
        ordered = m.sortControl;
        const float white[4] = {1.0F, 1.0F, 1.0F, 1.0F};
        std::size_t index = 0;
        for (const auto& [key, region] : visible) {
            const float position[3] = {static_cast<float>(region->origin[0] - frame.cam[0]),
                                       static_cast<float>(region->origin[1] - frame.cam[1]),
                                       static_cast<float>(region->origin[2] - frame.cam[2])};
            for (const KeptMesh& mesh : region->meshes) {
                const double ahead =
                    kSortBase + kSortSpan * static_cast<double>(total - index) / static_cast<double>(total);
                ++index;
                const float sortPos[3] = {static_cast<float>(forward[0] * ahead),
                                          static_cast<float>(forward[1] * ahead),
                                          static_cast<float>(forward[2] * ahead)};
                const void* use = nullptr;
                const float* rgba = white;
                if (region->xray) {
                    use = xrayMaterial;
                } else {
                    if (mesh.color > boxmesh::kMaxColor || !shown[mesh.color]) {
                        continue;
                    }
                    use = mesh.line ? lineMaterial : flatMaterial;
                    rgba = mesh.line ? lineRgba[mesh.color] : faceRgba[mesh.color];
                }
                if (use == nullptr) {
                    continue;
                }
                if (!renderKeptMesh(ctx, use, m, mesh.storage->bytes, rgba, position, sortPos)) {
                    failed = true;
                    break;
                }
                ++drawnMeshes;
            }
            if (failed) {
                break;
            }
        }
        restoreTopMatrix(m);
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    restoreState(ctx, tess, saved);
    if (failed) {
        keptBroke(L"draw");
        return;
    }
    if (matrixOk && !ordered && drawnMeshes != 0) {
        g_keptUnordered.fetch_add(1, std::memory_order_relaxed);
    }
    const bool stalled = !matrixOk || (waiting && !progressed);
    g_keptStall = stalled ? g_keptStall + 1 : 0;
    if (g_keptStall >= kKeptStallFrames) {
        keptBroke(matrixOk ? L"the tessellator stayed busy" : L"the matrix stack was unreadable");
        return;
    }
    const auto micros = static_cast<unsigned long long>(
        (t1.QuadPart - t0.QuadPart) * 1000000LL / (freq.QuadPart != 0 ? freq.QuadPart : 1));
    g_drawn.fetch_add(1, std::memory_order_relaxed);
    g_keptFrames.fetch_add(1, std::memory_order_relaxed);
    g_keptLastXray.store(xray, std::memory_order_relaxed);
    g_keptLastDrawn.store(drawnMeshes, std::memory_order_relaxed);
    g_keptLastCulled.store(culled, std::memory_order_relaxed);
    g_keptDrawUsLast.store(micros, std::memory_order_relaxed);
    unsigned long long was = g_keptDrawUsMax.load(std::memory_order_relaxed);
    while (micros > was && !g_keptDrawUsMax.compare_exchange_weak(was, micros, std::memory_order_relaxed)) {
    }
    g_keptRegionCount.store(g_kept.size(), std::memory_order_relaxed);
    g_keptMeshCount.store(g_keptMeshes, std::memory_order_relaxed);
    g_keptFaceCount.store(g_keptFaces, std::memory_order_relaxed);
    g_keptLineCount.store(g_keptLines, std::memory_order_relaxed);
    g_keptWaiting.store(g_keptPending.size(), std::memory_order_relaxed);
}

void drawKeptFrame(void* lrp, void* ctx, void* tess, const float camF[3], std::uint32_t limit, bool xray,
                   const void* flatMaterial, const void* xrayMaterial, bool borrowed)
{
    TessSaved outer{};
    const bool saved = saveState(ctx, tess, outer);
    try {
        drawKeptFrameBody(lrp, ctx, tess, camF, limit, xray, flatMaterial, xrayMaterial, borrowed);
    } catch (...) {
        if (saved) {
            restoreState(ctx, tess, outer);
        }
        keptBroke(L"an exception while keeping the meshes");
    }
}

void drawBoxes(void* lrp, void* ctx)
{
    if (g_keptReleaseAsked.load(std::memory_order_seq_cst)) {
        if (!g_keptReleased.load(std::memory_order_relaxed)) {
            destroyKept(KeptWhy::Released);
            g_keptReleased.store(true, std::memory_order_release);
        }
        return;
    }
    if (!boxes::boxesOn()) {
        if (!g_kept.empty()) {
            destroyKept(KeptWhy::BoxesOff);
        }
        return;
    }
    const bool xray = boxes::boxXray();
    const int mode = xray ? kDefaultXray
                          : kDefaultFlat;
    if (mode == 0) {
        if (!g_kept.empty()) {
            destroyKept(KeptWhy::ModeOff);
        }
        return;
    }
    if (ctx == nullptr || lrp == nullptr || !memory::isReadable(ctx, kCtxTess + 8)) {
        g_badCtx.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    void* tess = nullptr;
    std::memcpy(&tess, static_cast<unsigned char*>(ctx) + kCtxTess, sizeof(tess));
    if (tess == nullptr || !memory::isReadable(tess, kTessReadable)) {
        g_badCtx.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const void* const material = materialFor(lrp, mode);
    if (material == nullptr) {
        g_noMaterial.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const Choice* const choice = choiceOf(mode);
    const bool multiply = choice != nullptr && choice->multiply;

    LARGE_INTEGER t0{};
    LARGE_INTEGER t1{};
    LARGE_INTEGER freq{};
    QueryPerformanceCounter(&t0);

    float cam[3] = {};
    if (!memory::isReadable(static_cast<unsigned char*>(lrp) + kLrpCamera, sizeof(cam))) {
        return;
    }
    std::memcpy(cam, static_cast<unsigned char*>(lrp) + kLrpCamera, sizeof(cam));
    if (!std::isfinite(cam[0]) || !std::isfinite(cam[1]) || !std::isfinite(cam[2])) {
        return;
    }
    if (float eye[3] = {}, vp[16] = {}; boxes::cameraSnapshot(eye, vp)) {
        const float dx = cam[0] - eye[0];
        const float dy = cam[1] - eye[1];
        const float dz = cam[2] - eye[2];
        if (std::fabs(dx) > 64.0F || std::fabs(dy) > 64.0F || std::fabs(dz) > 64.0F) {
            g_camShifted.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    std::uint32_t limit = 0;
    std::memcpy(&limit, static_cast<unsigned char*>(tess) + kTessLimit, sizeof(limit));
    if (limit < 64 || limit > (1U << 24)) {
        limit = 65532;
    }
    limit &= ~3U;
    {
        const double eye[3] = {cam[0], cam[1], cam[2]};
        prepare(xray, eye, FreeCamera::instance().borrowing(), limit / 4U);
    }
    if (g_prepared.retained) {
        const bool keptXray = g_prepared.xray;
        const int flatMode = kDefaultFlat;
        const int xrayMode = kDefaultXray;
        const Choice* const flatChoice = choiceOf(flatMode);
        const Choice* const xrayChoice = choiceOf(xrayMode);
        const bool flatOk = flatChoice != nullptr && flatChoice->multiply;
        const bool xrayOk = xrayChoice != nullptr && !xrayChoice->multiply && xrayChoice->mode != kLineMode;
        if (keptAvailable() && (keptXray ? xrayOk : flatOk)) {
            const void* const flatMaterial = flatOk ? materialFor(lrp, flatMode) : nullptr;
            const void* const xrayMaterial = xrayOk ? materialFor(lrp, xrayMode) : nullptr;
            if ((keptXray ? xrayMaterial : flatMaterial) != nullptr) {
                drawKeptFrame(lrp, ctx, tess, cam, limit, keptXray, flatMaterial, xrayMaterial,
                              FreeCamera::instance().borrowing());
            } else {
                g_noMaterial.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return;
    }
    if (!g_kept.empty()) {
        destroyKept(KeptWhy::NotKept);
    }
    g_quadCount.store(g_prepared.allQuads, std::memory_order_relaxed);
    g_edgeCount.store(g_prepared.allEdges, std::memory_order_relaxed);
    g_cellCount.store(g_prepared.cells, std::memory_order_relaxed);
    g_quadRadius.store(static_cast<int>(std::ceil(g_prepared.quadRadius)),
                       std::memory_order_relaxed);
    if (g_prepared.quads.empty()) {
        g_noQuads.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    TessSaved saved{};
    if (!saveState(ctx, tess, saved)) {
        return;
    }

    const float faceAlpha = boxes::boxFaceAlpha();
    const float inflate = static_cast<float>(0) / 1024.0F;
    const bool vertexColor = false;
    const bool flip = false;
    std::size_t budget = static_cast<std::size_t>(
        (std::max)(1, kDefaultMaxBoxes));
    std::uint32_t batches = 0;
    std::uint32_t faces = 0;
    bool busy = false;
    bool failed = false;
    DWORD code = 0;
    std::size_t capped = 0;
    const void* const lineMaterial = xray ? material : materialFor(lrp, kLineMode);
    if (lineMaterial == nullptr) {
        g_noLineMaterial.fetch_add(1, std::memory_order_relaxed);
    }
    std::size_t lineBudget = static_cast<std::size_t>(
        (std::max)(1, kDefaultMaxBoxes));
    std::uint32_t lines = 0;
    std::uint32_t facesOf[boxmesh::kMaxColor + 1] = {};
    std::uint32_t calls = 0;
    for (const Segment& seg : g_prepared.segments) {
        if (failed || busy) {
            break;
        }
        const std::size_t color = seg.color;
        if (color > boxmesh::kMaxColor) {
            continue;
        }
        if (color == 0 && !seg.line) {
            Job job{};
            job.ctx = ctx;
            job.tess = tess;
            job.material = material;
            job.cam[0] = cam[0];
            job.cam[1] = cam[1];
            job.cam[2] = cam[2];
            job.perQuadColor = true;
            for (std::size_t c = 1; c <= boxmesh::kMaxColor; ++c) {
                float rgb[3] = {};
                float alpha = faceAlpha;
                if (!boxes::boxStyle(static_cast<blocks::DiffColor>(c), rgb, &alpha)) {
                    alpha = 0.0F;
                }
                job.linePalette[c][0] = rgb[0];
                job.linePalette[c][1] = rgb[1];
                job.linePalette[c][2] = rgb[2];
                job.linePalette[c][3] = alpha > 0.0F ? kLineAlpha : 0.0F;
                if (multiply) {
                    const float k = std::clamp(alpha, 0.0F, 1.0F);
                    for (float& one : rgb) {
                        one = 0.5F + (one - 0.5F) * k;
                    }
                    alpha = 1.0F;
                }
                job.palette[c][0] = rgb[0];
                job.palette[c][1] = rgb[1];
                job.palette[c][2] = rgb[2];
                job.palette[c][3] = alpha;
            }
            {
                double yScale = 1.0 / std::tan(35.0 * 3.14159265358979 / 180.0);
                float camEye[3] = {};
                float vp[16] = {};
                if (boxes::cameraSnapshot(camEye, vp)) {
                    const double col1 = std::sqrt(static_cast<double>(vp[1]) * vp[1]
                                                  + static_cast<double>(vp[5]) * vp[5]
                                                  + static_cast<double>(vp[9]) * vp[9]);
                    const double col3 = std::sqrt(static_cast<double>(vp[3]) * vp[3]
                                                  + static_cast<double>(vp[7]) * vp[7]
                                                  + static_cast<double>(vp[11]) * vp[11]);
                    if (std::isfinite(col1) && col1 > 0.05 && col1 < 100.0) {
                        yScale = col1;
                    }
                    if (std::isfinite(col3) && col3 > 1e-3 && std::isfinite(vp[15])) {
                        job.forward[0] = vp[3];
                        job.forward[1] = vp[7];
                        job.forward[2] = vp[11];
                        job.forwardW = vp[15];
                        job.haveForward = true;
                    }
                }
                const render::Viewport view = render::overlayViewport();
                const double height = view.valid && view.height >= 64.0F ? view.height : 1080.0;
                job.ribbonScale = kRibbonPixels * 2.0 / (yScale * height);
            }
            const std::size_t total = seg.count;
            const std::size_t count = (std::min)(total, budget);
            capped += total - count;
            budget -= count;
            if (count == 0) {
                continue;
            }
            job.order = g_prepared.mixedOrder.data() + seg.first + (total - count);
            job.quads = g_prepared.quads.data();
            job.edges = g_prepared.edges.data();
            job.count = count;
            job.rgba[0] = 1.0F;
            job.rgba[1] = 1.0F;
            job.rgba[2] = 1.0F;
            job.rgba[3] = 1.0F;
            job.inflate = inflate;
            job.vertexColor = false;
            job.flip = flip;
            job.limit = limit;
            runJob(job);
            batches += job.batches;
            calls += job.batches;
            faces += job.faces;
            lines += job.ribbons;
            for (std::size_t c = 0; c <= boxmesh::kMaxColor; ++c) {
                facesOf[c] += job.facesOf[c];
            }
            busy = job.busy;
            failed = job.failed;
            code = job.code;
            continue;
        }
        float baseRgb[3] = {};
        float baseAlpha = faceAlpha;
        if (!boxes::boxStyle(static_cast<blocks::DiffColor>(color), baseRgb, &baseAlpha)) {
            continue;
        }
        if (!seg.line) {
            float rgb[3] = {baseRgb[0], baseRgb[1], baseRgb[2]};
            float alpha = baseAlpha;
            if (multiply) {
                const float k = std::clamp(alpha, 0.0F, 1.0F);
                for (float& one : rgb) {
                    one = 0.5F + (one - 0.5F) * k;
                }
                alpha = 1.0F;
            }
            const std::size_t total = seg.count;
            const std::size_t count = (std::min)(total, budget);
            capped += total - count;
            budget -= count;
            if (count == 0) {
                continue;
            }
            Job job{};
            job.ctx = ctx;
            job.tess = tess;
            job.material = material;
            job.cam[0] = cam[0];
            job.cam[1] = cam[1];
            job.cam[2] = cam[2];
            job.quads = g_prepared.quads.data() + seg.first;
            job.count = count;
            job.rgba[0] = rgb[0];
            job.rgba[1] = rgb[1];
            job.rgba[2] = rgb[2];
            job.rgba[3] = alpha;
            job.inflate = inflate;
            job.vertexColor = vertexColor;
            job.flip = flip;
            job.limit = limit;
            runJob(job);
            batches += job.batches;
            calls += job.batches;
            faces += job.faces;
            facesOf[color] += job.faces;
            busy = job.busy;
            failed = job.failed;
            code = job.code;
            continue;
        }
        if (lineMaterial == nullptr) {
            continue;
        }
        const std::size_t edgeTotal = seg.count;
        const std::size_t edgeCount = (std::min)(edgeTotal, lineBudget);
        capped += edgeTotal - edgeCount;
        lineBudget -= edgeCount;
        if (edgeCount == 0) {
            continue;
        }
        Job job{};
        job.ctx = ctx;
        job.tess = tess;
        job.material = lineMaterial;
        job.cam[0] = cam[0];
        job.cam[1] = cam[1];
        job.cam[2] = cam[2];
        job.edges = g_prepared.edges.data() + seg.first;
        job.edgeCount = edgeCount;
        job.rgba[0] = baseRgb[0];
        job.rgba[1] = baseRgb[1];
        job.rgba[2] = baseRgb[2];
        job.rgba[3] = kLineAlpha;
        job.vertexColor = vertexColor;
        job.limit = limit;
        runLineJob(job);
        batches += job.batches;
        calls += job.batches;
        lines += job.faces;
        busy = job.busy;
        failed = job.failed;
        code = job.code;
    }
    g_lastCalls.store(calls, std::memory_order_relaxed);
    g_lastLines.store(lines, std::memory_order_relaxed);
    g_lines.fetch_add(lines, std::memory_order_relaxed);
    restoreState(ctx, tess, saved);
    capped += g_prepared.allQuads - g_prepared.quads.size();
    if (lineMaterial != nullptr) {
        capped += g_prepared.allEdges - g_prepared.edges.size();
    }
    for (std::size_t c = 0; c <= boxmesh::kMaxColor; ++c) {
        g_lastFaces[c].store(facesOf[c], std::memory_order_relaxed);
    }
    g_lastCap.store(g_prepared.mixed ? g_prepared.faceBudget : frameBudget(),
                    std::memory_order_relaxed);

    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    const auto micros = static_cast<unsigned long long>(
        (t1.QuadPart - t0.QuadPart) * 1000000LL / (freq.QuadPart != 0 ? freq.QuadPart : 1));
    unsigned long long was = g_microsMax.load(std::memory_order_relaxed);
    while (micros > was
           && !g_microsMax.compare_exchange_weak(was, micros, std::memory_order_relaxed)) {
    }
    if (capped != 0) {
        g_capped.fetch_add(capped, std::memory_order_relaxed);
    }
    if (failed) {
        g_failed.fetch_add(1, std::memory_order_relaxed);
        if (g_failLogs.fetch_add(1, std::memory_order_relaxed) < 5) {
            log().error(L"WorldMesh: faulted while stacking boxes {:#x} (material {} / faces "
                        L"{})",
                        code,
                        mode,
                        faces);
        }
    }
    if (batches != 0) {
        g_drawn.fetch_add(1, std::memory_order_relaxed);
    }
}

void* __fastcall detourStageHost(void* lrp, void* arg2, void* view, void* arg4)
{
    void* const result =
        (g_hostOriginal != nullptr) ? g_hostOriginal(lrp, arg2, view, arg4) : nullptr;
    g_hostCalls.fetch_add(1, std::memory_order_relaxed);
    {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) {
            void* ctx = nullptr;
            if (arg2 != nullptr && memory::isReadable(arg2, kHostCtxAt + sizeof(ctx))) {
                std::memcpy(&ctx, static_cast<unsigned char*>(arg2) + kHostCtxAt, sizeof(ctx));
            }
            int flag = -1;
            if (view != nullptr && memory::isReadable(view, kViewMainFlag + 1)) {
                flag = *(static_cast<const unsigned char*>(view) + kViewMainFlag);
            }
        }
    }
    if (g_drawAt.load(std::memory_order_relaxed) != 1
        || g_teardown.load(std::memory_order_acquire)) {
        return result;
    }
    void* ctx = nullptr;
    if (arg2 == nullptr || !memory::isReadable(arg2, kHostCtxAt + sizeof(ctx))) {
        return result;
    }
    std::memcpy(&ctx, static_cast<unsigned char*>(arg2) + kHostCtxAt, sizeof(ctx));
    if (ctx == nullptr) {
        return result;
    }
    if (g_drewThisFrame.exchange(true, std::memory_order_acq_rel)) {
        return result;
    }
    if (g_inside.exchange(true, std::memory_order_seq_cst)) {
        return result;
    }
    drawBoxes(lrp, ctx);
    g_inside.store(false, std::memory_order_release);
    return result;
}

void __fastcall detourNameTagStage(void* lrp, void* ctx, void* view, void* extra)
{
    TSUKUYOMI_HOOK_COUNT("NameTagStage");
    callOriginal(lrp, ctx, view, extra);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_teardown.load(std::memory_order_acquire)) {
        return;
    }
    if (g_inside.exchange(true, std::memory_order_seq_cst)) {
        return;
    }
    drawBoxes(lrp, ctx);
    g_inside.store(false, std::memory_order_release);
}

}

bool installHooks()
{
    const Scanner& scanner = Scanner::instance();
    void* const stage = scanner.address(Target::NameTagStage);
    g_begin = scanner.addressAs<TessBeginFn>(Target::TessellatorBegin);
    g_vertex = scanner.addressAs<TessVertexFn>(Target::TessellatorVertex);
    g_color = scanner.addressAs<TessColorFn>(Target::TessellatorColor);
    g_render = scanner.addressAs<RenderMeshFn>(Target::RenderMeshImmediately);
    {
        auto* const end = scanner.address(Target::TessellatorEnd);
        auto* const clear = scanner.address(Target::TessellatorClear);
        auto* const meshRender = scanner.address(Target::MeshRender);
        auto* const meshDestroy = scanner.address(Target::MeshDestroy);
        auto* const immediate = reinterpret_cast<const std::byte*>(g_render);
        const auto callAt = [immediate](std::size_t at) -> void* {
            if (std::to_integer<unsigned>(immediate[at]) != 0xE8U) {
                return nullptr;
            }
            return memory::ripTarget(immediate, at + 1);
        };
        void* flag = nullptr;
        bool callsMatch = false;
        const char* nameText = nullptr;
        std::size_t nameLength = 0;
        if (immediate != nullptr && memory::isReadable(immediate, 0x100)) {
            flag = callAt(0x3d);
            callsMatch = end != nullptr && callAt(0x7a) == static_cast<void*>(end)
                         && meshRender != nullptr && callAt(0xbb) == static_cast<void*>(meshRender)
                         && meshDestroy != nullptr && callAt(0xc7) == static_cast<void*>(meshDestroy)
                         && clear != nullptr && callAt(0xef) == static_cast<void*>(clear);
            static constexpr unsigned char kLea[] = {0x48, 0x8D, 0x0D};
            static constexpr unsigned char kMov[] = {0x48, 0xC7, 0x85, 0xE0, 0x03, 0x00, 0x00};
            if (std::memcmp(immediate + 0x42, kLea, sizeof(kLea)) == 0
                && std::memcmp(immediate + 0x50, kMov, sizeof(kMov)) == 0) {
                nameText = static_cast<const char*>(memory::ripTarget(immediate, 0x45));
                std::uint32_t length = 0;
                std::memcpy(&length, immediate + 0x57, sizeof(length));
                nameLength = length;
            }
        }
        const bool flagOk = flag != nullptr && memory::inGameModule(flag);
        const bool nameOk = nameText != nullptr && memory::inGameModule(nameText) && nameLength > 0
                            && nameLength < 256 && memory::isReadable(nameText, nameLength);
        if (flagOk && callsMatch && nameOk) {
            g_meshEnd = reinterpret_cast<TessEndFn>(end);
            g_tessClear = reinterpret_cast<TessClearFn>(clear);
            g_rendererFlag = reinterpret_cast<RendererFlagFn>(flag);
            g_meshRender = reinterpret_cast<MeshRenderFn>(meshRender);
            g_meshDestroy = reinterpret_cast<MeshDestroyFn>(meshDestroy);
            g_meshName = MeshName{nameText, nameLength};
            log().info(L"WorldMesh: kept meshes are ready (boxes are kept per {}-block region in both modes "
                       L"and drawn without re-sending the vertices)",
                       boxmesh::kRegionSize);
        } else {
            log().warn(L"WorldMesh: kept meshes are not available (signatures {}/{}/{}/{} / match the "
                       L"immediate path {} / renderer flag {} / mesh name {}). Boxes use the immediate "
                       L"drawing",
                       end != nullptr, clear != nullptr, meshRender != nullptr, meshDestroy != nullptr,
                       callsMatch, flagOk, nameOk);
        }
    }
    if (std::byte* const site = scanner.address(Target::MaterialPtrCtorSite)) {
        void* const group = memory::ripTarget(site, 0x11);
        void* const ctor = memory::ripTarget(site, 0x1D);
        if (group != nullptr && memory::isReadable(group, 8) && ctor != nullptr
            && memory::inGameModule(ctor)) {
            g_matGroup = group;
            g_matCtor = reinterpret_cast<MaterialCtorFn>(ctor);
        }
    }
    if (stage == nullptr || g_begin == nullptr || g_vertex == nullptr || g_color == nullptr
        || g_render == nullptr) {
        log().warn(L"WorldMesh: the immediate-mesh path is incomplete (stage {} / begin {} / "
                   L"vertex {} / color {} / draw {})",
                   stage != nullptr,
                   g_begin != nullptr,
                   g_vertex != nullptr,
                   g_color != nullptr,
                   g_render != nullptr);
        return false;
    }
    if (!HookManager::instance().create(stage, reinterpret_cast<void*>(&detourNameTagStage),
                                        reinterpret_cast<void**>(&g_original),
                                        L"NameTagStage")) {
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    {
        void* const host = scanner.address(Target::NameTagStageCaller);
        if (host != nullptr) {
            HookManager::instance().create(host, reinterpret_cast<void*>(&detourStageHost),
                                           reinterpret_cast<void**>(&g_hostOriginal),
                                           L"NameTagStageCaller");
        }
    }
    startWorker();
    return true;
}

int mode()
{
    return boxes::boxXray() ? kDefaultXray
                            : kDefaultFlat;
}

bool active(bool xray)
{
    if (!g_installed.load(std::memory_order_acquire) || g_teardown.load(std::memory_order_acquire)) {
        return false;
    }
    const int want = xray ? kDefaultXray
                          : kDefaultFlat;
    return want != 0 && !g_modeBroken[want & 7].load(std::memory_order_relaxed);
}

void onPresent()
{
    g_drewThisFrame.store(false, std::memory_order_relaxed);
}

void report()
{
    if (!g_installed.load(std::memory_order_acquire)) {
        return;
    }
    static unsigned long long last = 0;
    static unsigned reports = 0;
    const unsigned long long now = GetTickCount64();
    if (last == 0) {
        last = now;
        return;
    }
    const unsigned long long interval = reports < 3 ? 10000ULL : 300000ULL;
    if (now - last < interval) {
        return;
    }
    const unsigned long long seconds = (now - last) / 1000ULL;
    last = now;
    const unsigned long long calls = g_calls.exchange(0, std::memory_order_relaxed);
    const unsigned long long drawn = g_drawn.exchange(0, std::memory_order_relaxed);
    const unsigned long long failed = g_failed.exchange(0, std::memory_order_relaxed);
    const unsigned long long noMaterial = g_noMaterial.exchange(0, std::memory_order_relaxed);
    const unsigned long long camShifted = g_camShifted.exchange(0, std::memory_order_relaxed);
    const unsigned long long noQuads = g_noQuads.exchange(0, std::memory_order_relaxed);
    const unsigned long long badCtx = g_badCtx.exchange(0, std::memory_order_relaxed);
    const unsigned long long capped = g_capped.exchange(0, std::memory_order_relaxed);
    const unsigned long long hostCalls = g_hostCalls.exchange(0, std::memory_order_relaxed);
    const unsigned long long keptFrames = g_keptFrames.exchange(0, std::memory_order_relaxed);
    if (boxes::boxesOn() && keptFrames != 0) {
        log().info(L"WorldMesh: {} frame(s) drew kept meshes ({}; {} region(s) / {} mesh(es) / {} "
                   L"face(s) / {} line(s); last frame drew {} mesh(es) and skipped {} region(s) off "
                   L"screen in {:.2f} ms, longest {:.2f} ms; {} frame(s) could not set the drawing order; "
                   L"rebuilt {} region(s) and re-sorted {} for the camera, longest {:.2f} ms in a frame (one region up to "
                   L"{:.2f} ms), "
                   L"{} waiting; grouped {} time(s), last {} ms, longest {} ms)",
                   keptFrames,
                   g_keptLastXray.load(std::memory_order_relaxed) ? L"see-through" : L"not see-through",
                   g_keptRegionCount.load(std::memory_order_relaxed),
                   g_keptMeshCount.load(std::memory_order_relaxed),
                   g_keptFaceCount.load(std::memory_order_relaxed),
                   g_keptLineCount.load(std::memory_order_relaxed),
                   g_keptLastDrawn.load(std::memory_order_relaxed),
                   g_keptLastCulled.load(std::memory_order_relaxed),
                   static_cast<double>(g_keptDrawUsLast.load(std::memory_order_relaxed)) / 1000.0,
                   static_cast<double>(g_keptDrawUsMax.exchange(0, std::memory_order_relaxed))
                       / 1000.0,
                   g_keptUnordered.exchange(0, std::memory_order_relaxed),
                   g_keptRebuilt.exchange(0, std::memory_order_relaxed),
                   g_keptReordered.exchange(0, std::memory_order_relaxed),
                   static_cast<double>(g_keptRebuildUsMax.exchange(0, std::memory_order_relaxed))
                       / 1000.0,
                   static_cast<double>(g_keptRegionUsMax.exchange(0, std::memory_order_relaxed)) / 1000.0,
                   g_keptWaiting.load(std::memory_order_relaxed),
                   g_groupCount.exchange(0, std::memory_order_relaxed),
                   g_groupMsLast.load(std::memory_order_relaxed),
                   g_groupMsMax.exchange(0, std::memory_order_relaxed));
    }
    const unsigned long long immediateDrawn = drawn > keptFrames ? drawn - keptFrames : 0;
    if (boxes::boxesOn() && (immediateDrawn != 0 || capped != 0)) {
        const int radius = g_quadRadius.load(std::memory_order_relaxed);
        log().info(L"WorldMesh: {} frame(s) drew boxes ({} quad(s) / {} line(s) ready, "
                   L"{} dropped by the per-frame cap {}{}; last frame cyan {} red {} orange {} "
                   L"pink {} and {} line(s) in {} draw call(s){}; merged {} time(s), last {} ms, "
                   L"longest {} ms; picked the nearest {} time(s), longest {} us; re-sorted only "
                   L"{} time(s), longest {} us)",
                   immediateDrawn,
                   g_quadCount.load(std::memory_order_relaxed),
                   g_edgeCount.load(std::memory_order_relaxed),
                   capped,
                   g_lastCap.load(std::memory_order_relaxed),
                   radius >= 0 ? std::format(L", keeping those within {} block(s) of the camera",
                                             radius)
                               : std::wstring{},
                   g_lastFaces[1].load(std::memory_order_relaxed),
                   g_lastFaces[2].load(std::memory_order_relaxed),
                   g_lastFaces[3].load(std::memory_order_relaxed),
                   g_lastFaces[4].load(std::memory_order_relaxed),
                   g_lastLines.load(std::memory_order_relaxed),
                   g_lastCalls.load(std::memory_order_relaxed),
                   boxes::boxXray()
                       ? L" (see-through faces and outlines in one call, far to near)"
                       : L"",
                   g_mergeCount.exchange(0, std::memory_order_relaxed),
                   g_mergeMsLast.load(std::memory_order_relaxed),
                   g_mergeMsMax.exchange(0, std::memory_order_relaxed),
                   g_selectCount.exchange(0, std::memory_order_relaxed),
                   g_selectUsMax.exchange(0, std::memory_order_relaxed),
                   g_sortCount.exchange(0, std::memory_order_relaxed),
                   g_sortUsMax.exchange(0, std::memory_order_relaxed));
    }
    static unsigned toldFallback = 0;
    if (boxes::boxesOn() && toldFallback < 12) {
        const bool xray = boxes::boxXray();
        const int want = xray ? kDefaultXray
                              : kDefaultFlat;
        if (!active(xray)) {
            ++toldFallback;
            log().warn(L"WorldMesh: the color boxes cannot be drawn ({} material {} / "
                       L"resolved {} / stage calls {} / host calls {}); nothing else draws "
                       L"them since stage FB",
                       xray ? L"see-through (name tag)" : L"not see-through (selection overlay)",
                       want,
                       (want > 0 && !g_modeBroken[want & 7].load(std::memory_order_relaxed))
                           ? L"yes"
                           : L"no",
                       calls,
                       hostCalls);
        }
    }
    if (const unsigned long long skipped = g_edgesSkipped.exchange(0, std::memory_order_relaxed);
        skipped != 0) {
        static std::atomic<bool> toldEdges{false};
        if (!toldEdges.exchange(true, std::memory_order_relaxed)) {
            log().info(L"WorldMesh: {} cell(s) is over {}, so the outlines follow the merged "
                       L"rectangles instead of the shape of each group",
                       g_cellCount.load(std::memory_order_relaxed),
                       kEdgeCellLimit);
        }
    }
    const unsigned long long lines = g_lines.exchange(0, std::memory_order_relaxed);
    if (const unsigned long long noLine = g_noLineMaterial.exchange(0, std::memory_order_relaxed);
        noLine != 0 && lines == 0) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true, std::memory_order_relaxed)) {
            log().warn(L"WorldMesh: the outline material could not be resolved, so color boxes "
                       L"are drawn without outlines ({} frame(s))",
                       noLine);
        }
    }
    if (drawn == 0 && failed == 0 && noMaterial == 0 && camShifted == 0 && noQuads == 0
        && badCtx == 0 && !boxes::boxesOn()) {
        return;
    }
    if (reports >= 60) {
        return;
    }
    ++reports;
    if (boxes::boxesOn() && drawn == 0) {
        const auto list = boxes::boxSnapshot();
        const bool haveBoxes = list && !list->empty();
        if (!haveBoxes) {
            return;
        }
        if (calls == 0 && hostCalls == 0 && badCtx == 0) {
            static std::atomic<bool> toldIdle{false};
            if (!toldIdle.exchange(true, std::memory_order_relaxed)) {
                log().info(L"WorldMesh: the world was not drawn in the last {} seconds "
                           L"(no name-tag stage and no host call), so the color boxes wait",
                           seconds);
            }
            return;
        }
        if (calls == 0 && hostCalls != 0 && g_drawAt.load(std::memory_order_relaxed) == 0) {
            g_drawAt.store(1, std::memory_order_relaxed);
            log().warn(L"WorldMesh: the name-tag stage never ran in {} seconds (host calls "
                       L"{}). Drawing at the end of the host call instead",
                       seconds,
                       hostCalls);
            return;
        }
        static unsigned toldStuck = 0;
        if (toldStuck < 12) {
            ++toldStuck;
            log().warn(L"WorldMesh: {} box(es) are waiting but nothing was drawn in {} seconds "
                       L"(stage calls {} / host calls {} / no faces {} / unreadable context {} "
                       L"/ no material {})",
                       list->size(),
                       seconds,
                       calls,
                       hostCalls,
                       noQuads,
                       badCtx,
                       noMaterial);
        }
    }
}

void shutdown()
{
    g_keptReleaseAsked.store(true, std::memory_order_seq_cst);
    const bool pending = g_installed.load(std::memory_order_acquire)
                         && (g_keptMeshCount.load(std::memory_order_seq_cst) != 0
                             || g_inside.load(std::memory_order_seq_cst));
    for (int i = 0; pending && i < 100 && !g_keptReleased.load(std::memory_order_acquire); ++i) {
        Sleep(10);
    }
    const bool released = g_keptReleased.load(std::memory_order_acquire);
    const std::size_t last = g_keptLastReturned.load(std::memory_order_relaxed);
    const KeptWhy why = g_keptLastWhy.load(std::memory_order_relaxed);
    if (pending && !released) {
        log().warn(L"WorldMesh: the render thread did not come back to release the kept meshes; "
                   L"leaving them in place");
    } else if (released && why == KeptWhy::Released && last != 0) {
        log().info(L"WorldMesh: released {} kept mesh(es) on the render thread", last);
    } else if (last != 0) {
        const wchar_t* const reason = why == KeptWhy::BoxesOff  ? L"when the boxes went off"
                                      : why == KeptWhy::ModeOff ? L"when the box mode was set to none"
                                      : why == KeptWhy::Rebake  ? L"to rebuild them with new settings"
                                      : why == KeptWhy::Broken  ? L"after giving up on them"
                                      : why == KeptWhy::NotKept ? L"when the boxes left the kept drawing"
                                                                : L"on request";
        log().info(L"WorldMesh: no kept mesh left at shutdown (the last {} were returned on the render thread "
                   L"{})", last, reason);
    }
    g_teardown.store(true, std::memory_order_release);
    for (int i = 0; i < 50 && g_inside.load(std::memory_order_acquire); ++i) {
        Sleep(10);
    }
    stopWorker();
}

}
