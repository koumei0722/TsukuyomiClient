#include "render/MeshKit.h"
#include "core/Logger.h"
#include "core/Notice.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "memory/Signatures.h"
#include "render/BoxMesher.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>

namespace tsukuyomi::meshkit {
using namespace tesslayout;
constexpr auto kTessOffset = kOffset, kTessUseMatrix = kUseMatrix, kTessNoColor = kNoColor;
constexpr auto kTessBuilding = kBuilding, kTessVoid = kVoid;
MaterialCtorFn g_matCtor = nullptr;
void* g_matGroup = nullptr;
void* g_terrainMatGroup = nullptr;
TessEndFn g_meshEnd = nullptr;
TessClearFn g_tessClear = nullptr;
RendererFlagFn g_rendererFlag = nullptr;
MeshRenderFn g_meshRender = nullptr;
MeshDestroyFn g_meshDestroy = nullptr;
std::uint64_t fnv1(std::string_view text)
{
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (const char c : text) {
        h *= 0x100000001B3ULL;
        h ^= static_cast<unsigned char>(c);
    }
    return h;
}

__declspec(noinline) bool callMaterialCtor(MaterialPtr* out, void* group, const void* name)
{
    __try {
        g_matCtor(out, group, name);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool infoMatches(const void* info, std::uint64_t hash)
{
    std::uint64_t got = 0;
    return info != nullptr && memory::copyGuarded(static_cast<const unsigned char*>(info) + 0x10, &got, sizeof(got))
           && got == hash;
}

void makeHashed(std::string_view name, char* longName, std::size_t longCapacity, HashedStringBuf& out)
{
    std::memset(&out, 0, sizeof(out));
    out.hash = fnv1(name);
    out.size = name.size();
    if (name.size() <= 15) {
        std::memcpy(out.text, name.data(), name.size());
        out.capacity = 15;
    } else {
        const std::size_t n = (std::min)(name.size(), longCapacity - 1);
        std::memcpy(longName, name.data(), n);
        longName[n] = 0;
        char* const at = longName;
        std::memcpy(out.text, &at, sizeof(at));
        out.size = n;
        out.capacity = longCapacity - 1;
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

__declspec(noinline) void destroyMesh(void* mesh)
{
    __try {
        g_meshDestroy(mesh);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

__declspec(noinline) bool renderMesh(void* ctx, const void* material, TopMatrix& m, void* mesh,
                                    const float rgba[4], const float position[3], const float* sortPos,
                                    const unsigned char* atlasTexture, bool singleBlock)
{
    auto* const sc = static_cast<unsigned char*>(ctx);
    unsigned char savedSingleBlock = 0;
    bool haveSingleBlock = false;
    __try {
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
        if (sortPos != nullptr && m.sortControl) {
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
        alignas(16) unsigned char params[kRenderParamsBytes] = {};
        alignas(16) unsigned char optional[0x50] = {};
        alignas(16) unsigned char texture[0x40] = {};
        if (atlasTexture != nullptr) {
            std::memcpy(params, atlasTexture, kRenderParamVariant);
            const std::size_t count = 1;
            std::memcpy(params + kRenderParamCount, &count, sizeof(count));
        }
        if (singleBlock) {
            savedSingleBlock = sc[kCtxSingleBlock];
            haveSingleBlock = true;
            sc[kCtxSingleBlock] = 1;
        }
        g_meshRender(mesh, static_cast<unsigned char*>(ctx) + 0x10, material, params, nullptr, nullptr,
                     texture, nullptr, optional);
        return true;
      } __finally {
        if (haveSingleBlock) sc[kCtxSingleBlock] = savedSingleBlock;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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

bool ready()
{
    return g_meshEnd != nullptr && g_tessClear != nullptr && g_rendererFlag != nullptr
           && g_meshRender != nullptr && g_meshDestroy != nullptr;
}

bool resolve()
{
    const Scanner& scanner = Scanner::instance();
    const auto g_render = scanner.address(Target::RenderMeshImmediately);
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
                         && meshRender != nullptr && callAt(0xbc) == static_cast<void*>(meshRender)
                         && meshDestroy != nullptr && callAt(0xc9) == static_cast<void*>(meshDestroy)
                         && clear != nullptr && callAt(0xf1) == static_cast<void*>(clear);
            static constexpr unsigned char kLea[] = {0x48, 0x8D, 0x0D};
            static constexpr unsigned char kMov[] = {0x48, 0xC7, 0x85, 0xC0, 0x03, 0x00, 0x00};
            if (std::memcmp(immediate + 0x42, kLea, sizeof(kLea)) == 0
                && std::memcmp(immediate + 0x50, kMov, sizeof(kMov)) == 0) {
                nameText = static_cast<const char*>(memory::ripTarget(immediate, 0x45));
                std::uint32_t length = 0;
                std::memcpy(&length, immediate + 0x57, sizeof(length));
                nameLength = length;
            }
        }
        const bool flagOk = flag != nullptr && memory::inGameModule(flag) && memory::isExecutable(flag, 1);
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
        void* const group = memory::ripTarget(site, 0x14);
        void* const ctor = memory::ripTarget(site, 0x23);
        if (group != nullptr && memory::isReadable(group, 8) && ctor != nullptr
            && memory::inGameModule(ctor) && memory::isExecutable(ctor, 1)) {
            g_matGroup = group;
            g_matCtor = reinterpret_cast<MaterialCtorFn>(ctor);
        }
    }
    if (const auto* site = scanner.address(Target::TerrainMaterialRegistrySite)) {
        void* const group = memory::ripTarget(site, 0x14);
        if (group != nullptr && memory::inGameModule(group) && memory::isReadable(group, 8)) {
            g_terrainMatGroup = group;
        }
    }
    return ready();
}

void* terrainMaterialGroup() { return g_terrainMatGroup; }

bool makeMaterial(void* group, std::string_view name, Material& out)
{
    if (group == nullptr || g_matCtor == nullptr) return false;
    std::unique_ptr<char[]> longName;
    if (name.size() > 15) longName = std::make_unique<char[]>(name.size() + 1);
    alignas(16) HashedStringBuf buf{};
    makeHashed(name, longName.get(), name.size() + 1, buf);
    (void)longName.release();
    const bool called = callMaterialCtor(&out.ptr, group, &buf);
    void* body = nullptr;
    out.ok = called && infoMatches(out.ptr.info, fnv1(name))
             && memory::copyGuarded(static_cast<const unsigned char*>(out.ptr.info) + kMaterialInfoBody,
                                    &body, sizeof(body)) && body != nullptr;
    return out.ok;
}
}
