#include "render/GhostLayer.h"

#include "core/Logger.h"
#include "memory/Memory.h"
#include "core/Paths.h"
#include "game/BlockRegistry.h"
#include "hooks/Detours.h"
#include "hooks/HookManager.h"
#include "render/BoxRenderer.h"
#include "render/DiffAtlas.h"
#include "render/FrameTrace.h"

#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace tsukuyomi::ghost {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kSetPipelineStateIndex = 25;

constexpr std::size_t kOmSetBlendFactorIndex = 23;

constexpr std::size_t kSetGraphicsRoot32BitConstantsIndex = 36;
constexpr std::size_t kSetGraphicsRootCbvIndex = 38;

constexpr std::size_t kCreateGraphicsPipelineStateIndex = 10;
constexpr std::size_t kCreatePipelineStateIndex = 47;

constexpr std::size_t kCreateCommittedResourceIndex = 27;

constexpr std::size_t kCreatePlacedResourceIndex = 29;

using SetPipelineStateFn = void(__stdcall*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using OmSetBlendFactorFn = void(__stdcall*)(ID3D12GraphicsCommandList*, const FLOAT[4]);
using CreateGraphicsPipelineStateFn = HRESULT(__stdcall*)(
    ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using CreatePipelineStateFn = HRESULT(__stdcall*)(ID3D12Device2*,
                                                  const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID,
                                                  void**);

using CreateCommittedResourceFn = HRESULT(__stdcall*)(ID3D12Device*,
                                                      const D3D12_HEAP_PROPERTIES*,
                                                      D3D12_HEAP_FLAGS,
                                                      const D3D12_RESOURCE_DESC*,
                                                      D3D12_RESOURCE_STATES,
                                                      const D3D12_CLEAR_VALUE*, REFIID, void**);

SetPipelineStateFn g_setPipelineState = nullptr;
OmSetBlendFactorFn g_omSetBlendFactor = nullptr;
CreateGraphicsPipelineStateFn g_createGraphicsPipelineState = nullptr;
CreatePipelineStateFn g_createPipelineState = nullptr;
CreateCommittedResourceFn g_createCommittedResource = nullptr;

enum Twin {
    kTwinFactor = 0,
    kTwinSolid,
    kTwinAdd,
    kTwinMul,
    kTwinSrcAlpha,
    kTwinSampleMask,
    kTwinCount
};

struct GhostPso {
    ID3D12PipelineState* origin = nullptr;
    ID3D12PipelineState* twin[kTwinCount] = {};
    std::atomic<unsigned long long> swaps{0};

    GhostPso() = default;
    GhostPso(const GhostPso& other)
        : origin(other.origin), swaps(other.swaps.load(std::memory_order_relaxed))
    {
        for (int i = 0; i < kTwinCount; ++i) {
            twin[i] = other.twin[i];
        }
    }
};

std::mutex g_psoGuard;
std::vector<GhostPso> g_psos;

std::atomic<unsigned char> g_mark{0};

std::atomic<int> g_markWindow{0};

std::atomic<unsigned> g_matchInWindow{0};
std::atomic<unsigned> g_matchGhostOn{0};
std::atomic<unsigned> g_matchIdle{0};

std::atomic<bool> g_teardown{false};

std::atomic<int> g_diagBe{0};
std::atomic<unsigned long long> g_diagBeRead{0};

int readDiagBeFile()
{
    std::error_code ec;
    const std::filesystem::path file = paths::dataDir() / L"diag-be.txt";
    if (!std::filesystem::exists(file, ec)) {
        return 0;
    }
    std::ifstream in{file};
    if (!in) {
        return 0;
    }
    int mode = 0;
    if (!(in >> mode) || mode < 0 || mode > 10) {
        return 0;
    }
    return mode;
}

Twin twinForMode(int mode)
{
    switch (mode) {
    case 3: return kTwinSolid;
    case 4: return kTwinAdd;
    case 5: return kTwinMul;
    case 6: return kTwinSrcAlpha;
    case 10: return kTwinSampleMask;
    default: return kTwinFactor;
    }
}

int diagBeNow()
{
    const ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_diagBeRead.load(std::memory_order_acquire);
    if (last == 0 || now - last >= 500) {
        if (g_diagBeRead.compare_exchange_strong(last, now, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            const int mode = readDiagBeFile();
            g_diagBe.store(mode, std::memory_order_release);
            return mode;
        }
    }
    return g_diagBe.load(std::memory_order_acquire);
}

std::atomic<unsigned> g_reports{0};

bool looksLikeGhostDesc(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
    const unsigned char mark = g_mark.load(std::memory_order_acquire);
    if (mark == 0) {
        return false;
    }
    if (desc.BlendState.RenderTarget[0].RenderTargetWriteMask != mark) {
        return false;
    }
    const bool inWindow = g_markWindow.load(std::memory_order_acquire) > 0;
    if (inWindow) {
        g_matchInWindow.fetch_add(1, std::memory_order_relaxed);
    } else if (blocks::ghostOn()) {
        g_matchGhostOn.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_matchIdle.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

ID3D12PipelineState* makeConstantBlendTwin(ID3D12Device* device,
                                           const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                           Twin kind)
{
    if (device == nullptr || desc == nullptr || g_createGraphicsPipelineState == nullptr) {
        return nullptr;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC copy = *desc;
    copy.CachedPSO.pCachedBlob = nullptr;
    copy.CachedPSO.CachedBlobSizeInBytes = 0;

    static std::atomic<unsigned> told{0};
    if (kind == kTwinFactor && told.load(std::memory_order_relaxed) < 4) {
        told.fetch_add(1, std::memory_order_relaxed);
    }

    if (kind != kTwinSolid) {
        copy.BlendState.AlphaToCoverageEnable = FALSE;
    }

    if (kind == kTwinSampleMask) {
        const UINT samples = copy.SampleDesc.Count;
        if (samples < 2 || samples > 32) {
            return nullptr;
        }
        const float a = blocks::ghostAlpha();
        UINT bits = static_cast<UINT>(a * static_cast<float>(samples) + 0.5F);
        if (bits < 1) {
            bits = 1;
        }
        if (bits > samples) {
            bits = samples;
        }
        UINT mask = 0;
        for (UINT i = 0; i < bits; ++i) {
            mask |= 1u << i;
        }
        copy.SampleMask = mask;
        static std::atomic<unsigned> told{0};
    }

    const UINT targets = copy.BlendState.IndependentBlendEnable != 0
                             ? (copy.NumRenderTargets > 0 ? copy.NumRenderTargets : 1)
                             : 1;
    for (UINT i = 0; i < targets && i < 8; ++i) {
        D3D12_RENDER_TARGET_BLEND_DESC& rt = copy.BlendState.RenderTarget[i];
        if (kind != kTwinSolid && kind != kTwinSampleMask) {
            rt.BlendEnable = TRUE;
            rt.LogicOpEnable = FALSE;
            rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        switch (kind) {
        case kTwinFactor:
            rt.SrcBlend = D3D12_BLEND_BLEND_FACTOR;
            rt.DestBlend = D3D12_BLEND_INV_BLEND_FACTOR;
            rt.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
            rt.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
            break;
        case kTwinAdd:
            rt.SrcBlend = D3D12_BLEND_ONE;
            rt.DestBlend = D3D12_BLEND_ONE;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlendAlpha = D3D12_BLEND_ONE;
            break;
        case kTwinMul:
            rt.SrcBlend = D3D12_BLEND_BLEND_FACTOR;
            rt.DestBlend = D3D12_BLEND_ZERO;
            rt.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
            rt.DestBlendAlpha = D3D12_BLEND_ZERO;
            break;
        case kTwinSrcAlpha:
            rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case kTwinSampleMask:
            rt.BlendEnable = FALSE;
            break;
        case kTwinSolid:
        default:
            break;
        }
        rt.RenderTargetWriteMask = 0x0F;
    }

    ID3D12PipelineState* twin = nullptr;
    const HRESULT hr = g_createGraphicsPipelineState(
        device, &copy, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&twin));
    if (FAILED(hr) || twin == nullptr) {
        return nullptr;
    }
    return twin;
}

struct UploadRange {
    D3D12_GPU_VIRTUAL_ADDRESS va = 0;
    UINT64 size = 0;
    unsigned char* cpu = nullptr;
};
std::mutex g_uploadGuard;
std::vector<UploadRange> g_uploads;
constexpr std::size_t kMaxUploads = 4096;
thread_local bool t_ourMap = false;

void noteUploadResource(ID3D12Resource* res, const D3D12_HEAP_PROPERTIES* heap,
                        const D3D12_RESOURCE_DESC* desc)
{
    if (res == nullptr || heap == nullptr || desc == nullptr) {
        return;
    }
    if (heap->Type != D3D12_HEAP_TYPE_UPLOAD
        || desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(g_uploadGuard);
        if (g_uploads.size() >= kMaxUploads) {
            return;
        }
    }
    void* cpu = nullptr;
    t_ourMap = true;
    const HRESULT mapped = res->Map(0, nullptr, &cpu);
    t_ourMap = false;
    if (FAILED(mapped) || cpu == nullptr) {
        return;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS va = res->GetGPUVirtualAddress();
    if (va == 0) {
        return;
    }
    const std::lock_guard<std::mutex> lock(g_uploadGuard);
    g_uploads.push_back({va, desc->Width, static_cast<unsigned char*>(cpu)});
}

constexpr std::size_t kResourceMapIndex = 8;

using ResourceMapFn = HRESULT(__stdcall*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**);
ResourceMapFn g_resourceMap = nullptr;

HRESULT __stdcall detourResourceMap(ID3D12Resource* res, UINT sub, const D3D12_RANGE* range,
                                    void** data)
{
    const HRESULT hr =
        g_resourceMap != nullptr ? g_resourceMap(res, sub, range, data) : E_FAIL;
    if (t_ourMap || FAILED(hr) || res == nullptr || data == nullptr || *data == nullptr
        || sub != 0 || g_teardown.load(std::memory_order_acquire)) {
        return hr;
    }
    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        return hr;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS va = res->GetGPUVirtualAddress();
    if (va == 0) {
        return hr;
    }
    const std::lock_guard<std::mutex> lock(g_uploadGuard);
    if (g_uploads.size() >= kMaxUploads) {
        return hr;
    }
    for (const UploadRange& r : g_uploads) {
        if (r.va == va) {
            return hr;
        }
    }
    g_uploads.push_back({va, desc.Width, static_cast<unsigned char*>(*data)});
    return hr;
}

unsigned char* cpuOfGpuAddress(D3D12_GPU_VIRTUAL_ADDRESS va, UINT64& leftOut)
{
    const std::lock_guard<std::mutex> lock(g_uploadGuard);
    for (const UploadRange& r : g_uploads) {
        if (va >= r.va && va < r.va + r.size) {
            leftOut = r.va + r.size - va;
            return r.cpu + (va - r.va);
        }
    }
    return nullptr;
}

struct RootCbvSlot {
    UINT index = 0xFFFFFFFFU;
    D3D12_GPU_VIRTUAL_ADDRESS va = 0;
};
constexpr std::size_t kMaxRootCbv = 8;
thread_local RootCbvSlot t_rootCbv[kMaxRootCbv];

using SetGraphicsRootCbvFn = void(__stdcall*)(ID3D12GraphicsCommandList*, UINT,
                                              D3D12_GPU_VIRTUAL_ADDRESS);
SetGraphicsRootCbvFn g_setGraphicsRootCbv = nullptr;

void __stdcall detourSetGraphicsRootCbv(ID3D12GraphicsCommandList* list, UINT index,
                                        D3D12_GPU_VIRTUAL_ADDRESS va)
{
    if (index < kMaxRootCbv) {
        t_rootCbv[index].index = index;
        t_rootCbv[index].va = va;
    }
    if (g_setGraphicsRootCbv != nullptr) {
        g_setGraphicsRootCbv(list, index, va);
    }
}

using SetGraphicsRoot32Fn = void(__stdcall*)(ID3D12GraphicsCommandList*, UINT, UINT,
                                             const void*, UINT);
SetGraphicsRoot32Fn g_setGraphicsRoot32 = nullptr;

void __stdcall detourSetGraphicsRoot32(ID3D12GraphicsCommandList* list, UINT index, UINT count,
                                       const void* data, UINT offset)
{
    if (g_setGraphicsRoot32 != nullptr) {
        g_setGraphicsRoot32(list, index, count, data, offset);
    }
}

std::atomic<unsigned> g_peekTold[kMaxRootCbv] = {};

std::atomic<int> g_pokeSlot{-1};
std::atomic<unsigned> g_pokeOff{0};
std::atomic<ULONGLONG> g_pokeReadAt{0};

void readPokeFile()
{
    const ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_pokeReadAt.load(std::memory_order_acquire);
    if (last != 0 && now - last < 500) {
        return;
    }
    if (!g_pokeReadAt.compare_exchange_strong(last, now, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        return;
    }
    int slot = -1;
    unsigned off = 0;
    std::error_code ec;
    const std::filesystem::path file = paths::dataDir() / L"diag-cbv.txt";
    if (std::filesystem::exists(file, ec)) {
        std::ifstream in{file};
        if (!in || !(in >> slot) || !(in >> off) || slot < 0
            || slot >= static_cast<int>(kMaxRootCbv) || off > 0x1000) {
            slot = -1;
        }
    }
    g_pokeSlot.store(slot, std::memory_order_release);
    g_pokeOff.store(off, std::memory_order_release);
}

void pokeRootConstants()
{
    readPokeFile();
    const int slot = g_pokeSlot.load(std::memory_order_acquire);
    if (slot < 0) {
        return;
    }
    const RootCbvSlot cbv = t_rootCbv[static_cast<std::size_t>(slot)];
    if (cbv.index == 0xFFFFFFFFU || cbv.va == 0) {
        return;
    }
    UINT64 left = 0;
    unsigned char* cpu = cpuOfGpuAddress(cbv.va, left);
    const unsigned off = g_pokeOff.load(std::memory_order_acquire);
    if (cpu == nullptr) {
        auto* const raw = reinterpret_cast<unsigned char*>(static_cast<std::uintptr_t>(cbv.va));
        if (!memory::isReadable(raw, off + 4 * sizeof(float))) {
            return;
        }
        cpu = raw;
        left = off + 4 * sizeof(float);
    }
    if (left < off + 4 * sizeof(float)) {
        return;
    }
    reinterpret_cast<float*>(cpu + off)[3] = blocks::ghostAlpha();
}

thread_local bool t_ourBlendFactor = false;
std::atomic<unsigned long long> g_gameBlendFactor{0};
std::atomic<unsigned long long> g_gameBlendFactorOn{0};

void __stdcall detourOmSetBlendFactor(ID3D12GraphicsCommandList* list, const FLOAT factor[4])
{
    if (!t_ourBlendFactor) {
        g_gameBlendFactor.fetch_add(1, std::memory_order_relaxed);
        if (blocks::ghostOn()) {
            g_gameBlendFactorOn.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<unsigned> told{0};
        }
    }
    if (g_omSetBlendFactor != nullptr) {
        g_omSetBlendFactor(list, factor);
    }
}

void setOurBlendFactor(ID3D12GraphicsCommandList* list, const float factor[4])
{
    t_ourBlendFactor = true;
    list->OMSetBlendFactor(factor);
    t_ourBlendFactor = false;
}

void putOurBlendFactor(ID3D12GraphicsCommandList* list)
{
    const float a = blocks::ghostAlpha();
    const float factor[4] = {a, a, a, a};
    setOurBlendFactor(list, factor);
}

thread_local ID3D12GraphicsCommandList* t_twinList = nullptr;

std::atomic<unsigned long long> g_psoInGhostBe{0};
std::atomic<unsigned long long> g_psoInGhostBeHit{0};
std::atomic<unsigned long long> g_drawInGhostBe{0};

std::atomic<unsigned long long> g_listType[8] = {};

void noteListType(ID3D12GraphicsCommandList* list)
{
    if (list == nullptr) {
        return;
    }
    const int type = static_cast<int>(list->GetType());
    if (type >= 0 && type < 8) {
        g_listType[type].fetch_add(1, std::memory_order_relaxed);
    }
}

void __stdcall detourSetPipelineState(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso)
{
    const int diag = diagBeNow();
    bool swapped = false;
    ID3D12PipelineState* const gamePso = pso;
    const bool inGhostBe = pso != nullptr && hooks::inGhostBeDispatch();
    if (inGhostBe) {
        g_psoInGhostBe.fetch_add(1, std::memory_order_relaxed);
    }
    if (pso != nullptr && diag != 2 && blocks::ghostOn()
        && !g_teardown.load(std::memory_order_acquire)) {
        ID3D12PipelineState* twin = nullptr;
        {
            const std::lock_guard<std::mutex> lock(g_psoGuard);
            for (GhostPso& entry : g_psos) {
                if (entry.origin == pso) {
                    twin = entry.twin[twinForMode(diag)];
                    if (twin != nullptr && inGhostBe) {
                        g_psoInGhostBeHit.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (twin != nullptr) {
                        entry.swaps.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                }
            }
        }
        if (twin != nullptr) {
            noteListType(list);
            if (diag == 8) {
                putOurBlendFactor(list);
            }
            pso = twin;
            swapped = true;
        }
    }
    if (g_setPipelineState != nullptr) {
        g_setPipelineState(list, pso);
    }
    frametrace::onSetPso(list, gamePso, swapped);
    if (swapped && diag != 8) {
        putOurBlendFactor(list);
    }
    if (swapped) {
        pokeRootConstants();
    }
    t_twinList = swapped ? list : nullptr;
}

void beforeDrawImpl(ID3D12GraphicsCommandList* list)
{
    if (hooks::inGhostBeDispatch()) {
        g_drawInGhostBe.fetch_add(1, std::memory_order_relaxed);
    }
    if (t_twinList == nullptr || t_twinList != list) {
        return;
    }
    if (diagBeNow() == 9) {
        return;
    }
    putOurBlendFactor(list);
}

void tellAdapterOnce(ID3D12Device* device)
{
    static std::atomic<bool> told{false};
    if (device == nullptr || told.exchange(true)) {
        return;
    }
    const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (dxgi == nullptr) {
        return;
    }
    using CreateFactory1Fn = HRESULT(__stdcall*)(REFIID, void**);
    const auto create =
        reinterpret_cast<CreateFactory1Fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (create == nullptr) {
        return;
    }
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(create(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(factory.GetAddressOf())))) {
        return;
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), __uuidof(IDXGIAdapter1),
                                          reinterpret_cast<void**>(adapter.GetAddressOf())))) {
        return;
    }
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) {
        return;
    }
}

HRESULT __stdcall detourCreateGraphicsPipelineState(ID3D12Device* device,
                                                    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                                    REFIID riid, void** out)
{
    tellAdapterOnce(device);

    boxes::noteGamePso(desc);

    const HRESULT hr = g_createGraphicsPipelineState != nullptr
                           ? g_createGraphicsPipelineState(device, desc, riid, out)
                           : E_FAIL;
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        frametrace::notePso(static_cast<ID3D12PipelineState*>(*out), desc);
    }
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && desc != nullptr
        && !g_teardown.load(std::memory_order_acquire) && looksLikeGhostDesc(*desc)) {
        if (ID3D12PipelineState* const twin = makeConstantBlendTwin(device, desc, kTwinFactor)) {
            GhostPso entry;
            entry.origin = static_cast<ID3D12PipelineState*>(*out);
            entry.twin[kTwinFactor] = twin;
            for (int k = kTwinSolid; k < kTwinCount; ++k) {
                entry.twin[k] = makeConstantBlendTwin(device, desc, static_cast<Twin>(k));
            }
            const std::lock_guard<std::mutex> lock(g_psoGuard);
            if (g_psos.size() < 256) {
                g_psos.push_back(entry);
            }
        }
    }
    return hr;
}

HRESULT __stdcall detourCreatePipelineState(ID3D12Device2* device,
                                            const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                            REFIID riid, void** out)
{
    const HRESULT hr =
        g_createPipelineState != nullptr ? g_createPipelineState(device, desc, riid, out) : E_FAIL;
    return hr;
}

HRESULT __stdcall detourCreateCommittedResource(ID3D12Device* device,
                                                const D3D12_HEAP_PROPERTIES* heap,
                                                D3D12_HEAP_FLAGS flags,
                                                const D3D12_RESOURCE_DESC* desc,
                                                D3D12_RESOURCE_STATES state,
                                                const D3D12_CLEAR_VALUE* clear, REFIID riid,
                                                void** out)
{
    const HRESULT hr = g_createCommittedResource != nullptr
                           ? g_createCommittedResource(device, heap, flags, desc, state, clear,
                                                       riid, out)
                           : E_FAIL;
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && desc != nullptr
        && !g_teardown.load(std::memory_order_acquire)) {
        atlas::noteCreatedResource(static_cast<ID3D12Resource*>(*out),
                                   static_cast<unsigned>(state), desc);
        if (hooks::beModelsOn()) {
            atlas::noteAnyTexture(static_cast<ID3D12Resource*>(*out), desc,
                                  static_cast<unsigned>(state));
        }
        noteUploadResource(static_cast<ID3D12Resource*>(*out), heap, desc);
    }
    return hr;
}

using CreatePlacedResourceFn = HRESULT(__stdcall*)(ID3D12Device*, ID3D12Heap*, UINT64,
                                                   const D3D12_RESOURCE_DESC*,
                                                   D3D12_RESOURCE_STATES,
                                                   const D3D12_CLEAR_VALUE*, REFIID, void**);
CreatePlacedResourceFn g_createPlacedResource = nullptr;

HRESULT __stdcall detourCreatePlacedResource(ID3D12Device* device, ID3D12Heap* heap,
                                             UINT64 offset, const D3D12_RESOURCE_DESC* desc,
                                             D3D12_RESOURCE_STATES state,
                                             const D3D12_CLEAR_VALUE* clear, REFIID riid,
                                             void** out)
{
    const HRESULT hr = g_createPlacedResource != nullptr
                           ? g_createPlacedResource(device, heap, offset, desc, state, clear,
                                                    riid, out)
                           : E_FAIL;
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && desc != nullptr && heap != nullptr
        && !g_teardown.load(std::memory_order_acquire)) {
        const D3D12_HEAP_DESC heapDesc = heap->GetDesc();
        noteUploadResource(static_cast<ID3D12Resource*>(*out), &heapDesc.Properties, desc);
        if (hooks::beModelsOn()) {
            atlas::noteAnyTexture(static_cast<ID3D12Resource*>(*out), desc,
                                  static_cast<unsigned>(state));
        }
    }
    return hr;
}

void* vtableEntry(void* object, std::size_t index)
{
    if (object == nullptr) {
        return nullptr;
    }
    void*** const asVtable = static_cast<void***>(object);
    void** const vtable = *asVtable;
    return vtable != nullptr ? vtable[index] : nullptr;
}

struct Targets {
    void* setPso = nullptr;
    void* omSetBlendFactor = nullptr;
    void* createGraphicsPso = nullptr;
    void* createPso = nullptr;
    void* createCommitted = nullptr;
    void* setGraphicsRootCbv = nullptr;
    void* setGraphicsRoot32 = nullptr;
    void* resourceMap = nullptr;
    void* createPlaced = nullptr;
};

bool captureTargets(Targets& t)
{
    const HMODULE d3d12Module = GetModuleHandleW(L"d3d12.dll");
    if (d3d12Module == nullptr) {
        log().warn(L"GhostLayer: d3d12.dll is not loaded yet");
        return false;
    }
    const auto createDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
        GetProcAddress(d3d12Module, "D3D12CreateDevice"));
    if (createDevice == nullptr) {
        return false;
    }

    ComPtr<ID3D12Device> device;
    if (FAILED(createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        log().error(L"GhostLayer: could not create the probe device");
        return false;
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator)))) {
        log().error(L"GhostLayer: could not create the probe command allocator");
        return false;
    }

    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                         nullptr, IID_PPV_ARGS(&list)))) {
        log().error(L"GhostLayer: could not create the probe command list");
        return false;
    }
    list->Close();

    t.setPso = vtableEntry(list.Get(), kSetPipelineStateIndex);
    t.omSetBlendFactor = vtableEntry(list.Get(), kOmSetBlendFactorIndex);
    t.setGraphicsRootCbv = vtableEntry(list.Get(), kSetGraphicsRootCbvIndex);
    t.setGraphicsRoot32 = vtableEntry(list.Get(), kSetGraphicsRoot32BitConstantsIndex);
    t.createGraphicsPso = vtableEntry(device.Get(), kCreateGraphicsPipelineStateIndex);
    t.createCommitted = vtableEntry(device.Get(), kCreateCommittedResourceIndex);
    t.createPlaced = vtableEntry(device.Get(), kCreatePlacedResourceIndex);
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bufDesc{};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = 256;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> probe;
        if (SUCCEEDED(device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&probe)))) {
            t.resourceMap = vtableEntry(probe.Get(), kResourceMapIndex);
        }
    }

    ComPtr<ID3D12Device2> device2;
    if (SUCCEEDED(device.As(&device2))) {
        t.createPso = vtableEntry(device2.Get(), kCreatePipelineStateIndex);
    }
    return t.setPso != nullptr && t.createGraphicsPso != nullptr;
}

}

