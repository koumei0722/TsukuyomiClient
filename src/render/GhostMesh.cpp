#include "render/GhostMesh.h"
#include "render/MeshKit.h"
#include "render/BoxRenderer.h"
#include "core/Logger.h"
#include "core/Notice.h"
#include "game/BlockRegistry.h"
#include "game/SchematicWorld.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "memory/Signatures.h"
#include "modules/Schematica.h"

#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace tsukuyomi::ghostmesh {
namespace {
using namespace tesslayout;
using GetTessFn = void*(__fastcall*)(void* owner);
using TessellateFn = bool(__fastcall*)(void* bt, void* tess, const void* block, const void* rec, bool chunk);
using LayerFn = std::uint8_t(__fastcall*)(const void* block);
using AirFn = void*(__fastcall*)(void* registry, void* out, void* name, std::int32_t aux, std::int32_t mode);
GetTessFn g_getTess = nullptr;
TessellateFn g_tessellate = nullptr;
LayerFn g_layer = nullptr;
AirFn g_airLookup = nullptr;
void* g_airA1 = nullptr;
void* g_airA2 = nullptr;
const void* g_cacheAir = nullptr;
std::int32_t g_airDefaultBlock = 0;
bool g_airTried = false;
std::atomic<bool> g_ready{false}, g_broken{false};
std::atomic<bool> g_releaseAsked{false}, g_released{false}, g_inside{false};
std::atomic<std::size_t> g_meshCount{0};
std::atomic<std::uint64_t> g_skipped{0}, g_badContext{0};
unsigned g_stallFrames = 0;
constexpr unsigned kGhostStallFrames = 600;
constexpr long long kGhostBuildBudgetUs = 3000;
constexpr int kOverlaySizeUnits = -8;

struct GhostSection {
    schematic::SectionKey key;
    std::uint64_t generation = 0;
    std::int32_t origin[3]{};
    std::unique_ptr<meshkit::MeshStorage> mesh;
    std::uint32_t cells = 0, skipped = 0;
    bool endStarted = false;
};
auto& g_kept = *new std::unordered_map<schematic::SectionKey, GhostSection, schematic::SectionKeyHash>;
std::shared_ptr<const schematic::RenderSnapshot> g_snap;
meshkit::Material g_material[2];
bool g_materialTried[2]{};
std::uint8_t g_builtAlpha = 0xFF;

void fail(const char* key, const wchar_t* text)
{
    g_broken.store(true, std::memory_order_relaxed);
    notice::failOnce(key, text, "Ghost blocks are not drawn (see the log for the failed game function)");
}

void destroySection(GhostSection& section)
{
    if (section.mesh != nullptr) meshkit::destroyMesh(section.mesh->bytes);
    section.mesh.reset();
}

void destroyAll()
{
    for (auto& [key, section] : g_kept) { (void)key; destroySection(section); }
    g_kept.clear();
    g_snap.reset();
    g_meshCount.store(0, std::memory_order_seq_cst);
    g_skipped.store(0, std::memory_order_relaxed);
    g_stallFrames = 0;
}

__declspec(noinline) const void* lookupAir()
{
    __try {
        void* result[2]{};
        g_airLookup(g_airA1, result, g_airA2, 0, 0);
        return result[0] != nullptr ? *reinterpret_cast<const void* const*>(static_cast<const std::byte*>(result[0]) + g_airDefaultBlock)
                                    : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

__declspec(noinline) bool contextOf(void* lrp, void* ctx, void*& tess, void*& bt, void*& owner, float cam[3])
{
    __try {
        if (!meshkit::pageChecked(ctx, kCtxTess + sizeof(void*), false)
            || !meshkit::pageChecked(static_cast<unsigned char*>(lrp) + kLrpTessOwner, sizeof(void*), false)
            || !meshkit::pageChecked(static_cast<unsigned char*>(lrp) + kLrpCamera, sizeof(float) * 3, false)) return false;
        std::memcpy(&tess, static_cast<unsigned char*>(ctx) + kCtxTess, sizeof(tess));
        std::memcpy(&owner, static_cast<unsigned char*>(lrp) + kLrpTessOwner, sizeof(owner));
        std::memcpy(cam, static_cast<unsigned char*>(lrp) + kLrpCamera, sizeof(float) * 3);
        if (!meshkit::pageChecked(tess, kReadable, true) || !meshkit::pageChecked(owner, kOwnerAtlas + kTextureBytes, false)) return false;
        bt = g_getTess(owner);
        return meshkit::pageChecked(bt, kBtWritable, true)
               && std::isfinite(cam[0]) && std::isfinite(cam[1]) && std::isfinite(cam[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

const void* blockFor(const schematic::SectionView& view, const schematic::RenderSnapshot& snap,
                     std::uint32_t slot, const std::int32_t pos[3])
{
    const auto color = static_cast<schematic::DiffColor>(view.diff->color[slot]);
    if (color != schematic::DiffColor::Missing
        && !(snap.ghostOverMismatch && (color == schematic::DiffColor::Wrong || color == schematic::DiffColor::State))) return nullptr;
    if (!snap.layer.shows(pos[0], pos[1], pos[2])) return nullptr;
    const auto entry = view.want->want[slot];
    return entry != 0 && entry < schematic::kWantOutside && entry <= snap.palette->size()
               ? (*snap.palette)[entry - 1] : nullptr;
}

void slotPosition(std::uint32_t slot, const std::int32_t origin[3], std::int32_t pos[3])
{
    pos[0] = origin[0] + static_cast<int>(slot / 256);
    pos[1] = origin[1] + static_cast<int>((slot / 16) & 15);
    pos[2] = origin[2] + static_cast<int>(slot & 15);
}

void finishTess(unsigned char* t)
{
    const std::uint32_t zero32 = 0;
    const std::uint64_t zero64 = 0;
    std::memcpy(t + kResetWord, &zero32, sizeof(zero32));
    t[kVoid] = 0;
    t[kBuilding] = 0;
    t[kResetByte] = 0;
    meshkit::g_tessClear(t + kBuffers);
    t[kFormat] = 0;
    std::memcpy(t + kResetPointer, &zero64, sizeof(zero64));
    for (const auto at : kResetBytes) t[at] = 0;
}

void beginTess(unsigned char* t)
{
    finishTess(t);
    t[kMode] = 1;
    t[kNoColor] = 0;
    const std::uint16_t one16 = 1;
    const std::uint32_t zero32 = 0;
    std::memcpy(t + kBuilding, &one16, sizeof(one16));
    std::memcpy(t + kBeginWord, &zero32, sizeof(zero32));
    std::memcpy(t + kBeginCopyTo, t + kBeginCopyFrom, sizeof(std::uint64_t));
    t[kBeginByte] = 1;
    t[kVoid] = 1;
    if (t[kNoColor] == 0) {
        t[kBeginColorFlag] = 1;
        const std::uint32_t white = ~std::uint32_t{0};
        std::memcpy(t + kBeginDefaultColor, &white, sizeof(white));
        if (t[kFormat] == 0) t[kBeginFormatFlag] = 1;
    }
}

std::size_t tripleLength(const unsigned char* t)
{
    std::uintptr_t begin = 0, end = 0;
    std::memcpy(&begin, t + kPosBegin, sizeof(begin));
    std::memcpy(&end, t + kPosBegin + sizeof(void*), sizeof(end));
    return begin != 0 && end >= begin ? (end - begin) / (sizeof(float) * 3) : 0;
}

void shrinkVertices(unsigned char* t, std::size_t first, const std::int32_t pos[3], const std::int32_t origin[3])
{
    float* positions = nullptr;
    std::memcpy(&positions, t + kPosBegin, sizeof(positions));
    const std::size_t end = tripleLength(t);
    constexpr float scale = 1.0F + static_cast<float>(kOverlaySizeUnits) / 512.0F;
    for (std::size_t i = first; i < end; ++i) {
        for (int k = 0; k < 3; ++k) {
            const float center = static_cast<float>(pos[k] - origin[k]) + 0.5F;
            positions[i * 3 + k] = center + (positions[i * 3 + k] - center) * scale;
        }
    }
}

struct BlockRecord { std::int32_t x, y, z, pad; const void* block; };
static_assert(sizeof(BlockRecord) == 0x18);

void tessellateBlock(unsigned char* bt, unsigned char* tess, const void* block, const std::int32_t pos[3])
{
    const std::uint32_t layer = g_layer(block);
    std::memcpy(bt + kBtLayer, &layer, sizeof(layer));
    const BlockRecord rec{pos[0], pos[1], pos[2], 0, block};
    g_tessellate(bt, tess, block, &rec, false);
}

__declspec(noinline) bool buildSection(void* bt, void* tess, const schematic::SectionView& view,
                                     const schematic::RenderSnapshot& snap, GhostSection& out, bool& busy,
                                     std::uint8_t alpha)
{
    bool entered = false;
    bool began = false;
    __try {
        auto* const t = static_cast<unsigned char*>(tess);
        auto* const b = static_cast<unsigned char*>(bt);
        if (t[kBuilding] != 0 || t[kVoid] != 0) { busy = true; return false; }
        if (view.diff == nullptr || view.want == nullptr || snap.palette == nullptr) return true;
        for (std::uint32_t i = 0; i < 4096; ++i) {
            std::int32_t pos[3]; slotPosition(i, out.origin, pos);
            const void* const block = blockFor(view, snap, i, pos);
            if (block == nullptr) continue;
            if (static_cast<const unsigned char*>(block)[kBlockClientPipeline] == 1) { ++out.skipped; continue; }
            ++out.cells;
        }
        if (out.cells == 0) return true;
        const schematicworld::BuildWindow window{&snap};
        schematicworld::enterBuildWindow(&window);
        entered = true;
        began = true;
        beginTess(t);
        std::memcpy(b + kBtCacheRegion, b + kBtRegion, sizeof(void*));
        const std::int32_t cacheOrigin[3] = {out.origin[0] - 2, out.origin[1] - 2, out.origin[2] - 2};
        std::memcpy(b + kBtCacheOrigin, cacheOrigin, sizeof(cacheOrigin));
        std::memset(b + kBtCacheFlags, 0xff, kBtCacheFlagsBytes);
        for (std::size_t i = 0; i < kBtCacheBlockBytes / sizeof(void*); ++i)
            std::memcpy(b + kBtCacheBlocks + i * sizeof(void*), &g_cacheAir, sizeof(g_cacheAir));
        std::memset(b + kBtCacheTail, 0, kBtCacheBlockBytes);
        const float offset[3] = {-static_cast<float>(out.origin[0]), -static_cast<float>(out.origin[1]), -static_cast<float>(out.origin[2])};
        const float scale[3] = {1, 1, 1};
        std::memcpy(t + kOffset, offset, sizeof(offset));
        std::memcpy(t + kScale, scale, sizeof(scale));
        t[kUseMatrix] = 0;
        for (std::uint32_t i = 0; i < 4096; ++i) {
            std::int32_t pos[3]; slotPosition(i, out.origin, pos);
            const void* const block = blockFor(view, snap, i, pos);
            if (block == nullptr || static_cast<const unsigned char*>(block)[kBlockClientPipeline] == 1) continue;
            const std::size_t first = tripleLength(t);
            tessellateBlock(b, t, block, pos);
            const auto water = view.want->want2[i];
            if (water != 0 && water < schematic::kWantOutside && water <= snap.palette->size()) {
                const void* const waterBlock = (*snap.palette)[water - 1];
                if (waterBlock != nullptr) {
                    if (static_cast<const unsigned char*>(waterBlock)[kBlockClientPipeline] == 1) ++out.skipped;
                    else tessellateBlock(b, t, waterBlock, pos);
                }
            }
            const auto color = static_cast<schematic::DiffColor>(view.diff->color[i]);
            if (color == schematic::DiffColor::Wrong || color == schematic::DiffColor::State)
                shrinkVertices(t, first, pos, out.origin);
        }
        const float zero[3]{};
        std::memcpy(t + kOffset, zero, sizeof(zero));
        if (alpha != 0xFF) {
            unsigned char* colors = nullptr;
            unsigned char* colorsEnd = nullptr;
            std::memcpy(&colors, t + kColorBegin, sizeof(colors));
            std::memcpy(&colorsEnd, t + kColorBegin + sizeof(void*), sizeof(colorsEnd));
            for (unsigned char* c = colors; c != nullptr && c + 3 < colorsEnd; c += 4) c[3] = alpha;
        }
        t[kVoid] = 0;
        out.endStarted = true;
        meshkit::g_meshEnd(tess, out.mesh->bytes, 0, &meshkit::g_meshName, 2);
        finishTess(t);
        schematicworld::leaveBuildWindow();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (began) meshkit::resetTessellator(tess);
        if (entered) schematicworld::leaveBuildWindow();
        return false;
    }
}

__declspec(noinline) bool copyTexture(void* owner, unsigned char texture[kRenderParamVariant])
{
    __try {
        std::memcpy(texture, static_cast<unsigned char*>(owner) + kOwnerAtlas, kTextureBytes);
        void* controls[2]{};
        std::memcpy(&controls[0], texture + sizeof(void*), sizeof(void*));
        std::memcpy(&controls[1], texture + sizeof(void*) * 3, sizeof(void*));
        for (void* ctrl : controls) {
            if (ctrl != nullptr && !meshkit::pageChecked(static_cast<unsigned char*>(ctrl) + kCtrlStrongCount, sizeof(long), true)) return false;
        }
        for (void* ctrl : controls) {
            if (ctrl != nullptr) _InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<unsigned char*>(ctrl) + kCtrlStrongCount));
        }
        texture[kRenderParamTag] = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void drawBody(void* lrp, void* ctx)
{
    const auto snap = schematicworld::renderSnapshot();
    if (!Schematica::instance().wantsGhostHooks() || snap == nullptr || snap->sections.empty()) { destroyAll(); return; }
    if (g_broken.load(std::memory_order_relaxed)) { destroyAll(); return; }
    if (!meshkit::ready() || !g_ready.load(std::memory_order_acquire)) return;
    void* tess = nullptr; void* bt = nullptr; void* owner = nullptr;
    float camF[3]{};
    if (!contextOf(lrp, ctx, tess, bt, owner, camF)) { g_badContext.fetch_add(1, std::memory_order_relaxed); return; }
    g_cacheAir = lookupAir();
    if (!g_airTried) {
        g_airTried = true;
        log().info(L"GhostMesh: terrain registry {:#x}; cache air {:#x}; registry air {:#x}",
                   reinterpret_cast<std::uintptr_t>(meshkit::terrainMaterialGroup()),
                   reinterpret_cast<std::uintptr_t>(g_cacheAir), reinterpret_cast<std::uintptr_t>(blocks::airBlock()));
    }
    if (g_cacheAir == nullptr) { fail("GhostMesh.resolve", L"GhostMesh: the game's cache air lookup failed"); return; }
    if (g_snap == nullptr || g_snap->serial != snap->serial) {
        if (g_snap != nullptr
            && (g_snap->palette != snap->palette || g_snap->air != snap->air
                || g_snap->ghostOverMismatch != snap->ghostOverMismatch || g_snap->layer.on != snap->layer.on
                || g_snap->layer.axis != snap->layer.axis || g_snap->layer.lo != snap->layer.lo || g_snap->layer.hi != snap->layer.hi)) {
            destroyAll();
        }
        for (auto it = g_kept.begin(); it != g_kept.end();) {
            if (!snap->at.contains(it->first)) { destroySection(it->second); it = g_kept.erase(it); }
            else {
                if (g_snap != nullptr) {
                    const auto old = g_snap->at.find(it->first);
                    const auto& next = snap->sections[snap->at.at(it->first)];
                    if (old == g_snap->at.end() || g_snap->sections[old->second].want != next.want)
                        it->second.generation = ~next.generation;
                }
                ++it;
            }
        }
        g_snap = snap;
    }
    const int opacity = Schematica::instance().ghostOpacity();
    const int matIndex = opacity >= 100 ? 0 : 1;
    const auto alpha = static_cast<std::uint8_t>(std::clamp(opacity, 0, 100) * 255 / 100);
    if (alpha != g_builtAlpha) {
        destroyAll();
        g_builtAlpha = alpha;
    }
    if (!g_materialTried[matIndex]) {
        g_materialTried[matIndex] = true;
        if (!meshkit::makeMaterial(meshkit::terrainMaterialGroup(), matIndex == 0 ? "moving_block" : "moving_block_blend", g_material[matIndex])) {
            fail("GhostMesh.material", L"GhostMesh: moving_block material is missing or has an empty body"); return;
        }
    }
    const double cam[3] = {camF[0], camF[1], camF[2]};
    std::vector<const schematic::SectionView*> pending;
    for (const auto& view : snap->sections) {
        const auto it = g_kept.find(view.key);
        if (it == g_kept.end() || it->second.generation != view.generation) pending.push_back(&view);
    }
    const auto distance = [&](const schematic::SectionView* view) {
        const std::int32_t origin[3] = {view->key.cx * 16, view->key.cy * 16, view->key.cz * 16};
        return meshkit::regionBoxDistance(origin, cam);
    };
    std::sort(pending.begin(), pending.end(), [&](const auto* a, const auto* b) { return distance(a) < distance(b); });
    meshkit::TessSaved saved{};
    if (!meshkit::saveState(ctx, tess, saved)) return;
    struct StateGuard {
        void* ctx; void* tess; const meshkit::TessSaved& saved;
        ~StateGuard() { meshkit::restoreState(ctx, tess, saved); }
    } stateGuard{ctx, tess, saved};
    const auto started = std::chrono::steady_clock::now();
    bool stalled = false;
    for (const auto* view : pending) {
        GhostSection fresh;
        fresh.key = view->key; fresh.generation = view->generation;
        fresh.origin[0] = view->key.cx * 16; fresh.origin[1] = view->key.cy * 16; fresh.origin[2] = view->key.cz * 16;
        fresh.mesh = std::make_unique<meshkit::MeshStorage>();
        auto [it, inserted] = g_kept.try_emplace(view->key);
        if (inserted) it->second.generation = ~view->generation;
        bool busy = false;
        if (!buildSection(bt, tess, *view, *snap, fresh, busy, g_builtAlpha)) {
            if (busy) { stalled = true; break; }
            if (fresh.endStarted) (void)fresh.mesh.release();
            fail("GhostMesh.build", L"GhostMesh: block tessellation or mesh end raised an exception");
            return;
        }
        if (fresh.cells == 0) fresh.mesh.reset();
        const bool hadMesh = it->second.mesh != nullptr;
        const bool hasMesh = fresh.mesh != nullptr;
        destroySection(it->second);
        it->second = std::move(fresh);
        if (hasMesh && !hadMesh) g_meshCount.fetch_add(1, std::memory_order_seq_cst);
        else if (hadMesh && !hasMesh) g_meshCount.fetch_sub(1, std::memory_order_seq_cst);
        if (std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count() >= kGhostBuildBudgetUs) break;
    }
    if (stalled) {
        if (++g_stallFrames >= kGhostStallFrames) { fail("GhostMesh.stall", L"GhostMesh: Tessellator stayed busy for 600 frames"); return; }
    } else g_stallFrames = 0;
    std::size_t meshes = 0;
    std::uint64_t skipped = 0;
    for (const auto& [key, section] : g_kept) { (void)key; meshes += section.mesh != nullptr ? 1 : 0; skipped += section.skipped; }
    g_meshCount.store(meshes, std::memory_order_seq_cst);
    g_skipped.store(skipped, std::memory_order_relaxed);
    float eye[3]{}, vp[16]{};
    bool haveVp = boxes::cameraSnapshot(eye, vp);
    if (haveVp) for (int k = 0; k < 3; ++k) if (std::fabs(eye[k] - camF[k]) > 0.5F) haveVp = false;
    meshkit::TopMatrix matrix{};
    if (!meshkit::findTopMatrix(ctx, matrix)) return;
    struct MatrixGuard { meshkit::TopMatrix& matrix; ~MatrixGuard() { meshkit::restoreTopMatrix(matrix); } } matrixGuard{matrix};
    const float rgba[4] = {1, 1, 1, 1};
    for (const auto& [key, section] : g_kept) {
        const auto at = snap->at.find(key);
        if (section.mesh == nullptr || at == snap->at.end() || section.generation != snap->sections[at->second].generation
            || !meshkit::regionVisible(section.origin, cam, vp, haveVp)) continue;
        unsigned char texture[kRenderParamVariant]{};
        if (!copyTexture(owner, texture)) { fail("GhostMesh.build", L"GhostMesh: terrain atlas control blocks are unreadable"); return; }
        const float position[3] = {static_cast<float>(section.origin[0]) - camF[0], static_cast<float>(section.origin[1]) - camF[1], static_cast<float>(section.origin[2]) - camF[2]};
        if (!meshkit::renderMesh(ctx, &g_material[matIndex].ptr, matrix, section.mesh->bytes, rgba, position, nullptr, texture, true)) {
            fail("GhostMesh.build", L"GhostMesh: Mesh::render raised an exception"); return;
        }
    }
}
}

bool resolve()
{
    const Scanner& scanner = Scanner::instance();
    void* const getTess = scanner.address(Target::GetBlockTessellator);
    void* const tessellate = scanner.address(Target::BlockTessellate);
    void* const layer = scanner.address(Target::BlockRenderLayerOf);
    const auto* const site = scanner.address(Target::AirBlockLookupSite);
    void* air = nullptr;
    if (site != nullptr && memory::isReadable(site, 0x70)) {
        g_airA1 = memory::ripTarget(site, 3); g_airA2 = memory::ripTarget(site, 10);
        air = memory::ripTarget(site, 0x16);
        std::memcpy(&g_airDefaultBlock, site + 0x6c, sizeof(g_airDefaultBlock));
    }
    for (const void* fn : {getTess, tessellate, layer, air}) {
        if (fn == nullptr || !memory::inGameModule(fn) || !memory::isExecutable(fn, 1)) {
            fail("GhostMesh.resolve", L"GhostMesh: block tessellation signatures could not be resolved"); return false;
        }
    }
    if (g_airA1 == nullptr || g_airA2 == nullptr || !memory::isReadable(g_airA1, 1) || !memory::isReadable(g_airA2, 1)
        || g_airDefaultBlock <= 0 || g_airDefaultBlock >= 0x1000 || g_airDefaultBlock % sizeof(void*) != 0) {
        fail("GhostMesh.resolve", L"GhostMesh: cache air lookup arguments are unreadable"); return false;
    }
    g_getTess = reinterpret_cast<GetTessFn>(getTess);
    g_tessellate = reinterpret_cast<TessellateFn>(tessellate);
    g_layer = reinterpret_cast<LayerFn>(layer);
    g_airLookup = reinterpret_cast<AirFn>(air);
    g_ready.store(true, std::memory_order_release);
    return true;
}

void releaseOnRenderThread()
{
    destroyAll();
    g_released.store(true, std::memory_order_release);
}

void drawFrame(void* lrp, void* ctx)
{
    if (g_inside.exchange(true, std::memory_order_seq_cst)) return;
    try {
        if (g_releaseAsked.load(std::memory_order_seq_cst)) releaseOnRenderThread();
        else drawBody(lrp, ctx);
    } catch (const std::bad_alloc&) {
        fail("GhostMesh.memory", L"GhostMesh: section mesh allocation failed");
    } catch (const std::exception&) {
        fail("GhostMesh.build", L"GhostMesh: section reconciliation failed");
    }
    g_inside.store(false, std::memory_order_seq_cst);
}

void shutdown()
{
    g_releaseAsked.store(true, std::memory_order_seq_cst);
    const bool pending = g_meshCount.load(std::memory_order_seq_cst) != 0 || g_inside.load(std::memory_order_seq_cst);
    for (int i = 0; pending && i < 100 && !g_released.load(std::memory_order_acquire); ++i) Sleep(10);
    if (pending && !g_released.load(std::memory_order_acquire))
        log().warn(L"GhostMesh: the render thread did not return; leaving kept meshes in place");
}

void report()
{
    static bool toldSkipped = false, toldContext = false;
    const auto skipped = g_skipped.load(std::memory_order_relaxed);
    if (!toldSkipped && skipped != 0) {
        toldSkipped = true;
        log().info(L"GhostMesh: {} cell(s) of the ClientBlockPipeline kinds are not drawn yet", skipped);
    }
    if (!toldContext && g_badContext.load(std::memory_order_relaxed) != 0) {
        toldContext = true;
        log().warn(L"GhostMesh: drawing context could not be read ({} attempts)", g_badContext.load(std::memory_order_relaxed));
    }
}
}
