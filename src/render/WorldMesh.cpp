#include "game/SchematicWorld.h"
#include "render/WorldMesh.h"
#include "render/MeshKit.h"
#include "render/GhostMesh.h"

#include "core/Logger.h"
#include "core/Notice.h"
#include "core/Strings.h"
#include "game/BlockRegistry.h"
#include "game/GameData.h"
#include "hooks/HookManager.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "memory/Signatures.h"
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
using namespace meshkit;

using NameTagStageFn = void(__fastcall*)(void* lrp, void* ctx, void* view, void* extra);
using TessBeginFn = void(__fastcall*)(void* tess, void* unused, std::uint8_t mode, int reserve,
                                      bool faceData);
using TessVertexFn = void(__fastcall*)(void* tess, float x, float y, float z);
using TessColorFn = void(__fastcall*)(void* tess, float r, float g, float b, float a);
using RenderMeshFn = void(__fastcall*)(void* ctx, void* tess, const void* material,
                                       void* texture);

NameTagStageFn g_original = nullptr;
using DrawOneFn = void(__fastcall*)(void*, void*, const void*, void*, void*);
DrawOneFn g_drawOne = nullptr;
std::int32_t g_tagLrpField = 0, g_tagLrpSubField = 0;
std::uint8_t g_fontWidthSlot = 0;
std::atomic<bool> g_tagsReady{false}, g_tagsBroken{false};
std::mutex g_tagMutex;
std::vector<WorldTag> g_worldTags;
std::atomic<unsigned long long> g_tagsDrawn{0}, g_tagsSkipped{0}, g_tagsWantedAt{0};
std::atomic<bool> g_tagsNoMaterial{false};
std::atomic<bool> g_keptBroken{false};
std::atomic<bool> g_linesBroken{false};
std::atomic<bool> g_debugRibbonsBroken{false};
std::atomic<bool> g_fillsBroken{false};
std::mutex g_debugMutex;
std::vector<DebugLine> g_debugLines;
std::vector<DebugLine> g_sourceLines[static_cast<int>(LineSource::Count)];
std::vector<DebugFill> g_debugFills;
std::vector<DebugFill> g_sourceFills[static_cast<int>(LineSource::Count)];
std::atomic<unsigned> g_debugNoMaterial{0};
TessBeginFn g_begin = nullptr;
TessVertexFn g_vertex = nullptr;
TessColorFn g_color = nullptr;
RenderMeshFn g_render = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_teardown{false};

constexpr auto kCtxColor = tesslayout::kCtxColor, kCtxTess = tesslayout::kCtxTess;
constexpr auto kTessLimit = tesslayout::kLimit, kTessBuilding = tesslayout::kBuilding, kTessVoid = tesslayout::kVoid;
constexpr auto kTessNoColor = tesslayout::kNoColor, kTessOffset = tesslayout::kOffset, kTessScale = tesslayout::kScale;
constexpr auto kTessUseMatrix = tesslayout::kUseMatrix, kLrpCamera = tesslayout::kLrpCamera;
constexpr auto kTessReadable = tesslayout::kReadable;
struct Choice {
    int mode;
    const char* name;
    std::ptrdiff_t lrpOffset;
    bool multiply;
};
constexpr Choice kChoices[] = {
    {1, "selection_overlay", 0x1088, true},
    {2, "name_tag", 0, false},
    {5, "name_tag_depth_tested", 0, false},
    {6, "selection_box", 0x10e8, false},
    {7, "name_tag_text", 0, false},
};
constexpr int kDefaultFlat = 1;
constexpr int kDefaultXray = 2;

constexpr int kLineMode = 6;
constexpr int kDebugRibbonMode = 5;
constexpr int kDebugOnTopMode = 2;
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

std::atomic<unsigned long long> g_calls{0};
std::atomic<unsigned long long> g_drawn{0};
std::atomic<unsigned long long> g_noMaterial{0};
std::atomic<unsigned long long> g_badCtx{0};
std::atomic<unsigned long long> g_camShifted{0};
std::atomic<unsigned long long> g_noLineMaterial{0};
std::atomic<std::size_t> g_keptMeshCount{0};
std::atomic<bool> g_inside{false};

__declspec(noinline) void callOriginal(void* lrp, void* ctx, void* view, void* extra)
{
    g_original(lrp, ctx, view, extra);
}