bool installGhostLayerHooks()
{
    Targets t;
    if (!captureTargets(t)) {
        log().warn(L"GhostLayer: could not hook D3D12");
        return false;
    }

    HookManager& hooks = HookManager::instance();
    bool ok = hooks.create(t.setPso, &detourSetPipelineState,
                           reinterpret_cast<void**>(&g_setPipelineState), L"D3D12SetPipelineState")
              && hooks.create(t.createGraphicsPso, &detourCreateGraphicsPipelineState,
                              reinterpret_cast<void**>(&g_createGraphicsPipelineState),
                              L"D3D12CreateGraphicsPipelineState");
    if (t.createPso != nullptr) {
        ok = hooks.create(t.createPso, &detourCreatePipelineState,
                          reinterpret_cast<void**>(&g_createPipelineState),
                          L"D3D12CreatePipelineState")
             && ok;
    }
    if (t.omSetBlendFactor != nullptr) {
        if (!hooks.create(t.omSetBlendFactor, &detourOmSetBlendFactor,
                          reinterpret_cast<void**>(&g_omSetBlendFactor),
                          L"D3D12OMSetBlendFactor")) {
            log().warn(L"GhostLayer: could not hook OMSetBlendFactor");
        }
    }

    if (t.setGraphicsRootCbv != nullptr) {
        if (!hooks.create(t.setGraphicsRootCbv, &detourSetGraphicsRootCbv,
                          reinterpret_cast<void**>(&g_setGraphicsRootCbv),
                          L"D3D12SetGraphicsRootCBV")) {
            log().warn(L"GhostLayer: could not hook SetGraphicsRootConstantBufferView");
        }
    }
    if (t.setGraphicsRoot32 != nullptr) {
        if (!hooks.create(t.setGraphicsRoot32, &detourSetGraphicsRoot32,
                          reinterpret_cast<void**>(&g_setGraphicsRoot32),
                          L"D3D12SetGraphicsRoot32BitConstants")) {
            log().warn(L"GhostLayer: could not hook SetGraphicsRoot32BitConstants");
        }
    }

    if (t.createPlaced != nullptr) {
        if (!hooks.create(t.createPlaced, &detourCreatePlacedResource,
                          reinterpret_cast<void**>(&g_createPlacedResource),
                          L"D3D12CreatePlacedResource")) {
            log().warn(L"GhostLayer: could not hook CreatePlacedResource");
        }
    }

    if (t.resourceMap != nullptr) {
        if (!hooks.create(t.resourceMap, &detourResourceMap,
                          reinterpret_cast<void**>(&g_resourceMap), L"D3D12ResourceMap")) {
            log().warn(L"GhostLayer: could not hook ID3D12Resource::Map");
        }
    }

    if (t.createCommitted != nullptr) {
        if (!hooks.create(t.createCommitted, &detourCreateCommittedResource,
                          reinterpret_cast<void**>(&g_createCommittedResource),
                          L"D3D12CreateCommittedResource")) {
            log().warn(L"GhostLayer: could not hook CreateCommittedResource (our own tiles "
                       L"stay stuck waiting for a barrier)");
        }
    }
    return ok;
}

void setGhostMark(unsigned char writeMask)
{
    g_mark.store(writeMask, std::memory_order_release);
}

void beginMarkWindow()
{
    g_markWindow.fetch_add(1, std::memory_order_release);
}

void endMarkWindow()
{
    int now = g_markWindow.load(std::memory_order_acquire);
    while (now > 0
           && !g_markWindow.compare_exchange_weak(now, now - 1, std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
    }
}

int diagBeMode()
{
    return diagBeNow();
}

void beforeDraw(ID3D12GraphicsCommandList* list)
{
    beforeDrawImpl(list);
}

void shutdownGhostLayer()
{
    g_teardown.store(true, std::memory_order_release);

}

}
