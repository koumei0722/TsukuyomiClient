#pragma once

#include "render/TessellatorLayout.h"
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tsukuyomi::meshkit {
using TessEndFn = void(__fastcall*)(void* tess, void* outMesh, int usage, const void* name,
                                    unsigned int flag);
using TessClearFn = void(__fastcall*)(void* buffers);
using RendererFlagFn = unsigned int(__fastcall*)();
using MeshRenderFn = void(__fastcall*)(void* mesh, void* ctxPlus10, const void* material,
                                       void* params, void* a5, void* a6, void* texture, void* a8,
                                       void* optional);
using MeshDestroyFn = void(__fastcall*)(void* mesh);
extern TessEndFn g_meshEnd;
extern TessClearFn g_tessClear;
extern RendererFlagFn g_rendererFlag;
extern MeshRenderFn g_meshRender;
extern MeshDestroyFn g_meshDestroy;
struct MaterialPtr {
    void* info = nullptr;
    void* ctrl = nullptr;
};

struct HashedStringBuf {
    std::uint64_t hash;
    char text[16];
    std::uint64_t size;
    std::uint64_t capacity;
    std::uint64_t extra;
};
static_assert(sizeof(HashedStringBuf) == 0x30);

struct alignas(16) MeshStorage {
    unsigned char bytes[0x400];
};
struct TessSaved {
    unsigned char offsetScale[24];
    unsigned char useMatrix;
    unsigned char noColor;
    float color[4];
    unsigned char colorDirty;
    bool haveColor;
};

struct MeshName {
    const char* text;
    std::size_t length;
};
extern MeshName g_meshName;
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

using MaterialCtorFn = void*(__fastcall*)(void* out, void* group, const void* name);
extern MaterialCtorFn g_matCtor;
extern void* g_matGroup;
struct Material { MaterialPtr ptr{}; bool ok = false; };
std::uint64_t fnv1(std::string_view text);
void makeHashed(std::string_view name, char* longName, std::size_t longCapacity, HashedStringBuf& out);
bool callMaterialCtor(MaterialPtr* out, void* group, const void* name);
bool infoMatches(const void* info, std::uint64_t hash);
bool makeMaterial(void* group, std::string_view name, Material& out);
void* terrainMaterialGroup();
bool resolve();
bool ready();
bool pageChecked(const void* address, std::size_t size, bool write);
bool saveState(void* ctx, void* tess, TessSaved& saved);
void restoreState(void* ctx, void* tess, const TessSaved& saved);
bool findTopMatrix(void* ctx, TopMatrix& m);
void restoreTopMatrix(TopMatrix& m);
void resetTessellator(void* tess);
void destroyMesh(void* mesh);
bool renderMesh(void* ctx, const void* material, TopMatrix& m, void* mesh, const float rgba[4],
                const float position[3], const float* sortPos, const unsigned char* texture, bool singleBlock);
double regionBoxDistance(const std::int32_t origin[3], const double cam[3]);
bool regionVisible(const std::int32_t origin[3], const double cam[3], const float vp[16], bool haveVp);
}
