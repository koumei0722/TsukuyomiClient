#include "render/WorldMesh.h"

#include "core/Logger.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "game/BlockRegistry.h"
#include "hooks/Detours.h"
#include "hooks/HookManager.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "memory/Signatures.h"
#include "modules/Zoom.h"
#include "render/BoxMesher.h"
#include "render/BoxRenderer.h"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
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
};
constexpr int kDefaultFlat = 1;
constexpr int kDefaultXray = 2;

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

std::atomic<int> g_modeFlat{kDefaultFlat};
std::atomic<int> g_modeXray{kDefaultXray};
std::atomic<bool> g_modeBroken[8]{};
std::atomic<bool> g_vertexColor{false};
std::atomic<int> g_inflate{0};
constexpr int kDefaultMaxBoxes = 30000;
std::atomic<int> g_maxBoxes{kDefaultMaxBoxes};
std::atomic<bool> g_flip{false};

std::atomic<unsigned long long> g_calls{0};
std::atomic<unsigned long long> g_drawn{0};
std::atomic<unsigned long long> g_failed{0};
std::atomic<unsigned long long> g_capped{0};
std::atomic<unsigned long long> g_noMaterial{0};
std::atomic<unsigned long long> g_noQuads{0};
std::atomic<unsigned long long> g_badCtx{0};
std::atomic<unsigned long long> g_camShifted{0};
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

    void* const info = hooks::materialInfoByName(name);
    if (info == nullptr || !infoMatches(info, hash) || !memory::isReadable(info, 0x10)) {
        log().warn(L"WorldMesh: material {} could not be resolved (neither the constructor "
                   L"site nor the registry)", toUtf16(name));
        return false;
    }
    void* ctrl = nullptr;
    std::memcpy(&ctrl, static_cast<const unsigned char*>(info) + 8, sizeof(ctrl));
    if (ctrl != nullptr && memory::isWritable(static_cast<unsigned char*>(ctrl) + 8, 4)) {
        _InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<unsigned char*>(ctrl) + 8));
    } else {
        ctrl = nullptr;
    }
    named.ptr = MaterialPtr{info, ctrl};
    named.ok = true;
    return true;
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

struct Prepared {
    unsigned long long version = ~0ULL;
    bool xray = false;
    std::vector<boxmesh::Quad> quads;
    std::size_t begin[4] = {};
    std::size_t end[4] = {};
    std::size_t cells = 0;
};
Prepared g_prepared;

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