char g_longNames[std::size(kChoices)][64]{};

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
        makeHashed(name, g_longNames[slot], sizeof(g_longNames[slot]), buf);
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
        void* info = nullptr;
        if (!memory::copyGuarded(at, &info, sizeof(info))) {
            return nullptr;
        }
        if (!infoMatches(info, fnv1(choice->name))) {
            static std::atomic<unsigned> told{0};
            if (told.fetch_add(1, std::memory_order_relaxed) < 3) {
                log().warn(L"WorldMesh: the material at LRP{:+#x} is not {} (object {:#x}). "
                           L"Color boxes are not drawn in this view",
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

struct RegionOut {
    boxmesh::RegionKey key;
    std::uint64_t generation = 0;
    std::shared_ptr<const boxmesh::RegionGeometry> geometry;
};

struct Prepared {
    bool xray = false;
    bool retained = false;
    std::vector<RegionOut> regions;
};
Prepared g_prepared;
std::uint64_t g_preparedSerial = 0;

bool keptAvailable()
{
    return meshkit::ready()
           && !g_keptBroken.load(std::memory_order_relaxed);
}

constexpr double kRibbonPixels = 1.5;
constexpr double kRibbonNearW = 0.05;

using MeshRequest = boxes::SectionRequest;
std::mutex g_workLock;
std::condition_variable g_workCv;
bool g_workHas = false;
bool g_workStop = false;
bool g_resultReady = false;
Prepared g_result;
HANDLE g_worker = nullptr;
std::atomic<bool> g_workerRunning{false};
unsigned long long g_requestedVersion = ~0ULL;

struct Regions {
    bool xray = false;
    boxmesh::RegionSet set;
};
Regions g_regions;

const schematic::DiffSection* neighborDiff(void* ctx, const schematic::SectionKey& key)
{
    const auto& snap = *static_cast<const schematic::RenderSnapshot*>(ctx);
    const auto it = snap.at.find(key);
    return it == snap.at.end() ? nullptr : snap.sections[it->second].diff.get();
}

void copyPrepared(const Regions& state, Prepared& out)
{
    out.xray = state.xray; out.retained = true;
    out.regions.clear(); out.regions.reserve(state.set.entries().size());
    for (const auto& [key, entry] : state.set.entries())
        out.regions.push_back(RegionOut{key, entry.generation, entry.geometry});
}

void buildRegions(const MeshRequest& request, Regions& state, Prepared& out)
{
    std::vector<boxmesh::RegionChange> changes;
    for (const auto& key : request.remove) changes.push_back({{key.cx, key.cy, key.cz}, {}});
    const auto snap = schematicworld::renderSnapshot();
    for (const auto& key : request.collect) {
        boxmesh::RegionChange change{{key.cx, key.cy, key.cz}, {}};
        if (snap) {
            const auto it = snap->at.find(key);
            if (it != snap->at.end()) {
                const auto& view = snap->sections[it->second];
                if (view.diff) {
                    std::vector<schematic::BoxCell> boxes;
                    schematic::collectBoxesInSection(key, *view.diff, neighborDiff,
                        const_cast<schematic::RenderSnapshot*>(snap.get()), snap->layer, !request.xray, boxes,
                        snap->selectedColor, snap->highlightFiltered ? &snap->highlight : nullptr);
                    change.cells.reserve(boxes.size());
                    for (const auto& box : boxes)
                        change.cells.push_back({box.x, box.y, box.z, box.color, box.covered, box.nearMask});
                }
            }
        }
        changes.push_back(std::move(change));
    }
    state.set.apply(changes, request.reset, request.xray);
    state.xray = request.xray;
    out.xray = state.xray; out.retained = true;
}

bool buildPreparedSafely(const MeshRequest& request, Regions& regions, Prepared& out)
{
    try {
        buildRegions(request, regions, out);
        return true;
    } catch (const std::exception&) {
        regions = Regions{};
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) {
            log().warn(L"WorldMesh: preparing the boxes ran out of memory ({} box(es) in the list); nothing is "
                       L"drawn until the list changes",
                       request.collect.size());
        }
        return false;
    }
}

Prepared emptyPrepared(const MeshRequest& request)
{
    Prepared out;
    out.xray = request.xray;
    out.retained = true;
    return out;
}

DWORD WINAPI workerLoop(LPVOID)
{
    std::unordered_map<schematic::SectionKey, bool, schematic::SectionKeyHash> pending;
    std::vector<schematic::SectionKey> order;
    std::size_t cursor = 0;
    unsigned long long observed = ~0ULL;
    bool xray = false;
    auto publish = [&](Prepared made) {
        std::lock_guard lock(g_workLock);
        g_result = std::move(made); g_resultReady = true;
    };
    for (;;) {
        {
            std::unique_lock lock(g_workLock);
            g_workCv.wait(lock, [&] { return g_workStop || g_workHas || !pending.empty(); });
            if (g_workStop) return 0;
            g_workHas = false;
        }
        const auto started = GetTickCount64();
        Prepared made;
        bool changed = false;
        try {
        do {
            { std::lock_guard lock(g_workLock); if (g_workStop) return 0; }
            const auto version = boxes::sectionRequestVersion();
            if (observed != version) {
                observed = version;
                MeshRequest incoming;
                if (boxes::takeSectionRequests(incoming)) {
                    if (incoming.reset) {
                        pending.clear();
                        MeshRequest reset{true, incoming.xray, {}, {}};
                        if (!buildPreparedSafely(reset, g_regions, made)) made = emptyPrepared(reset);
                        changed = true;
                    }
                    xray = incoming.xray;
                    for (const auto& key : incoming.collect) pending[key] = true;
                    for (const auto& key : incoming.remove) pending[key] = false;
                    order.clear(); order.reserve(pending.size());
                    for (const auto& [key, collect] : pending) { (void)collect; order.push_back(key); }
                    float eye[3]{}, vp[16]{};
                    if (boxes::cameraSnapshot(eye, vp)) {
                        const auto distance = [&](const schematic::SectionKey& key) {
                            const double dx = key.cx * 16.0 + 8 - eye[0], dy = key.cy * 16.0 + 8 - eye[1],
                                         dz = key.cz * 16.0 + 8 - eye[2];
                            return dx * dx + dy * dy + dz * dz;
                        };
                        std::sort(order.begin(), order.end(), [&](const auto& l, const auto& r) { return distance(l) < distance(r); });
                    }
                    cursor = 0;
                }
            }
            if (pending.empty()) break;
            const auto key = order[cursor++];
            const bool collect = pending.at(key); pending.erase(key);
            MeshRequest request;
            request.xray = xray;
            if (collect) request.collect.push_back(key); else request.remove.push_back(key);
            if (!buildPreparedSafely(request, g_regions, made)) {
                made = emptyPrepared(request); pending.clear();
            }
            changed = true;
        } while (GetTickCount64() - started < 50);
        if (changed) {
            copyPrepared(g_regions, made);
        }
        } catch (const std::exception&) {
            g_regions = Regions{}; pending.clear(); order.clear(); cursor = 0;
            made = emptyPrepared({false, xray, {}, {}}); changed = true;
            static bool told = false;
            if (!told) { told = true; log().warn(L"WorldMesh: merging the section requests ran out of memory"); }
        }
        if (changed) publish(std::move(made));
    }
}

void prepare(bool xray)
{
    (void)xray;
    if (!g_workerRunning.load(std::memory_order_acquire)) return;
    {
        std::lock_guard lock(g_workLock);
        if (g_resultReady) {
            g_prepared = std::move(g_result); g_resultReady = false; ++g_preparedSerial;
        }
    }
    const auto version = boxes::sectionRequestVersion();
    if (version != g_requestedVersion) {
        g_requestedVersion = version;
        { std::lock_guard lock(g_workLock); g_workHas = true; }
        g_workCv.notify_one();
    }
}

void startWorker()
{
    g_worker = CreateThread(nullptr, 0, &workerLoop, nullptr, 0, nullptr);
    if (g_worker == nullptr) {
        notice::failOnce("WorldMesh.worker",
                         L"WorldMesh: could not start the merge thread; color boxes are not drawn",
                         "Color boxes are not drawn: the background thread could not be started");
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
            while (WaitForSingleObject(g_worker, INFINITE) != WAIT_OBJECT_0) Sleep(1);
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
    const DebugLine* debugLines;
    const DebugFill* debugFills;
    std::size_t edgeCount;
    float rgba[4];
    std::uint32_t limit;
    double ribbonScale;
    double forward[3];
    double forwardW;
    bool haveForward;
    bool busy;
    bool failed;
    bool ribbonFailed;
    bool fillFailed;
    DWORD code;
};

void emitRect(const Job& job, const boxmesh::Quad& quad)
{
    std::int32_t c[4][3] = {};
    boxmesh::corners(quad, c);
    float v[4][3] = {};
    for (int i = 0; i < 4; ++i) {
        for (int k = 0; k < 3; ++k) {
            v[i][k] = static_cast<float>(static_cast<double>(c[i][k]) - job.cam[k]);
        }
    }
    void* const tess = job.tess;
    for (int i = 0; i < 4; ++i) {
        g_vertex(tess, v[i][0], v[i][1], v[i][2]);
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

void emitDebugEdge(const Job& job, const DebugLine& edge)
{
    for (const double* point : {edge.a, edge.b}) {
        g_vertex(job.tess,
                 static_cast<float>(point[0] - job.cam[0]),
                 static_cast<float>(point[1] - job.cam[1]),
                 static_cast<float>(point[2] - job.cam[2]));
    }
}

__declspec(noinline) void runLineJob(Job& job)
{
    if (g_linesBroken.load(std::memory_order_relaxed)) return;
    bool building = false;
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
            building = true;
            if (tess[kTessBuilding] == 0) {
                building = false;
                job.busy = true;
                return;
            }
            const float zero[3] = {0.0F, 0.0F, 0.0F};
            const float one[3] = {1.0F, 1.0F, 1.0F};
            std::memcpy(tess + kTessOffset, zero, sizeof(zero));
            std::memcpy(tess + kTessScale, one, sizeof(one));
            tess[kTessUseMatrix] = 0;
            tess[kTessNoColor] = 1;
            std::uint32_t used = 0;
            for (; at < job.edgeCount; ++at) {
                if (used + 2U > limit && used != 0) {
                    break;
                }
                emitDebugEdge(job, job.debugLines[at]);
                used += 2U;
            }
            if (color != nullptr) {
                std::memcpy(color, job.rgba, sizeof(job.rgba));
                reinterpret_cast<unsigned char*>(color)[0x10] = 1;
            }
            alignas(16) unsigned char texture[0x40] = {};
            g_render(job.ctx, tess, job.material, texture);
            building = false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job.failed = true;
        job.code = GetExceptionCode();
        if (building) resetTessellator(job.tess);
        g_linesBroken.store(true, std::memory_order_relaxed);
    }
}

void emitDebugFill(const Job& job, const DebugFill& fill)
{
    double v[4][3] = {};
    for (int i = 0; i < 4; ++i) {
        for (int k = 0; k < 3; ++k) v[i][k] = fill.p[i][k] - job.cam[k];
    }
    const double e1[3] = {v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2]};
    const double e2[3] = {v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2]};
    const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    const bool front = -(n[0] * v[0][0] + n[1] * v[0][1] + n[2] * v[0][2]) >= 0.0;
    for (int j = 0; j < 4; ++j) {
        const int i = front ? j : 3 - j;
        g_vertex(job.tess, static_cast<float>(v[i][0]), static_cast<float>(v[i][1]), static_cast<float>(v[i][2]));
    }
}

__declspec(noinline) void runFillJob(Job& job)
{
    if (g_fillsBroken.load(std::memory_order_relaxed)) return;
    bool building = false;
    __try {
        auto* const tess = static_cast<unsigned char*>(job.tess);
        float* color = nullptr;
        std::memcpy(&color, static_cast<unsigned char*>(job.ctx) + kCtxColor, sizeof(color));
        const std::uint32_t limit = job.limit & ~3U;
        std::size_t at = 0;
        while (at < job.edgeCount) {
            if (tess[kTessBuilding] != 0 || tess[kTessVoid] != 0) {
                job.busy = true;
                return;
            }
            g_begin(tess, nullptr, kPrimitiveQuads, 0, false);
            building = true;
            if (tess[kTessBuilding] == 0) {
                building = false;
                job.busy = true;
                return;
            }
            const float zero[3] = {0.0F, 0.0F, 0.0F};
            const float one[3] = {1.0F, 1.0F, 1.0F};
            std::memcpy(tess + kTessOffset, zero, sizeof(zero));
            std::memcpy(tess + kTessScale, one, sizeof(one));
            tess[kTessUseMatrix] = 0;
            tess[kTessNoColor] = 1;
            std::uint32_t used = 0;
            for (; at < job.edgeCount; ++at) {
                if (used + 4U > limit && used != 0) {
                    break;
                }
                emitDebugFill(job, job.debugFills[at]);
                used += 4U;
            }
            if (color != nullptr) {
                std::memcpy(color, job.rgba, sizeof(job.rgba));
                reinterpret_cast<unsigned char*>(color)[0x10] = 1;
            }
            alignas(16) unsigned char texture[0x40] = {};
            g_render(job.ctx, tess, job.material, texture);
            building = false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job.failed = true;
        job.code = GetExceptionCode();
        if (building) resetTessellator(job.tess);
        g_fillsBroken.store(true, std::memory_order_relaxed);
        job.fillFailed = true;
    }
}

bool ribbonVisible(const Job& job, const DebugLine& line)
{
    double p[2][3] = {};
    for (int k = 0; k < 3; ++k) {
        p[0][k] = line.a[k] - job.cam[k];
        p[1][k] = line.b[k] - job.cam[k];
    }
    double v[4][3] = {};
    return boxmesh::ribbonCorners(p[0], p[1], job.haveForward ? job.forward : nullptr, job.forwardW,
                                  job.ribbonScale * line.width, kRibbonNearW, v);
}

bool emitDebugRibbon(const Job& job, const DebugLine& line)
{
    double p[2][3] = {};
    for (int k = 0; k < 3; ++k) {
        p[0][k] = line.a[k] - job.cam[k];
        p[1][k] = line.b[k] - job.cam[k];
    }
    double v[4][3] = {};
    if (!boxmesh::ribbonCorners(p[0], p[1], job.haveForward ? job.forward : nullptr, job.forwardW,
                                job.ribbonScale * line.width, kRibbonNearW, v)) {
        return false;
    }
    for (int i = 3; i >= 0; --i) {
        g_vertex(job.tess, static_cast<float>(v[i][0]), static_cast<float>(v[i][1]), static_cast<float>(v[i][2]));
    }
    return true;
}

__declspec(noinline) void runDebugRibbonJob(Job& job)
{
    if (g_debugRibbonsBroken.load(std::memory_order_relaxed)) return;
    bool building = false;
    __try {
        auto* const tess = static_cast<unsigned char*>(job.tess);
        float* color = nullptr;
        std::memcpy(&color, static_cast<unsigned char*>(job.ctx) + kCtxColor, sizeof(color));
        const std::uint32_t limit = job.limit & ~3U;
        std::size_t at = 0;
        while (at < job.edgeCount) {
            while (at < job.edgeCount && !ribbonVisible(job, job.debugLines[at])) ++at;
            if (at >= job.edgeCount) {
                break;
            }
            if (tess[kTessBuilding] != 0 || tess[kTessVoid] != 0) {
                job.busy = true;
                return;
            }
            g_begin(tess, nullptr, kPrimitiveQuads, 0, false);
            building = true;
            if (tess[kTessBuilding] == 0) {
                building = false;
                job.busy = true;
                return;
            }
            const float zero[3] = {0.0F, 0.0F, 0.0F};
            const float one[3] = {1.0F, 1.0F, 1.0F};
            std::memcpy(tess + kTessOffset, zero, sizeof(zero));
            std::memcpy(tess + kTessScale, one, sizeof(one));
            tess[kTessUseMatrix] = 0;
            tess[kTessNoColor] = 1;
            std::uint32_t used = 0;
            for (; at < job.edgeCount; ++at) {
                if (used + 4U > limit && used != 0) {
                    break;
                }
                if (emitDebugRibbon(job, job.debugLines[at])) {
                    used += 4U;
                }
            }
            if (color != nullptr) {
                std::memcpy(color, job.rgba, sizeof(job.rgba));
                reinterpret_cast<unsigned char*>(color)[0x10] = 1;
            }
            alignas(16) unsigned char texture[0x40] = {};
            g_render(job.ctx, tess, job.material, texture);
            building = false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job.failed = true;
        job.code = GetExceptionCode();
        if (building) resetTessellator(job.tess);
        g_debugRibbonsBroken.store(true, std::memory_order_relaxed);
        job.ribbonFailed = true;
    }
}

void drawDebugLines(void* lrp, void* ctx)
{
    std::vector<DebugLine> lines;
    std::vector<DebugFill> fills;
    {
        std::lock_guard lock(g_debugMutex);
        lines = g_debugLines;
        fills = g_debugFills;
    }
    if ((lines.empty() && fills.empty()) || lrp == nullptr || ctx == nullptr) return;
    void* tess = nullptr;
    float cam[3]{};
    std::uint32_t limit = 0;
    if (!memory::copyGuarded(static_cast<std::byte*>(ctx) + kCtxTess, &tess, sizeof(tess))
        || tess == nullptr
        || !memory::copyGuarded(static_cast<std::byte*>(lrp) + kLrpCamera, cam, sizeof(cam))
        || !memory::copyGuarded(static_cast<std::byte*>(tess) + kTessLimit, &limit, sizeof(limit))
        || !std::isfinite(cam[0]) || !std::isfinite(cam[1]) || !std::isfinite(cam[2])) return;
    const void* material = materialFor(lrp, kLineMode);
    if (material == nullptr) {
        g_debugNoMaterial.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool anyThick = std::any_of(lines.begin(), lines.end(), [](const DebugLine& l) { return l.width > 1.0F; });
    const void* const ribbonMaterial = anyThick ? materialFor(lrp, kDebugRibbonMode) : nullptr;
    const bool anyOnTop = std::any_of(lines.begin(), lines.end(), [](const DebugLine& l) { return l.onTop; });
    const void* const onTopMaterial = anyOnTop ? materialFor(lrp, kDebugOnTopMode) : nullptr;
    TessSaved saved{};
    if (!saveState(ctx, tess, saved)) return;
    Job job{};
    job.ctx = ctx;
    job.tess = tess;
    job.material = material;
    job.cam[0] = cam[0];
    job.cam[1] = cam[1];
    job.cam[2] = cam[2];
    job.debugLines = lines.data();
    job.edgeCount = lines.size();
    job.limit = (limit >= 64 && limit <= (1U << 24)) ? limit : 65532;
    const bool anyMultiply = std::any_of(fills.begin(), fills.end(), [](const DebugFill& f) { return !f.blend; });
    const bool anyBlend = std::any_of(fills.begin(), fills.end(), [](const DebugFill& f) { return f.blend; });
    const void* const fillMaterial = anyMultiply ? materialFor(lrp, kDefaultFlat) : nullptr;
    const void* const blendMaterial = anyBlend ? materialFor(lrp, kDebugRibbonMode) : nullptr;
    for (std::size_t at = 0; at < fills.size();) {
        std::size_t end = at + 1;
        while (end < fills.size() && fills[end].blend == fills[at].blend
               && std::equal(fills[at].rgba, fills[at].rgba + 4, fills[end].rgba)) ++end;
        const void* const groupMaterial = fills[at].blend ? blendMaterial : fillMaterial;
        if (groupMaterial == nullptr) {
            at = end;
            continue;
        }
        job.debugFills = fills.data() + at;
        job.edgeCount = end - at;
        std::copy_n(fills[at].rgba, 4, job.rgba);
        job.material = groupMaterial;
        runFillJob(job);
        if (job.failed || job.busy) break;
        at = end;
    }
    job.material = material;
    job.debugLines = lines.data();
    const bool linesBlocked = job.failed || job.busy;
    if (anyThick) {
        double yScale = 0.0;
        float eye[3] = {};
        float vp[16] = {};
        if (boxes::cameraSnapshot(eye, vp)) {
            const double col1 = std::sqrt(static_cast<double>(vp[1]) * vp[1] + static_cast<double>(vp[5]) * vp[5]
                                          + static_cast<double>(vp[9]) * vp[9]);
            const double col3 = std::sqrt(static_cast<double>(vp[3]) * vp[3] + static_cast<double>(vp[7]) * vp[7]
                                          + static_cast<double>(vp[11]) * vp[11]);
            if (std::isfinite(col1) && col1 > 0.05 && col1 < 100.0) yScale = col1;
            if (std::isfinite(col3) && col3 > 1e-3 && std::isfinite(vp[15])) {
                job.forward[0] = vp[3];
                job.forward[1] = vp[7];
                job.forward[2] = vp[11];
                job.forwardW = vp[15];
                job.haveForward = true;
            }
        }
        const render::Viewport view = render::overlayViewport();
        if (yScale > 0.0 && view.valid && view.height >= 64.0F) {
            job.ribbonScale = 2.0 / (yScale * view.height);
        } else {
            job.haveForward = false;
        }
    }
    for (std::size_t at = 0; !linesBlocked && at < lines.size();) {
        const float* color = lines[at].rgba;
        const float width = lines[at].width;
        const bool onTop = lines[at].onTop;
        std::size_t end = at + 1;
        while (end < lines.size() && std::equal(color, color + 4, lines[end].rgba) && lines[end].width == width
               && lines[end].onTop == onTop) ++end;
        job.debugLines = lines.data() + at;
        job.edgeCount = end - at;
        std::copy_n(color, 4, job.rgba);
        if (onTop) {
            if (onTopMaterial == nullptr) {
                at = end;
                continue;
            }
            job.material = onTopMaterial;
            runLineJob(job);
        } else if (width > 1.0F) {
            if (ribbonMaterial == nullptr || !job.haveForward) {
                at = end;
                continue;
            }
            job.material = ribbonMaterial;
            runDebugRibbonJob(job);
        } else {
            job.material = material;
            runLineJob(job);
        }
        if (job.failed || job.busy) break;
        at = end;
    }
    restoreState(ctx, tess, saved);
    if (job.failed) {
        const bool ribbons = job.ribbonFailed;
        const bool filled = job.fillFailed;
        notice::failOnce(filled ? "WorldMesh.debugFillFailed" : ribbons ? "WorldMesh.debugRibbonFailed" : "WorldMesh.lineFailed",
                         std::format(L"WorldMesh: {} failed (exception {:08X}); this drawing path is disabled",
                                     filled ? L"debug fills" : ribbons ? L"debug ribbons" : L"lines", job.code),
                         "Debug lines: drawing failed inside the game; the affected drawing path is disabled");
    }
    if (anyBlend && blendMaterial == nullptr) {
        notice::failOnce("WorldMesh.blendFill",
                         L"WorldMesh: the name_tag_depth_tested material was not found; blended fills are not drawn",
                         "Light Overlay is not drawn: its material was not found");
    }
    if (anyMultiply && fillMaterial == nullptr) {
        notice::failOnce("WorldMesh.fill",
                         L"WorldMesh: the selection_overlay material was not found; debug fills are not drawn",
                         "Debug fills (Flexible Placement overlay) are not drawn: their material was not found");
    }
    if (anyOnTop && onTopMaterial == nullptr) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true, std::memory_order_relaxed)) {
            log().warn(L"WorldMesh: the always-on-top debug lines are not drawn (name_tag material missing)");
        }
    }
    if (anyThick && ribbonMaterial == nullptr) {
        notice::failOnce("WorldMesh.ribbon",
                         L"WorldMesh: the material for thick debug lines was not found; thick debug lines are not drawn",
                         "Thick debug lines (F3+B / F3+G) are not drawn: their material was not found");
    }
}

struct KeptMesh {
    std::unique_ptr<MeshStorage> storage;
    std::uint8_t color = 0;
    bool line = false;
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
float g_keptXrayPalette[2][boxmesh::kMaxColor + 1][4] = {};
std::uint64_t g_keptPaletteVersion = 1;
std::atomic<bool> g_keptReleaseAsked{false};
std::atomic<bool> g_keptReleased{false};
enum class KeptWhy : std::uint8_t {
    Released,
    BoxesOff,
    Broken,
};
std::atomic<std::size_t> g_keptLastReturned{0};
std::atomic<KeptWhy> g_keptLastWhy{KeptWhy::Released};

struct KeptBuild {
    void* tess;
    double origin[3];
    double eye[3];
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
    for (int i = 3; i >= 0; --i) {
        put(i);
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

bool renderKeptMesh(void* ctx, const void* material, TopMatrix& m, void* mesh,
                    const float rgba[4], const float position[3], const float sortPos[3])
{
    return renderMesh(ctx, material, m, mesh, rgba, position, sortPos, nullptr, false);
}

void destroyKeptMesh(void* mesh) { destroyMesh(mesh); }

void destroyKeptRegion(KeptRegion& region)
{
    for (KeptMesh& mesh : region.meshes) {
        if (mesh.storage) {
            destroyKeptMesh(mesh.storage->bytes);
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
    g_keptMeshCount.store(0, std::memory_order_relaxed);
}

void keptBroke(const wchar_t* where)
{
    if (!g_keptBroken.exchange(true)) {
        notice::failOnce("WorldMesh.keptBroke",
                         std::wstring(L"WorldMesh: a kept mesh failed (") + where + L"); color boxes are not drawn",
                         "Color boxes are not drawn: drawing them failed inside the game");
    }
    destroyKept(KeptWhy::Broken);
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
        boxmesh::paintOrder(quads, edges, eye, order);
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
        const KeptMade made = buildKeptRegion(source, tess, frame, fresh);
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
        g_keptMeshes += it->second.meshes.size();
        if (candidate.content) {
            g_keptPending.erase(candidate.key);
        }
        ++done;
    }
    return done != 0;
}

void drawKeptFrameBody(void* lrp, void* ctx, void* tess, const float camF[3], std::uint32_t limit, bool xray,
                       const void* flatMaterial, const void* xrayMaterial)
{
    KeptFrame frame;
    frame.xray = xray;
    frame.cam[0] = camF[0];
    frame.cam[1] = camF[1];
    frame.cam[2] = camF[2];
    frame.limit = limit;
    const float faceAlpha = boxes::boxFaceAlpha();
    float faceRgba[boxmesh::kMaxColor + 1][4] = {};
    float lineRgba[boxmesh::kMaxColor + 1][4] = {};
    bool shown[boxmesh::kMaxColor + 1] = {};
    for (std::size_t c = 1; c <= boxmesh::kMaxColor; ++c) {
        float rgb[3] = {};
        float alpha = faceAlpha;
        if (!boxes::boxStyle(static_cast<schematic::DiffColor>(c), rgb, &alpha)) {
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
            if (std::isfinite(col1) && col1 > 0.05 && col1 < 200.0) {
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
    const bool progressed = rebuildKept(tess, frame, waiting);
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
    for (const auto& [key, region] : g_kept) {
        if (!regionVisible(region.origin, frame.cam, vp, haveVp)) {
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
    bool failed = false;
    bool matrixOk = false;
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
            }
            if (failed) {
                break;
            }
        }
        restoreTopMatrix(m);
    }
    restoreState(ctx, tess, saved);
    if (failed) {
        keptBroke(L"draw");
        return;
    }
    const bool stalled = !matrixOk || (waiting && !progressed);
    g_keptStall = stalled ? g_keptStall + 1 : 0;
    if (g_keptStall >= kKeptStallFrames) {
        keptBroke(matrixOk ? L"the tessellator stayed busy" : L"the matrix stack was unreadable");
        return;
    }
    g_drawn.fetch_add(1, std::memory_order_relaxed);
    g_keptMeshCount.store(g_keptMeshes, std::memory_order_relaxed);
}

void drawKeptFrame(void* lrp, void* ctx, void* tess, const float camF[3], std::uint32_t limit, bool xray,
                   const void* flatMaterial, const void* xrayMaterial)
{
    TessSaved outer{};
    const bool saved = saveState(ctx, tess, outer);
    try {
        drawKeptFrameBody(lrp, ctx, tess, camF, limit, xray, flatMaterial, xrayMaterial);
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
    if (ctx == nullptr || lrp == nullptr || !pageChecked(ctx, kCtxTess + 8, false)) {
        g_badCtx.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    void* tess = nullptr;
    std::memcpy(&tess, static_cast<unsigned char*>(ctx) + kCtxTess, sizeof(tess));
    if (tess == nullptr || !pageChecked(tess, kTessReadable, false)) {
        g_badCtx.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const void* const material = materialFor(lrp, mode);
    if (material == nullptr) {
        g_noMaterial.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!keptAvailable()) {
        if (!g_kept.empty()) {
            destroyKept(KeptWhy::Broken);
        }
        return;
    }

    float cam[3] = {};
    if (!pageChecked(static_cast<unsigned char*>(lrp) + kLrpCamera, sizeof(cam), false)) {
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
    prepare(xray);
    if (g_prepared.retained) {
        const bool keptXray = g_prepared.xray;
        const int flatMode = kDefaultFlat;
        const int xrayMode = kDefaultXray;
        const void* const flatMaterial = materialFor(lrp, flatMode);
        const void* const xrayMaterial = materialFor(lrp, xrayMode);
        if ((keptXray ? xrayMaterial : flatMaterial) != nullptr) {
            drawKeptFrame(lrp, ctx, tess, cam, limit, keptXray, flatMaterial, xrayMaterial);
        } else {
            g_noMaterial.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
}

bool tagContext(void* lrp, void*& arg5)
{
    __try {
        const auto* nested = *reinterpret_cast<const unsigned char**>(static_cast<unsigned char*>(lrp) + g_tagLrpField);
        if (nested == nullptr) return false;
        arg5 = *reinterpret_cast<void* const*>(nested + g_tagLrpSubField);
        return arg5 != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool tagWidth(void* font, const char* text, std::size_t size, int& width)
{
    struct TextView { const char* data; std::size_t size; } value{text, size};
    __try {
        auto* table = *static_cast<unsigned char**>(font);
        using WidthFn = int(__fastcall*)(void*, const TextView*, float, int);
        const auto fn = *reinterpret_cast<WidthFn*>(table + g_fontWidthSlot);
        width = fn(font, &value, 1.0F, 0);
        return width >= 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool callDrawOne(void* ctx, void* view, const void* entry, void* font, void* arg5)
{
    __try { g_drawOne(ctx, view, entry, font, arg5); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void resolveWorldTags(const Scanner& scanner)
{
    const auto* draw = scanner.address(Target::NameTagDrawCall);
    const auto* width = scanner.address(Target::NameTagFontWidthCall);
    unsigned char a[43]{}, b[26]{};
    constexpr unsigned char stride[]{0x49, 0x81, 0xC7, 0x90, 0, 0, 0};
    bool ok = draw && width && memory::copyGuarded(draw, a, sizeof(a))
        && memory::copyGuarded(width, b, sizeof(b)) && a[31] == 0xE8
        && std::memcmp(a + 36, stride, sizeof(stride)) == 0 && b[20] == 0xFF && b[21] == 0x15;
    if (ok) {
        std::memcpy(&g_tagLrpField, a + 3, sizeof(g_tagLrpField));
        std::memcpy(&g_tagLrpSubField, a + 10, sizeof(g_tagLrpSubField));
        g_fontWidthSlot = b[6];
        g_drawOne = reinterpret_cast<DrawOneFn>(memory::ripTarget(draw, 32));
        ok = g_tagLrpField > 0 && g_tagLrpSubField > 0 && g_fontWidthSlot > 0
            && g_fontWidthSlot < 0x80 && g_fontWidthSlot % sizeof(void*) == 0
            && g_drawOne && memory::isReadable(reinterpret_cast<const void*>(g_drawOne), 1);
    }
    g_tagsReady.store(ok, std::memory_order_release);
    if (ok) log().info(L"WorldMesh: world tags ready (LRP +{:#x}, nested +{:#x}, font slot {:#x})",
        g_tagLrpField, g_tagLrpSubField, g_fontWidthSlot);
    else log().warn(L"WorldMesh: world tags unavailable (name tag call or font width did not validate)");
}

void drawWorldTags(void* lrp, void* ctx, void* view, void* extra)
{
    std::vector<WorldTag> tags;
    { std::lock_guard lock(g_tagMutex); tags = g_worldTags; }
    if (tags.empty() || !worldTagsAvailable() || !lrp || !ctx || !view || !extra) return;
    const auto* back = materialFor(lrp, 2);
    const auto* textMaterial = materialFor(lrp, 7);
    if (!back || !textMaterial) { g_tagsNoMaterial.store(true); g_tagsSkipped.fetch_add(tags.size()); return; }
    float camera[3]{};
    void* arg5 = nullptr;
    if (!memory::copyGuarded(view, camera, sizeof(camera)) || !std::isfinite(camera[0])
        || !std::isfinite(camera[1]) || !std::isfinite(camera[2]) || !tagContext(lrp, arg5)) {
        g_tagsSkipped.fetch_add(tags.size()); return;
    }
    constexpr double kTagDrawDistance = 64.0;
    for (const auto& tag : tags) {
        const double dx = tag.pos[0] - camera[0], dy = tag.pos[1] - camera[1], dz = tag.pos[2] - camera[2];
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const double pull = distance > kTagDrawDistance ? kTagDrawDistance / distance : 1.0;
        const float scale = static_cast<float>(tag.size * std::max(distance * pull, 4.0) / 8.0);
        const float position[]{static_cast<float>(camera[0] + dx * pull),
            static_cast<float>(camera[1] + dy * pull + tag.lift * scale * (1.6 / 60.0)), static_cast<float>(camera[2] + dz * pull)};
        if (!std::isfinite(distance) || !std::isfinite(scale) || scale <= 0 || !std::isfinite(position[0])
            || !std::isfinite(position[1]) || !std::isfinite(position[2])) { ++g_tagsSkipped; continue; }
        std::string text = tag.text.empty() ? " " : tag.text;
        if (tag.appendDistance) text += std::format("  {:.0f}m", std::round(distance));
        int width = 0;
        if (!tagWidth(extra, text.data(), text.size(), width)) { ++g_tagsSkipped; continue; }
        const int halfWidth = std::max(width / 2, tag.minHalfWidth);
        alignas(16) unsigned char entry[0x90]{};
        const std::size_t length = text.size(), capacity = length < 16 ? 15 : length;
        if (length < 16) std::memcpy(entry, text.c_str(), length + 1);
        else { const char* buffer = text.c_str(); std::memcpy(entry, &buffer, sizeof(buffer)); }
        std::memcpy(entry + 0x10, &length, sizeof(length));
        std::memcpy(entry + 0x18, &capacity, sizeof(capacity));
        const void* background = tag.backRgba[3] > 0 ? back : nullptr;
        std::memcpy(entry + 0x20, &background, sizeof(background));
        std::memcpy(entry + 0x28, &textMaterial, sizeof(textMaterial));
        std::memcpy(entry + 0x30, tag.backRgba, sizeof(tag.backRgba));
        std::memcpy(entry + 0x40, tag.textRgba, sizeof(tag.textRgba));
        std::memcpy(entry + 0x50, position, sizeof(position));
        std::memcpy(entry + 0x5c, &scale, sizeof(scale));
        std::memcpy(entry + 0x60, &halfWidth, sizeof(halfWidth));
        entry[0x70] = 1;
        const float spacing = 10.0F;
        std::memcpy(entry + 0x8c, &spacing, sizeof(spacing));
        if (!callDrawOne(ctx, view, entry, extra, arg5)) {
            g_tagsBroken.store(true, std::memory_order_release); ++g_tagsSkipped; break;
        }
        ++g_tagsDrawn;
    }
}
void __fastcall detourNameTagStage(void* lrp, void* ctx, void* view, void* extra)
{
    callOriginal(lrp, ctx, view, extra);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_teardown.load(std::memory_order_acquire)) {
        return;
    }
    if (g_inside.exchange(true, std::memory_order_seq_cst)) {
        return;
    }
    ghostmesh::drawFrame(lrp, ctx);
    drawBoxes(lrp, ctx);
    drawDebugLines(lrp, ctx);
    drawWorldTags(lrp, ctx, view, extra);
    g_inside.store(false, std::memory_order_release);
}

}

void setWorldTags(std::vector<WorldTag> tags)
{
    std::lock_guard lock(g_tagMutex);
    if (!tags.empty() && g_worldTags.empty()) g_tagsWantedAt.store(GetTickCount64());
    if (tags.empty()) g_tagsWantedAt.store(0);
    g_worldTags = std::move(tags);
}

bool worldTagsAvailable()
{
    return g_tagsReady.load(std::memory_order_acquire) && g_installed.load(std::memory_order_acquire)
        && !g_tagsBroken.load(std::memory_order_acquire) && !g_teardown.load(std::memory_order_acquire);
}

void setDebugLines(LineSource source, std::vector<DebugLine> lines)
{
    std::lock_guard lock(g_debugMutex);
    g_sourceLines[static_cast<int>(source)] = std::move(lines);
    lines.clear();
    for (const auto& kept : g_sourceLines) lines.insert(lines.end(), kept.begin(), kept.end());
    std::stable_sort(lines.begin(), lines.end(), [](const DebugLine& a, const DebugLine& b) {
        if (a.onTop != b.onTop) return !a.onTop;
        if (a.width != b.width) return a.width < b.width;
        return std::lexicographical_compare(a.rgba, a.rgba + 4, b.rgba, b.rgba + 4);
    });
    g_debugLines = std::move(lines);
}

void setDebugFills(LineSource source, std::vector<DebugFill> fills)
{
    std::lock_guard lock(g_debugMutex);
    g_sourceFills[static_cast<int>(source)] = std::move(fills);
    fills.clear();
    for (const auto& kept : g_sourceFills) fills.insert(fills.end(), kept.begin(), kept.end());
    std::stable_sort(fills.begin(), fills.end(), [](const DebugFill& a, const DebugFill& b) {
        if (a.blend != b.blend) return !a.blend;
        return std::lexicographical_compare(a.rgba, a.rgba + 4, b.rgba, b.rgba + 4);
    });
    g_debugFills = std::move(fills);
}

bool installHooks()
{
    const Scanner& scanner = Scanner::instance();
    void* const stage = scanner.address(Target::NameTagStage);
    g_begin = scanner.addressAs<TessBeginFn>(Target::TessellatorBegin);
    g_vertex = scanner.addressAs<TessVertexFn>(Target::TessellatorVertex);
    g_color = scanner.addressAs<TessColorFn>(Target::TessellatorColor);
    g_render = scanner.addressAs<RenderMeshFn>(Target::RenderMeshImmediately);
    resolveWorldTags(scanner);
    meshkit::resolve();
    ghostmesh::resolve();
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
    startWorker();
    return true;
}

static bool active(bool xray)
{
    if (!g_installed.load(std::memory_order_acquire) || g_teardown.load(std::memory_order_acquire)) {
        return false;
    }
    const int want = xray ? kDefaultXray
                          : kDefaultFlat;
    return !g_modeBroken[want & 7].load(std::memory_order_relaxed);
}

void report()
{
    ghostmesh::report();
    static bool toldTags = false, toldTagMaterial = false, toldNoTags = false;
    if (!toldTags && g_tagsBroken.load()) {
        toldTags = true; log().warn(L"WorldMesh: DrawOne raised an exception; world tags stopped");
    }
    if (!toldTagMaterial && g_tagsNoMaterial.load()) {
        toldTagMaterial = true; log().warn(L"WorldMesh: world tag materials unavailable");
    }
    const auto wantedAt = g_tagsWantedAt.load();
    if (!toldNoTags && wantedAt && GetTickCount64() - wantedAt >= 10000 && g_tagsDrawn.load() == 0) {
        toldNoTags = true;
        log().warn(L"WorldMesh: no world tag was drawn ({} skipped)", g_tagsSkipped.load());
        notice::failOnce("WorldMesh.noTags", L"Waypoints: cannot draw the world tags; see the log.",
            "Waypoints could not draw world tags. See the log.");
    }
    static bool warnedDebugMaterial = false;
    if (!warnedDebugMaterial && g_debugNoMaterial.load(std::memory_order_relaxed) != 0) {
        warnedDebugMaterial = true;
        log().warn(L"WorldMesh: debug line material unavailable ({} attempts)",
                   g_debugNoMaterial.load(std::memory_order_relaxed));
    }
    if (!g_installed.load(std::memory_order_acquire)) {
        return;
    }
    static unsigned long long last = 0;
    static unsigned long long lastCalls = 0;
    static unsigned checks = 0;
    const unsigned long long now = GetTickCount64();
    if (last == 0) {
        last = now;
        return;
    }
    const unsigned long long interval = checks < 3 ? 10000ULL : 300000ULL;
    if (now - last < interval) {
        return;
    }
    const unsigned long long seconds = (now - last) / 1000ULL;
    last = now;
    const unsigned long long totalCalls = g_calls.load(std::memory_order_relaxed);
    const unsigned long long calls = totalCalls - lastCalls;
    lastCalls = totalCalls;
    const unsigned long long drawn = g_drawn.exchange(0, std::memory_order_relaxed);
    const unsigned long long noMaterial = g_noMaterial.exchange(0, std::memory_order_relaxed);
    const unsigned long long camShifted = g_camShifted.exchange(0, std::memory_order_relaxed);
    const unsigned long long badCtx = g_badCtx.exchange(0, std::memory_order_relaxed);
    if (const unsigned long long noLine = g_noLineMaterial.exchange(0, std::memory_order_relaxed); noLine != 0) {
        static bool toldLine = false;
        if (!toldLine) {
            toldLine = true;
            log().warn(L"WorldMesh: the outline material could not be resolved, so color boxes "
                       L"are drawn without outlines ({} frame(s))", noLine);
        }
    }
    if (drawn != 0 || noMaterial != 0 || camShifted != 0 || badCtx != 0 || boxes::boxesOn()) {
        if (checks < 3) {
            ++checks;
        }
    }
    if (!boxes::boxesOn()) {
        return;
    }
    const auto snap = schematicworld::renderSnapshot();
    if (!snap || std::none_of(snap->sections.begin(), snap->sections.end(),
        [](const auto& view) { return view.diff && view.diff->anyColor; })) return;
    const bool worldDrawn = GameData::instance().msSinceView() <= 2000ULL;
    if (totalCalls == 0 && worldDrawn) {
        notice::failOnce("WorldMesh.noStage",
                         L"WorldMesh: the name-tag stage was never called, so color boxes cannot be drawn",
                         "Schematica cannot draw color boxes: the name-tag stage was not called");
    }
    static unsigned toldMaterial = 0;
    const bool xray = boxes::boxXray();
    const int want = xray ? kDefaultXray : kDefaultFlat;
    if (!active(xray) && toldMaterial < 12) {
        ++toldMaterial;
        log().warn(L"WorldMesh: the color boxes cannot be drawn ({} material {} / "
                   L"resolved {} / stage calls {}); no other path draws them",
                   xray ? L"see-through (name tag)" : L"not see-through (selection overlay)", want,
                   (want > 0 && !g_modeBroken[want & 7].load(std::memory_order_relaxed)) ? L"yes" : L"no",
                   calls);
    }
    if (drawn == 0 && (calls != 0 || worldDrawn || badCtx != 0 || noMaterial != 0 || camShifted != 0)) {
        static unsigned toldStuck = 0;
        if (toldStuck < 12) {
            ++toldStuck;
            log().warn(L"WorldMesh: {} section(s) are waiting but nothing was drawn in {} seconds "
                       L"(stage calls {} / unreadable context {} / no material {} / shifted camera {})",
                       snap->sections.size(), seconds, calls, badCtx, noMaterial, camShifted);
        }
    }
}
void shutdown()
{
    setWorldTags({});
    ghostmesh::shutdown();
    {
        std::lock_guard lock(g_debugMutex);
        for (auto& lines : g_sourceLines) lines.clear();
        g_debugLines.clear();
        for (auto& fills : g_sourceFills) fills.clear();
        g_debugFills.clear();
    }
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
                                      : why == KeptWhy::Broken  ? L"after giving up on them"
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
