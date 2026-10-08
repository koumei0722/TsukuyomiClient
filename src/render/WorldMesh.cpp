#include "render/WorldMesh.h"

#include "core/Logger.h"
#include "core/Notice.h"
#include "core/Strings.h"
#include "game/BlockRegistry.h"
#include "game/GameData.h"
#include "hooks/HookManager.h"
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
std::atomic<bool> g_linesBroken{false};
std::atomic<bool> g_debugRibbonsBroken{false};
std::mutex g_debugMutex;
std::vector<DebugLine> g_debugLines;
std::atomic<unsigned> g_debugNoMaterial{0};
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
    {5, "name_tag_depth_tested", 0, false},
    {6, "selection_box", 0x10f0, false},
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
    return g_meshEnd != nullptr && g_tessClear != nullptr && g_rendererFlag != nullptr
           && g_meshRender != nullptr && g_meshDestroy != nullptr
           && !g_keptBroken.load(std::memory_order_relaxed);
}

constexpr double kRibbonPixels = 1.5;
constexpr double kRibbonNearW = 0.05;

struct MeshRequest {
    unsigned long long version = ~0ULL;
    bool xray = false;
    std::shared_ptr<const std::vector<blocks::DiffBox>> list;
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

struct Regions {
    unsigned long long version = ~0ULL;
    bool xray = false;
    boxmesh::RegionSet set;
};
Regions g_regions;

void buildRegions(const MeshRequest& request, Regions& state, Prepared& out)
{
    out.xray = request.xray;
    out.retained = true;
    if (state.version != request.version || state.xray != request.xray) {
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
    }
    out.regions.clear();
    out.regions.reserve(state.set.entries().size());
    for (const auto& [key, entry] : state.set.entries()) {
        out.regions.push_back(RegionOut{key, entry.generation, entry.geometry});
    }
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
                       request.list ? request.list->size() : 0);
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
        if (!buildPreparedSafely(request, g_regions, made)) {
            made = emptyPrepared(request);
        }
        {
            std::lock_guard<std::mutex> lock(g_workLock);
            g_result = std::move(made);
            g_resultReady = true;
        }
    }
}