void buildPrepared(const MeshRequest& request, Prepared& out)
{
    out.version = request.version;
    out.xray = request.xray;
    out.quads.clear();
    for (std::size_t c = 0; c < 4; ++c) {
        out.begin[c] = 0;
        out.end[c] = 0;
    }
    out.cells = 0;
    if (!request.list || request.list->empty()) {
        return;
    }
    std::vector<boxmesh::Cell> cells;
    cells.reserve(request.list->size());
    for (const blocks::DiffBox& box : *request.list) {
        const auto color = static_cast<std::uint8_t>(box.color);
        if (color == 0 || color > 3) {
            continue;
        }
        cells.push_back(boxmesh::Cell{box.x, box.y, box.z, color});
    }
    out.cells = cells.size();
    out.quads = boxmesh::build(cells, !request.xray);
    for (std::size_t i = 0; i < out.quads.size(); ++i) {
        const std::size_t c = out.quads[i].color & 3U;
        if (out.end[c] == 0) {
            out.begin[c] = i;
        }
        out.end[c] = i + 1;
    }
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
        buildPrepared(request, made);
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
        if (version != g_prepared.version || xray != g_prepared.xray) {
            MeshRequest request;
            request.version = version;
            request.xray = xray;
            request.list = boxes::boxSnapshot();
            buildPrepared(request, g_prepared);
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_workLock);
        if (g_resultReady) {
            g_prepared = std::move(g_result);
            g_resultReady = false;
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
        if (WaitForSingleObject(g_worker, 3000) != WAIT_OBJECT_0) {
            log().warn(L"WorldMesh: the merge thread did not stop within 3 seconds");
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
    float rgba[4];
    float inflate;
    bool vertexColor;
    bool flip;
    std::uint32_t limit;
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
            g_begin(tess, nullptr, 1, 0, false);
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
            for (; at < job.count; ++at) {
                if (used + 4U > job.limit && used != 0) {
                    break;
                }
                emitRect(job, job.quads[at]);
                used += 4U;
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

void drawBoxes(void* lrp, void* ctx)
{
    if (!boxes::boxesOn()) {
        return;
    }
    const bool xray = boxes::boxXray();
    const int mode = xray ? g_modeXray.load(std::memory_order_relaxed)
                          : g_modeFlat.load(std::memory_order_relaxed);
    if (mode == 0) {
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

    prepare(xray);
    if (g_prepared.quads.empty()) {
        g_noQuads.fetch_add(1, std::memory_order_relaxed);
        return;
    }

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
            static std::atomic<unsigned> told{0};
            return;
        }
    }
    std::uint32_t limit = 0;
    std::memcpy(&limit, static_cast<unsigned char*>(tess) + kTessLimit, sizeof(limit));
    if (limit < 64 || limit > (1U << 24)) {
        limit = 65532;
    }
    limit &= ~3U;

    TessSaved saved{};
    if (!saveState(ctx, tess, saved)) {
        return;
    }

    const float faceAlpha = boxes::boxFaceAlpha();
    const float inflate = static_cast<float>(g_inflate.load(std::memory_order_relaxed)) / 1024.0F;
    const bool vertexColor = g_vertexColor.load(std::memory_order_relaxed);
    const bool flip = g_flip.load(std::memory_order_relaxed);
    std::size_t budget = static_cast<std::size_t>(
        (std::max)(1, g_maxBoxes.load(std::memory_order_relaxed)));
    std::uint32_t batches = 0;
    std::uint32_t faces = 0;
    bool busy = false;
    bool failed = false;
    DWORD code = 0;
    std::size_t capped = 0;
    for (std::size_t color = 1; color <= 3 && !failed && !busy; ++color) {
        const std::size_t first = g_prepared.begin[color];
        const std::size_t last = g_prepared.end[color];
        if (last <= first) {
            continue;
        }
        float rgb[3] = {};
        float alpha = faceAlpha;
        if (!boxes::boxStyle(static_cast<blocks::DiffColor>(color), rgb, &alpha)) {
            continue;
        }
        if (multiply) {
            const float k = std::clamp(alpha, 0.0F, 1.0F);
            for (float& one : rgb) {
                one = 0.5F + (one - 0.5F) * k;
            }
            alpha = 1.0F;
        }
        const std::size_t total = last - first;
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
        job.quads = g_prepared.quads.data() + first;
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
        faces += job.faces;
        busy = job.busy;
        failed = job.failed;
        code = job.code;
    }
    restoreState(ctx, tess, saved);

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
    if (g_inside.exchange(true, std::memory_order_acquire)) {
        return result;
    }
    drawBoxes(lrp, ctx);
    g_inside.store(false, std::memory_order_release);
    return result;
}

void __fastcall detourNameTagStage(void* lrp, void* ctx, void* view, void* extra)
{
    callOriginal(lrp, ctx, view, extra);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_teardown.load(std::memory_order_acquire)) {
        return;
    }
    if (g_inside.exchange(true, std::memory_order_acquire)) {
        return;
    }
    drawBoxes(lrp, ctx);
    g_inside.store(false, std::memory_order_release);
}

std::filesystem::file_time_type g_flagTime{};
bool g_flagSeen = false;
unsigned long long g_flagCheckedAt = 0;

void readFlag()
{
    const unsigned long long now = GetTickCount64();
    if (now - g_flagCheckedAt < 1000) {
        return;
    }
    g_flagCheckedAt = now;
    const std::filesystem::path file = paths::dataDir() / L"diag-worldmesh.txt";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        if (g_flagSeen) {
            g_flagSeen = false;
            g_modeFlat.store(kDefaultFlat, std::memory_order_relaxed);
            g_modeXray.store(kDefaultXray, std::memory_order_relaxed);
            g_vertexColor.store(false, std::memory_order_relaxed);
            g_inflate.store(0, std::memory_order_relaxed);
            g_maxBoxes.store(kDefaultMaxBoxes, std::memory_order_relaxed);
            g_flip.store(false, std::memory_order_relaxed);
        }
        return;
    }
    const auto time = std::filesystem::last_write_time(file, ec);
    if (g_flagSeen && time == g_flagTime) {
        return;
    }
    g_flagSeen = true;
    g_flagTime = time;
    std::string text;
    if (std::ifstream in{file}) {
        std::ostringstream all;
        all << in.rdbuf();
        text = all.str();
    }
    int flat = kDefaultFlat;
    int xray = kDefaultXray;
    bool vc = false;
    int inflate = 0;
    int maxBoxes = kDefaultMaxBoxes;
    bool flip = false;
    bool sawFlat = false;
    std::istringstream words(text);
    std::string word;
    while (words >> word) {
        try {
            if (word == "vc") {
                vc = true;
            } else if (word == "flip") {
                flip = true;
            } else if (word.rfind("inflate=", 0) == 0) {
                inflate = std::stoi(word.substr(8));
            } else if (word.rfind("max=", 0) == 0) {
                maxBoxes = std::stoi(word.substr(4));
            } else if (word[0] == 'x' && word.size() > 1) {
                xray = std::stoi(word.substr(1));
            } else if (!sawFlat && word[0] >= '0' && word[0] <= '9') {
                flat = std::stoi(word);
                sawFlat = true;
            }
        } catch (...) {
        }
    }
    if (choiceOf(flat) == nullptr) {
        flat = 0;
    }
    if (choiceOf(xray) == nullptr) {
        xray = 0;
    }
    g_modeFlat.store(flat, std::memory_order_relaxed);
    g_modeXray.store(xray, std::memory_order_relaxed);
    g_vertexColor.store(vc, std::memory_order_relaxed);
    g_inflate.store(inflate, std::memory_order_relaxed);
    g_maxBoxes.store(maxBoxes, std::memory_order_relaxed);
    g_flip.store(flip, std::memory_order_relaxed);
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
    return boxes::boxXray() ? g_modeXray.load(std::memory_order_relaxed)
                            : g_modeFlat.load(std::memory_order_relaxed);
}

bool active(bool xray)
{
    if (!g_installed.load(std::memory_order_acquire) || g_teardown.load(std::memory_order_acquire)) {
        return false;
    }
    const int want = xray ? g_modeXray.load(std::memory_order_relaxed)
                          : g_modeFlat.load(std::memory_order_relaxed);
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
    readFlag();
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
    const unsigned long long hostCalls = g_hostCalls.exchange(0, std::memory_order_relaxed);
    if (drawn == 0 && failed == 0 && noMaterial == 0 && camShifted == 0 && noQuads == 0
        && badCtx == 0 && !boxes::boxesOn()) {
        return;
    }
    if (reports >= 60) {
        return;
    }
    ++reports;
    if (boxes::boxesOn() && drawn == 0 && (calls == 0 || badCtx != 0)) {
        const auto list = boxes::boxSnapshot();
        const bool haveBoxes = list && !list->empty();
        static int idle = 0;
        if (haveBoxes && calls == 0 && hostCalls != 0
            && g_drawAt.load(std::memory_order_relaxed) == 0) {
            g_drawAt.store(1, std::memory_order_relaxed);
            idle = 0;
            log().warn(L"WorldMesh: the name-tag stage never ran in {} seconds (host calls "
                       L"{}). Drawing at the end of the host call instead",
                       seconds,
                       hostCalls);
            return;
        }
        if (haveBoxes) {
            ++idle;
            if (idle >= 2) {
                idle = 0;
                const int want = boxes::boxXray()
                                     ? g_modeXray.load(std::memory_order_relaxed)
                                     : g_modeFlat.load(std::memory_order_relaxed);
                if (want > 0 && !g_modeBroken[want & 7].exchange(true, std::memory_order_relaxed)) {
                    log().warn(L"WorldMesh: the immediate mesh drew no boxes (stage calls {} / "
                               L"boxes {} / no faces {} / unreadable context {} / no material "
                               L"{}). Giving up on material {} and falling back to the "
                               L"original drawing path",
                               calls,
                               list->size(),
                               noQuads,
                               badCtx,
                               noMaterial,
                               want);
                }
            }
        } else {
            idle = 0;
        }
    }
}

void shutdown()
{
    g_teardown.store(true, std::memory_order_release);
    for (int i = 0; i < 50 && g_inside.load(std::memory_order_acquire); ++i) {
        Sleep(10);
    }
    stopWorker();
}

}