void prepare(bool xray)
{
    const unsigned long long version = boxes::boxVersion();
    if (!g_workerRunning.load(std::memory_order_acquire)) {
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
    if (version != g_requestedVersion || xray != g_requestedXray) {
        g_requestedVersion = version;
        g_requestedXray = xray;
        {
            std::lock_guard<std::mutex> lock(g_workLock);
            g_work.version = version;
            g_work.xray = xray;
            g_work.list = boxes::boxSnapshot();
            g_workHas = true;
        }
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

void resetTessellator(void* tess);

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

struct TessSaved {
    unsigned char offsetScale[24];
    unsigned char useMatrix;
    unsigned char noColor;
    float color[4];
    unsigned char colorDirty;
    bool haveColor;
};

bool saveState(void* ctx, void* tess, TessSaved& saved);
void restoreState(void* ctx, void* tess, const TessSaved& saved);

void drawDebugLines(void* lrp, void* ctx)
{
    std::vector<DebugLine> lines;
    {
        std::lock_guard lock(g_debugMutex);
        lines = g_debugLines;
    }
    if (lines.empty() || lrp == nullptr || ctx == nullptr) return;
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
    for (std::size_t at = 0; at < lines.size();) {
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
        notice::failOnce(ribbons ? "WorldMesh.debugRibbonFailed" : "WorldMesh.lineFailed",
                         std::format(L"WorldMesh: {} failed (exception {:08X}); this drawing path is disabled",
                                     ribbons ? L"debug ribbons" : L"lines", job.code),
                         "Debug lines: drawing failed inside the game; the affected drawing path is disabled");
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

struct alignas(16) MeshStorage {
    unsigned char bytes[0x400];
};
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

struct MeshName {
    const char* text;
    std::size_t length;
};
MeshName g_meshName{nullptr, 0};

alignas(16) float g_sortMatrix[16] = {};
bool pageChecked(const void* address, std::size_t size, bool write)
{
    struct Entry {
        std::uintptr_t page = 0;
        unsigned long long at = 0;
        bool write = false;
    };
    constexpr std::size_t kEntries = 8;
    constexpr unsigned long long kKeepMs = 1000;
    thread_local Entry entries[kEntries];
    thread_local std::size_t next = 0;
    if (address == nullptr || size == 0) {
        return false;
    }
    const auto first = reinterpret_cast<std::uintptr_t>(address) & ~std::uintptr_t{0xFFF};
    const auto last = (reinterpret_cast<std::uintptr_t>(address) + size - 1) & ~std::uintptr_t{0xFFF};
    const unsigned long long now = GetTickCount64();
    const auto known = [&](std::uintptr_t page) {
        for (const Entry& e : entries) {
            if (e.page == page && now - e.at < kKeepMs && (e.write || !write)) {
                return true;
            }
        }
        return false;
    };
    if (known(first) && known(last)) {
        return true;
    }
    const bool ok = write ? memory::isWritable(address, size) : memory::isReadable(address, size);
    if (ok) {
        for (const std::uintptr_t page : {first, last}) {
            if (!known(page)) {
                entries[next] = Entry{page, now, write};
                next = (next + 1) % kEntries;
            }
        }
    }
    return ok;
}

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
        if (m.top == nullptr || !pageChecked(m.top, sizeof(float) * 16, true)
            || (m.pushed != nullptr && !pageChecked(m.pushed, sizeof(float) * 16, true))) {
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
            if (pageChecked(entry, sizeof(std::uint64_t), true)) {
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
            if (m.slot != nullptr && m.slot != m.top && pageChecked(m.slot, sizeof(float) * 16, true)) {
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

__declspec(noinline) void resetTessellator(void* tess)
{
    __try {
        auto* const t = static_cast<unsigned char*>(tess);
        t[kTessBuilding] = 0;
        if (g_tessClear != nullptr) {
            t[kTessVoid] = 0;
            g_tessClear(t + 8);
        }
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
            drawKeptFrame(lrp, ctx, tess, cam, limit, keptXray, flatMaterial, xrayMaterial,
                          FreeCamera::instance().borrowing());
        } else {
            g_noMaterial.fetch_add(1, std::memory_order_relaxed);
        }
        return;
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
    drawBoxes(lrp, ctx);
    drawDebugLines(lrp, ctx);
    g_inside.store(false, std::memory_order_release);
}

}

void setDebugLines(std::vector<DebugLine> lines)
{
    std::stable_sort(lines.begin(), lines.end(), [](const DebugLine& a, const DebugLine& b) {
        if (a.onTop != b.onTop) return !a.onTop;
        if (a.width != b.width) return a.width < b.width;
        return std::lexicographical_compare(a.rgba, a.rgba + 4, b.rgba, b.rgba + 4);
    });
    std::lock_guard lock(g_debugMutex);
    g_debugLines = std::move(lines);
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
            notice::failOnce("WorldMesh.kept",
                             std::format(L"WorldMesh: kept meshes are not available (signatures {}/{}/{}/{} / match "
                                         L"the immediate path {} / renderer flag {} / mesh name {}); color boxes are "
                                         L"not drawn",
                                         end != nullptr, clear != nullptr, meshRender != nullptr,
                                         meshDestroy != nullptr, callsMatch, flagOk, nameOk),
                             "Color boxes are not drawn: the game functions for drawing them were not found");
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
    const auto list = boxes::boxSnapshot();
    if (!list || list->empty()) {
        return;
    }
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
            log().warn(L"WorldMesh: {} box(es) are waiting but nothing was drawn in {} seconds "
                       L"(stage calls {} / unreadable context {} / no material {} / shifted camera {})",
                       list->size(), seconds, calls, badCtx, noMaterial, camShifted);
        }
    }
}
void shutdown()
{
    setDebugLines({});
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
