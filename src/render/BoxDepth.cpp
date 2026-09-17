#include "render/BoxRenderer.h"
#include "render/GhostLayer.h"
#include "render/DiffAtlas.h"
#include "render/FrameTrace.h"

#include "core/Logger.h"
#include "core/Paths.h"
#include "hooks/HookManager.h"
#include "render/Overlay.h"
#include "render/WorldMesh.h"

#include <Windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <filesystem>
#include <mutex>
#include <vector>

namespace tsukuyomi::boxes {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kOmSetRenderTargetsIndex = 46;

using OmSetRenderTargetsFn = void(__stdcall*)(ID3D12GraphicsCommandList*, UINT,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE*);
OmSetRenderTargetsFn g_om = nullptr;

constexpr std::size_t kMaxBoxes = 500000;
constexpr std::size_t kInstanceRings = 3;

struct Instance {
    float x;
    float y;
    float z;
    std::uint32_t color;
};

struct Formats {
    std::atomic<bool> have{false};
    std::atomic<unsigned> numRt{0};
    std::atomic<unsigned> rt[8]{};
    std::atomic<unsigned> dsv{0};
    std::atomic<unsigned> sampleCount{1};
    std::atomic<unsigned> sampleQuality{0};
};
Formats g_formats;

std::atomic<bool> g_ready{false};
std::atomic<bool> g_failed{false};
std::atomic<bool> g_teardown{false};

std::atomic<unsigned> g_departThisFrame{0};
std::atomic<unsigned> g_drawAt{1};
std::atomic<unsigned> g_bindThisFrame{0};
std::atomic<unsigned> g_bindAt{1};
std::atomic<unsigned> g_reports{0};

std::atomic<void*> g_worldColor{nullptr};
std::atomic<SIZE_T> g_worldRtv{0};
std::atomic<SIZE_T> g_lastDsv{0};

std::atomic<unsigned> g_clearThisFrame{0};
std::atomic<unsigned> g_clearLastFrame{0};
std::atomic<bool> g_drewThisFrame{false};
std::atomic<unsigned> g_clearLogged{0};
std::atomic<unsigned> g_drawWhenLogged{0};

std::atomic<SIZE_T> g_busyRtv{0};
std::atomic<SIZE_T> g_busyDsv{0};
std::atomic<unsigned> g_busyHits{0};
std::atomic<unsigned> g_busyDrawsNow{0};
std::atomic<bool> g_useAlt{false};
std::atomic<unsigned> g_drawAtPercent{95};

std::atomic<unsigned> g_frameClears{0};
std::atomic<SIZE_T> g_frameWorldRtv{0};
std::atomic<SIZE_T> g_frameWorldDsv{0};
std::atomic<unsigned> g_worldPickLogged{0};

std::atomic<bool> g_haveViewport{false};
std::atomic<float> g_vpTopLeftX{0.0F};
std::atomic<float> g_vpTopLeftY{0.0F};
std::atomic<float> g_vpWidth{0.0F};
std::atomic<float> g_vpHeight{0.0F};
std::atomic<float> g_vpMinDepth{0.0F};
std::atomic<float> g_vpMaxDepth{1.0F};
std::atomic<unsigned> g_endThisFrame{0};
std::atomic<unsigned> g_endLastFrame{0};

std::atomic<unsigned> g_guessColor{0};
std::atomic<unsigned> g_guessSamples{0};
std::atomic<unsigned> g_guessArea{0};
std::atomic<unsigned> g_guessDepth{45};
std::atomic<unsigned> g_forceDepthFormat{0};
std::atomic<SIZE_T> g_worldDsv{0};
std::atomic<unsigned long long> g_barrierCalls{0};
constexpr unsigned long long kBarrierWatch = 40000;

struct PsoSlot {
    std::atomic<unsigned long long> key{0};
    std::atomic<unsigned long long> hits{0};
    std::atomic<unsigned> numRt{0};
    std::atomic<unsigned> rt0{0};
    std::atomic<unsigned> dsv{0};
    std::atomic<unsigned> samples{0};
};
constexpr std::size_t kPsoSlots = 32;
PsoSlot g_psoKinds[kPsoSlots];

std::atomic<int> g_depthFunc{static_cast<int>(D3D12_COMPARISON_FUNC_LESS_EQUAL)};

struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> faceDepth;
    ComPtr<ID3D12PipelineState> faceXray;
    ComPtr<ID3D12PipelineState> edgeDepth;
    ComPtr<ID3D12PipelineState> edgeXray;
    ComPtr<ID3D12Resource> cube;
    ComPtr<ID3D12Resource> faceIndex;
    ComPtr<ID3D12Resource> edgeIndex;
    ComPtr<ID3D12Resource> instance[kInstanceRings];
    Instance* mapped[kInstanceRings] = {};
    std::size_t ring = 0;
    std::size_t count = 0;
    unsigned long long version = 0;
};
std::mutex g_gpuLock;
Gpu g_gpu;

constexpr char kShaderSource[] = R"(
cbuffer Root : register(b0)
{
    row_major float4x4 gVp;
    float3 gEye;
    float  gPad;
    float4 gTint;
};

struct VSIn
{
    float3 corner : POSITION;
    float3 origin : TEXCOORD0;
    float4 color  : COLOR0;
};

struct VSOut
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR0;
};

VSOut vsmain(VSIn input)
{
    VSOut output;
    float3 p = input.origin - gEye + input.corner;
    if (gPad > 2.5)
    {
        output.pos = mul(float4(input.origin + input.corner, 1.0), gVp);
    }
    else if (gPad > 1.5)
    {
        output.pos = mul(gVp, float4(p, 1.0));
    }
    else if (gPad > 0.5)
    {
        float2 xy = input.corner.xy * 2.0 - 1.0;
        output.pos = float4(xy * 0.5, 0.5, 1.0);
    }
    else
    {
        output.pos = mul(float4(p, 1.0), gVp);
    }
    output.color = float4(input.color.rgb, input.color.a * gTint.a);
    return output;
}

float4 psmain(VSOut input) : SV_TARGET
{
    return input.color;
}
)";

using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                   ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                   ID3DBlob**);

CompileFn resolveCompiler()
{
    static CompileFn cached = nullptr;
    static bool tried = false;
    if (tried) {
        return cached;
    }
    tried = true;
    HMODULE module = GetModuleHandleW(L"d3dcompiler_47.dll");
    if (module == nullptr) {
        module = LoadLibraryW(L"d3dcompiler_47.dll");
    }
    if (module == nullptr) {
        log().warn(L"BoxDepth: d3dcompiler_47.dll could not be loaded; depth-aware boxes are "
                   L"unavailable");
        return nullptr;
    }
    cached = reinterpret_cast<CompileFn>(GetProcAddress(module, "D3DCompile"));
    return cached;
}

bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out)
{
    const CompileFn compileFn = resolveCompiler();
    if (compileFn == nullptr) {
        return false;
    }
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = compileFn(kShaderSource, sizeof(kShaderSource) - 1, nullptr, nullptr,
                                 nullptr, entry, target, 0, 0, &out, &errors);
    if (FAILED(hr)) {
        std::wstring what;
        if (errors) {
            const char* const text = static_cast<const char*>(errors->GetBufferPointer());
            for (std::size_t i = 0; i < errors->GetBufferSize() && text[i] != 0 && i < 512; ++i) {
                what.push_back(static_cast<wchar_t>(static_cast<unsigned char>(text[i])));
            }
        }
        log().error(L"BoxDepth: shader compilation failed hr={:#x} - {}",
                    static_cast<unsigned>(hr),
                    what);
        return false;
    }
    return true;
}

ComPtr<ID3D12Resource> makeUploadBuffer(ID3D12Device* device, std::size_t bytes, void** mapped)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&buffer)))) {
        return nullptr;
    }
    if (mapped != nullptr) {
        D3D12_RANGE none{0, 0};
        if (FAILED(buffer->Map(0, &none, mapped))) {
            return nullptr;
        }
    }
    return buffer;
}

constexpr float kCorners[8][3] = {
    {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 1.0F, 1.0F},
    {1.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 1.0F}, {1.0F, 1.0F, 0.0F}, {1.0F, 1.0F, 1.0F},
};
constexpr std::uint16_t kFaceIndices[36] = {
    0, 1, 3,  0, 3, 2,
    4, 6, 7,  4, 7, 5,
    0, 4, 5,  0, 5, 1,
    2, 3, 7,  2, 7, 6,
    0, 2, 6,  0, 6, 4,
    1, 5, 7,  1, 7, 3,
};
constexpr std::uint16_t kEdgeIndices[24] = {
    0, 4, 1, 5, 2, 6, 3, 7,
    0, 2, 1, 3, 4, 6, 5, 7,
    0, 1, 2, 3, 4, 5, 6, 7,
};

constexpr std::uint32_t kColors[3] = {
    0xFFFFCC33u,
    0xFF4040FFu,
    0xFF1A99FFu,
};

ComPtr<ID3D12PipelineState> makePso(ID3D12Device* device, ID3D12RootSignature* root,
                                    ID3DBlob* vs, ID3DBlob* ps, bool lines, bool depth)
{
    D3D12_INPUT_ELEMENT_DESC layout[3] = {};
    layout[0].SemanticName = "POSITION";
    layout[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    layout[0].InputSlot = 0;
    layout[0].AlignedByteOffset = 0;
    layout[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    layout[1].SemanticName = "TEXCOORD";
    layout[1].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    layout[1].InputSlot = 1;
    layout[1].AlignedByteOffset = 0;
    layout[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
    layout[1].InstanceDataStepRate = 1;
    layout[2].SemanticName = "COLOR";
    layout[2].Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    layout[2].InputSlot = 1;
    layout[2].AlignedByteOffset = 12;
    layout[2].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
    layout[2].InstanceDataStepRate = 1;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.InputLayout = {layout, 3};
    desc.SampleMask = UINT_MAX;

    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;

    desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    desc.DepthStencilState.DepthEnable = depth ? TRUE : FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc =
        static_cast<D3D12_COMPARISON_FUNC>(g_depthFunc.load(std::memory_order_relaxed));

    desc.PrimitiveTopologyType = lines ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                       : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    const unsigned numRt = g_formats.numRt.load(std::memory_order_relaxed);
    desc.NumRenderTargets = numRt;
    for (unsigned i = 0; i < numRt && i < 8; ++i) {
        desc.RTVFormats[i] =
            static_cast<DXGI_FORMAT>(g_formats.rt[i].load(std::memory_order_relaxed));
    }
    const unsigned forced = g_forceDepthFormat.load(std::memory_order_relaxed);
    desc.DSVFormat = static_cast<DXGI_FORMAT>(
        forced != 0 ? forced : g_formats.dsv.load(std::memory_order_relaxed));
    desc.SampleDesc.Count = g_formats.sampleCount.load(std::memory_order_relaxed);
    desc.SampleDesc.Quality = g_formats.sampleQuality.load(std::memory_order_relaxed);

    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        log().error(L"BoxDepth: could not create the PSO (lines {} / depth {}) hr={:#x}",
                    lines,
                    depth,
                    static_cast<unsigned>(hr));
        return nullptr;
    }
    return pso;
}

bool ensureGpu(ID3D12GraphicsCommandList* list)
{
    if (g_ready.load(std::memory_order_acquire)) {
        return true;
    }
    if (g_failed.load(std::memory_order_acquire)) {
        return false;
    }
    if (!g_formats.have.load(std::memory_order_acquire)) {
        const unsigned samples = g_guessSamples.load(std::memory_order_relaxed);
        const unsigned color = g_guessColor.load(std::memory_order_relaxed);
        if (samples == 0 || color == 0 || g_barrierCalls.load(std::memory_order_relaxed) < 2000) {
            return false;
        }
        g_formats.numRt.store(1, std::memory_order_relaxed);
        g_formats.rt[0].store(color, std::memory_order_relaxed);
        g_formats.dsv.store(g_guessDepth.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
        g_formats.sampleCount.store(samples, std::memory_order_relaxed);
        g_formats.sampleQuality.store(0, std::memory_order_relaxed);
        g_formats.have.store(true, std::memory_order_release);
    }
    const std::lock_guard<std::mutex> guard(g_gpuLock);
    if (g_ready.load(std::memory_order_relaxed) || g_failed.load(std::memory_order_relaxed)) {
        return g_ready.load(std::memory_order_relaxed);
    }

    ComPtr<ID3D12Device> device;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) {
        g_failed.store(true, std::memory_order_release);
        return false;
    }

    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.ShaderRegister = 0;
    param.Constants.Num32BitValues = 24;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &param;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*,
                                         D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    SerializeFn serialize = nullptr;
    if (const HMODULE module = GetModuleHandleW(L"d3d12.dll"); module != nullptr) {
        serialize = reinterpret_cast<SerializeFn>(
            GetProcAddress(module, "D3D12SerializeRootSignature"));
    }
    if (serialize == nullptr) {
        log().error(L"BoxDepth: D3D12SerializeRootSignature could not be resolved");
        g_failed.store(true, std::memory_order_release);
        return false;
    }
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(serialize(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors))) {
        log().error(L"BoxDepth: could not serialize the root signature");
        g_failed.store(true, std::memory_order_release);
        return false;
    }
    ComPtr<ID3D12RootSignature> root;
    if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                           serialized->GetBufferSize(), IID_PPV_ARGS(&root)))) {
        log().error(L"BoxDepth: could not create the root signature");
        g_failed.store(true, std::memory_order_release);
        return false;
    }

    ComPtr<ID3DBlob> vs;
    ComPtr<ID3DBlob> ps;
    if (!compile("vsmain", "vs_5_0", vs) || !compile("psmain", "ps_5_0", ps)) {
        g_failed.store(true, std::memory_order_release);
        return false;
    }

    Gpu gpu;
    gpu.device = device;
    gpu.root = root;
    gpu.faceDepth = makePso(device.Get(), root.Get(), vs.Get(), ps.Get(), false, true);
    gpu.faceXray = makePso(device.Get(), root.Get(), vs.Get(), ps.Get(), false, false);
    gpu.edgeDepth = makePso(device.Get(), root.Get(), vs.Get(), ps.Get(), true, true);
    gpu.edgeXray = makePso(device.Get(), root.Get(), vs.Get(), ps.Get(), true, false);
    if (!gpu.faceDepth || !gpu.faceXray || !gpu.edgeDepth || !gpu.edgeXray) {
        g_failed.store(true, std::memory_order_release);
        return false;
    }

    void* mapped = nullptr;
    gpu.cube = makeUploadBuffer(device.Get(), sizeof(kCorners), &mapped);
    if (!gpu.cube) {
        g_failed.store(true, std::memory_order_release);
        return false;
    }
    std::memcpy(mapped, kCorners, sizeof(kCorners));
    gpu.faceIndex = makeUploadBuffer(device.Get(), sizeof(kFaceIndices), &mapped);
    if (!gpu.faceIndex) {
        g_failed.store(true, std::memory_order_release);
        return false;
    }
    std::memcpy(mapped, kFaceIndices, sizeof(kFaceIndices));
    gpu.edgeIndex = makeUploadBuffer(device.Get(), sizeof(kEdgeIndices), &mapped);
    if (!gpu.edgeIndex) {
        g_failed.store(true, std::memory_order_release);
        return false;
    }
    std::memcpy(mapped, kEdgeIndices, sizeof(kEdgeIndices));

    for (std::size_t i = 0; i < kInstanceRings; ++i) {
        void* at = nullptr;
        gpu.instance[i] = makeUploadBuffer(device.Get(), kMaxBoxes * sizeof(Instance), &at);
        if (!gpu.instance[i]) {
            g_failed.store(true, std::memory_order_release);
            return false;
        }
        gpu.mapped[i] = static_cast<Instance*>(at);
    }

    g_gpu = std::move(gpu);
    g_ready.store(true, std::memory_order_release);
    log().info(L"BoxDepth: depth-aware drawing is ready (render targets {} / depth fmt{} / "
               L"samples {})",
               g_formats.numRt.load(std::memory_order_relaxed),
               g_formats.dsv.load(std::memory_order_relaxed),
               g_formats.sampleCount.load(std::memory_order_relaxed));
    return true;
}

bool solidMode();
bool entryMode();
bool pickCurrentTarget(SIZE_T& rtv, SIZE_T& dsv);

void refreshInstances()
{
    const unsigned long long version = boxVersion();
    if (g_gpu.version == version && g_gpu.count != 0) {
        return;
    }
    const auto boxes = boxSnapshot();
    if (!boxes) {
        g_gpu.count = 0;
        g_gpu.version = version;
        return;
    }
    g_gpu.ring = (g_gpu.ring + 1) % kInstanceRings;
    Instance* const out = g_gpu.mapped[g_gpu.ring];
    const bool keepHidden = boxXray();
    std::size_t at = 0;
    for (const blocks::DiffBox& box : *boxes) {
        const int slot = static_cast<int>(box.color) - 1;
        if (slot < 0 || slot > 2 || at >= kMaxBoxes) {
            continue;
        }
        if (box.hidden && !keepHidden) {
            continue;
        }
        out[at].x = static_cast<float>(box.x);
        out[at].y = static_cast<float>(box.y);
        out[at].z = static_cast<float>(box.z);
        out[at].color = kColors[slot];
        ++at;
    }
    g_gpu.count = at;
    g_gpu.version = version;
    {
        static std::size_t told = static_cast<std::size_t>(-1);
        if (told != at) {
            told = at;
        }
    }
}

void drawInto(ID3D12GraphicsCommandList* list)
{
    if (worldmesh::active(boxXray())) {
        return;
    }
    float eye[3];
    float vp[16];
    if (!cameraSnapshot(eye, vp)) {
        return;
    }
    const std::lock_guard<std::mutex> guard(g_gpuLock);
    if (!g_ready.load(std::memory_order_relaxed)) {
        return;
    }
    refreshInstances();
    if (g_gpu.count == 0) {
        return;
    }
    frametrace::onOurs(list, "boxes");

    const bool solid = solidMode();
    const float face = solid ? 2.0F : boxFaceAlpha();
    const bool xray = solid || boxXray();

    float constants[24] = {};
    std::memcpy(constants, vp, sizeof(vp));
    constants[16] = eye[0];
    constants[17] = eye[1];
    constants[18] = eye[2];
    constants[19] = 0.0F;

    D3D12_VERTEX_BUFFER_VIEW vbv[2]{};
    vbv[0].BufferLocation = g_gpu.cube->GetGPUVirtualAddress();
    vbv[0].SizeInBytes = sizeof(kCorners);
    vbv[0].StrideInBytes = sizeof(float) * 3;
    vbv[1].BufferLocation = g_gpu.instance[g_gpu.ring]->GetGPUVirtualAddress();
    vbv[1].SizeInBytes = static_cast<UINT>(g_gpu.count * sizeof(Instance));
    vbv[1].StrideInBytes = sizeof(Instance);

    D3D12_INDEX_BUFFER_VIEW faceIbv{};
    faceIbv.BufferLocation = g_gpu.faceIndex->GetGPUVirtualAddress();
    faceIbv.SizeInBytes = sizeof(kFaceIndices);
    faceIbv.Format = DXGI_FORMAT_R16_UINT;
    D3D12_INDEX_BUFFER_VIEW edgeIbv{};
    edgeIbv.BufferLocation = g_gpu.edgeIndex->GetGPUVirtualAddress();
    edgeIbv.SizeInBytes = sizeof(kEdgeIndices);
    edgeIbv.Format = DXGI_FORMAT_R16_UINT;

    if (g_haveViewport.load(std::memory_order_acquire)) {
        D3D12_VIEWPORT viewport{};
        viewport.TopLeftX = g_vpTopLeftX.load(std::memory_order_relaxed);
        viewport.TopLeftY = g_vpTopLeftY.load(std::memory_order_relaxed);
        viewport.Width = g_vpWidth.load(std::memory_order_relaxed);
        viewport.Height = g_vpHeight.load(std::memory_order_relaxed);
        viewport.MinDepth = g_vpMinDepth.load(std::memory_order_relaxed);
        viewport.MaxDepth = g_vpMaxDepth.load(std::memory_order_relaxed);
        D3D12_RECT scissor{static_cast<LONG>(viewport.TopLeftX),
                           static_cast<LONG>(viewport.TopLeftY),
                           static_cast<LONG>(viewport.TopLeftX + viewport.Width),
                           static_cast<LONG>(viewport.TopLeftY + viewport.Height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
    }

    list->SetGraphicsRootSignature(g_gpu.root.Get());
    list->IASetVertexBuffers(0, 2, vbv);

    constants[20] = 1.0F;
    constants[21] = 1.0F;
    constants[22] = 1.0F;
    constants[23] = face * 0.5F;
    list->SetPipelineState(xray ? g_gpu.faceXray.Get() : g_gpu.faceDepth.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetIndexBuffer(&faceIbv);
    list->SetGraphicsRoot32BitConstants(0, 24, constants, 0);
    list->DrawIndexedInstanced(36, static_cast<UINT>(g_gpu.count), 0, 0, 0);

    const float edge = face * 2.2F > 1.0F ? 1.0F : face * 2.2F;
    constants[23] = edge;
    list->SetPipelineState(xray ? g_gpu.edgeXray.Get() : g_gpu.edgeDepth.Get());
    list->SetGraphicsRoot32BitConstants(0, 24, constants, 0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    list->IASetIndexBuffer(&edgeIbv);
    list->DrawIndexedInstanced(24, static_cast<UINT>(g_gpu.count), 0, 0, 0);

}

struct ListSlot {
    std::atomic<void*> key{nullptr};
    std::atomic<bool> world{false};
    std::atomic<SIZE_T> dsv{0};
    std::atomic<SIZE_T> rtv{0};
    std::atomic<unsigned> numRt{0};
};
constexpr std::size_t kListSlots = 64;
ListSlot g_lists[kListSlots];

std::atomic<int> g_solidMode{0};
std::atomic<unsigned> g_departLogged{0};

std::atomic<int> g_entryMode{0};

bool entryMode()
{
    int mode = g_entryMode.load(std::memory_order_relaxed);
    if (mode == 0) {
        std::error_code ec;
        mode = std::filesystem::exists(paths::dataDir() / L"diag-entry.txt", ec) ? 1 : 2;
        g_entryMode.store(mode, std::memory_order_relaxed);
    }
    return mode == 1;
}

bool solidMode()
{
    int mode = g_solidMode.load(std::memory_order_relaxed);
    if (mode == 0) {
        std::error_code ec;
        mode = std::filesystem::exists(paths::dataDir() / L"diag-solid.txt", ec) ? 1 : 2;
        g_solidMode.store(mode, std::memory_order_relaxed);
    }
    return mode == 1;
}

ListSlot& slotFor(void* list)
{
    return g_lists[(reinterpret_cast<std::uintptr_t>(list) >> 4) % kListSlots];
}

void __stdcall detourOm(ID3D12GraphicsCommandList* list, UINT numRt,
                        const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single,
                        const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
{
    frametrace::onOm(list, numRt,
                     (numRt > 0 && rtvs != nullptr) ? static_cast<std::uint64_t>(rtvs[0].ptr) : 0,
                     dsv != nullptr ? static_cast<std::uint64_t>(dsv->ptr) : 0);
    bool drawAfter = false;
    if (list != nullptr && !g_teardown.load(std::memory_order_acquire) && boxesOn()) {
        ListSlot& slot = slotFor(list);
        const SIZE_T nowDsv = dsv != nullptr ? dsv->ptr : 0;
        const SIZE_T nowRtv = (numRt > 0 && rtvs != nullptr) ? rtvs[0].ptr : 0;
        const bool nowWorld = numRt > 0 && nowDsv != 0;
        if (nowWorld) {
            g_lastDsv.store(nowDsv, std::memory_order_relaxed);
            if (g_frameClears.load(std::memory_order_relaxed) >= 1
                && g_frameWorldDsv.load(std::memory_order_relaxed) == 0) {
                g_frameWorldRtv.store(nowRtv, std::memory_order_relaxed);
                g_frameWorldDsv.store(nowDsv, std::memory_order_relaxed);
                if (g_worldPickLogged.load(std::memory_order_relaxed) < 3) {
                    g_worldPickLogged.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        const bool sameKey = slot.key.load(std::memory_order_acquire) == list;
        const bool wasWorld = sameKey && slot.world.load(std::memory_order_relaxed);
        const bool same = sameKey && slot.dsv.load(std::memory_order_relaxed) == nowDsv
                          && slot.rtv.load(std::memory_order_relaxed) == nowRtv;
        const SIZE_T world = g_worldDsv.load(std::memory_order_relaxed);
        const bool wasTheWorld =
            wasWorld && world != 0 && slot.dsv.load(std::memory_order_relaxed) == world;
        if (wasTheWorld && !same) {
            const unsigned index = g_departThisFrame.fetch_add(1, std::memory_order_relaxed) + 1;
            if (g_departLogged.load(std::memory_order_relaxed) < 12) {
                g_departLogged.fetch_add(1, std::memory_order_relaxed);
            }
            (void)index;
        }
        if (nowWorld && world != 0 && nowDsv == world) {
            g_bindThisFrame.fetch_add(1, std::memory_order_relaxed);
            g_worldRtv.store(nowRtv, std::memory_order_relaxed);
            if (solidMode()) {
                drawAfter = true;
            }
        }
        if (nowWorld && !g_drewThisFrame.load(std::memory_order_relaxed)) {
            const unsigned last = g_busyHits.load(std::memory_order_relaxed);
            const unsigned now = g_busyDrawsNow.load(std::memory_order_relaxed);
            const unsigned pct = g_drawAtPercent.load(std::memory_order_relaxed);
            if (last != 0 && now * 100 >= last * pct) {
                drawAfter = true;
                g_drewThisFrame.store(true, std::memory_order_relaxed);
            }
        }
        slot.numRt.store(numRt, std::memory_order_relaxed);
        slot.world.store(nowWorld, std::memory_order_relaxed);
        slot.dsv.store(nowDsv, std::memory_order_relaxed);
        slot.rtv.store(nowRtv, std::memory_order_relaxed);
        slot.key.store(list, std::memory_order_release);
    }
    if (g_om != nullptr) {
        g_om(list, numRt, rtvs, single, dsv);
    }
    if (drawAfter && ensureGpu(list)) {
        drawInto(list);
        if (g_drawWhenLogged.load(std::memory_order_relaxed) < 30) {
            g_drawWhenLogged.fetch_add(1, std::memory_order_relaxed);
        }
        (void)0;
        if (g_drawWhenLogged.load(std::memory_order_relaxed) < 6) {
            g_drawWhenLogged.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

constexpr std::size_t kResourceBarrierIndex = 26;

using ResourceBarrierFn = void(__stdcall*)(ID3D12GraphicsCommandList*, UINT,
                                           const D3D12_RESOURCE_BARRIER*);
ResourceBarrierFn g_barrier = nullptr;

struct ResSlot {
    std::atomic<void*> key{nullptr};
    std::atomic<unsigned long long> hits{0};
    std::atomic<unsigned> format{0};
    std::atomic<unsigned> width{0};
    std::atomic<unsigned> height{0};
    std::atomic<unsigned> samples{0};
    std::atomic<unsigned> flags{0};
};
constexpr std::size_t kResSlots = 96;
ResSlot g_res[kResSlots];
std::atomic<int> g_barrierMode{0};

bool barrierMode()
{
    int mode = g_barrierMode.load(std::memory_order_relaxed);
    if (mode == 0) {
        std::error_code ec;
        mode = std::filesystem::exists(paths::dataDir() / L"diag-res.txt", ec) ? 1 : 2;
        g_barrierMode.store(mode, std::memory_order_relaxed);
    }
    return mode == 1;
}

void noteResource(ID3D12Resource* resource)
{
    if (resource == nullptr) {
        return;
    }
    const std::size_t start =
        (reinterpret_cast<std::uintptr_t>(resource) >> 4) % kResSlots;
    for (std::size_t i = 0; i < kResSlots; ++i) {
        ResSlot& slot = g_res[(start + i) % kResSlots];
        void* was = slot.key.load(std::memory_order_acquire);
        if (was == nullptr) {
            void* expected = nullptr;
            if (slot.key.compare_exchange_strong(expected, resource, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                const D3D12_RESOURCE_DESC desc = resource->GetDesc();
                slot.format.store(static_cast<unsigned>(desc.Format), std::memory_order_relaxed);
                slot.width.store(static_cast<unsigned>(desc.Width), std::memory_order_relaxed);
                slot.height.store(desc.Height, std::memory_order_relaxed);
                slot.samples.store(desc.SampleDesc.Count, std::memory_order_relaxed);
                slot.flags.store(static_cast<unsigned>(desc.Flags), std::memory_order_relaxed);
                was = resource;
            } else {
                was = expected;
            }
        }
        if (was == resource) {
            slot.hits.fetch_add(1, std::memory_order_relaxed);
            const unsigned flags = slot.flags.load(std::memory_order_relaxed);
            if ((flags & 0x1u) != 0) {
                const unsigned area = slot.width.load(std::memory_order_relaxed)
                                      * slot.height.load(std::memory_order_relaxed);
                const unsigned samples = slot.samples.load(std::memory_order_relaxed);
                const unsigned score = area + (samples > 1 ? 0x40000000u : 0u);
                if (score > g_guessArea.load(std::memory_order_relaxed)) {
                    g_guessArea.store(score, std::memory_order_relaxed);
                    g_guessColor.store(slot.format.load(std::memory_order_relaxed),
                                       std::memory_order_relaxed);
                    g_guessSamples.store(samples, std::memory_order_relaxed);
                    g_worldColor.store(resource, std::memory_order_relaxed);
                }
            }
            return;
        }
    }
}

void reportResources()
{
    static std::atomic<bool> done{false};
    if (done.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    for (std::size_t i = 0; i < kResSlots; ++i) {
        void* const key = g_res[i].key.load(std::memory_order_acquire);
        if (key == nullptr) {
            continue;
        }
    }
}

bool atlasDiagOn()
{
    static std::atomic<int> mode{0};
    int now = mode.load(std::memory_order_relaxed);
    if (now == 0) {
        std::error_code ec;
        now = std::filesystem::exists(paths::dataDir() / L"diag-atlas.txt", ec) ? 1 : 2;
        mode.store(now, std::memory_order_relaxed);
    }
    return now == 1;
}

void __stdcall detourBarrier(ID3D12GraphicsCommandList* list, UINT count,
                             const D3D12_RESOURCE_BARRIER* barriers)
{
    frametrace::onBarrier(list, count, barriers);
    bool worldEnded = false;
    const unsigned long long calls = g_barrierCalls.fetch_add(1, std::memory_order_relaxed);
    const bool watching = barrierMode() || atlasDiagOn() || calls < kBarrierWatch;
    if (atlasDiagOn() && calls == 20000) {
        reportResources();
    }
    if (barriers != nullptr && !g_teardown.load(std::memory_order_acquire)) {
        void* const worldColor = g_worldColor.load(std::memory_order_relaxed);
        for (UINT i = 0; i < count; ++i) {
            if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                continue;
            }
            const auto& to = barriers[i].Transition.StateAfter;
            const auto& from = barriers[i].Transition.StateBefore;
            const UINT interesting = D3D12_RESOURCE_STATE_RENDER_TARGET
                                     | D3D12_RESOURCE_STATE_DEPTH_WRITE;
            if (!atlasDiagOn() && ((to | from) & interesting) == 0) {
                continue;
            }
            if (watching) {
                noteResource(barriers[i].Transition.pResource);
            }
            if (atlasDiagOn()) {
                ID3D12Resource* const r = barriers[i].Transition.pResource;
                if (r != nullptr) {
                    const D3D12_RESOURCE_DESC d = r->GetDesc();
                    if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
                        && d.Width == d.Height && d.Width >= 512) {
                        static std::atomic<int> shots{0};
                    }
                }
            }
            if (worldColor != nullptr && barriers[i].Transition.pResource == worldColor
                && (from & D3D12_RESOURCE_STATE_RENDER_TARGET) != 0
                && (to & D3D12_RESOURCE_STATE_RENDER_TARGET) == 0) {
                worldEnded = true;
            }
        }
    }

    if (worldEnded && boxesOn() && !g_teardown.load(std::memory_order_acquire)
        && !g_drewThisFrame.load(std::memory_order_relaxed)) {
        g_endThisFrame.fetch_add(1, std::memory_order_relaxed);
        const ListSlot& slot = slotFor(list);
        const bool bound = slot.key.load(std::memory_order_acquire) == list
                           && slot.dsv.load(std::memory_order_relaxed) != 0
                           && slot.numRt.load(std::memory_order_relaxed) > 0;
        if (bound && ensureGpu(list)) {
            if (g_drawWhenLogged.load(std::memory_order_relaxed) < 6) {
                g_drawWhenLogged.fetch_add(1, std::memory_order_relaxed);
            }
            drawInto(list);
            g_drewThisFrame.store(true, std::memory_order_relaxed);
            if (g_barrier != nullptr) {
                g_barrier(list, count, barriers);
            }
            return;
        }
        SIZE_T rtv = 0;
        SIZE_T dsv = 0;
        if (!pickCurrentTarget(rtv, dsv)) {
            rtv = g_busyRtv.load(std::memory_order_relaxed);
            dsv = g_busyDsv.load(std::memory_order_relaxed);
        }
        if (rtv != 0 && dsv != 0 && ensureGpu(list)) {
            if (g_drawWhenLogged.load(std::memory_order_relaxed) < 6) {
                g_drawWhenLogged.fetch_add(1, std::memory_order_relaxed);
            }
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{rtv};
            D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{dsv};
            if (g_om != nullptr) {
                g_om(list, 1, &rtvHandle, FALSE, &dsvHandle);
            }
            drawInto(list);
            g_drewThisFrame.store(true, std::memory_order_relaxed);
        }
    }
    if (g_barrier != nullptr) {
        g_barrier(list, count, barriers);
    }

    if (barriers != nullptr && !g_teardown.load(std::memory_order_acquire)) {
        for (UINT i = 0; i < count; ++i) {
            if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                continue;
            }
            atlas::noteBarrier(list, barriers[i].Transition.pResource,
                               static_cast<unsigned>(barriers[i].Transition.StateBefore),
                               static_cast<unsigned>(barriers[i].Transition.StateAfter),
                               static_cast<unsigned>(barriers[i].Transition.Subresource));
        }
    }

    if (!g_teardown.load(std::memory_order_acquire)) {
        atlas::bakePending(list);
    }
}

constexpr std::size_t kCloseIndex = 9;
using CloseFn = HRESULT(__stdcall*)(ID3D12GraphicsCommandList*);
CloseFn g_close = nullptr;
std::atomic<unsigned> g_closeThisFrame{0};
std::atomic<unsigned> g_closeLastFrame{0};

constexpr std::size_t kDrawIndexedInstancedIndex = 13;
constexpr std::size_t kDrawInstancedIndex = 12;

using DrawIndexedFn = void(__stdcall*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using DrawFn = void(__stdcall*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
DrawIndexedFn g_drawIndexed = nullptr;
DrawFn g_draw = nullptr;

struct DsvSlot {
    std::atomic<SIZE_T> dsv{0};
    std::atomic<SIZE_T> rtv{0};
    std::atomic<unsigned> hits{0};
};
constexpr std::size_t kDsvSlots = 16;
DsvSlot g_dsvs[kDsvSlots];

void noteDraw(ID3D12GraphicsCommandList* list)
{
    if (list == nullptr || g_teardown.load(std::memory_order_acquire) || !boxesOn()) {
        return;
    }
    const ListSlot& slot = slotFor(list);
    if (slot.key.load(std::memory_order_acquire) != list) {
        return;
    }
    const SIZE_T dsv = slot.dsv.load(std::memory_order_relaxed);
    if (dsv == 0 || slot.numRt.load(std::memory_order_relaxed) == 0) {
        return;
    }
    const SIZE_T rtv = slot.rtv.load(std::memory_order_relaxed);
    const std::size_t start = (dsv >> 4) % kDsvSlots;
    for (std::size_t i = 0; i < kDsvSlots; ++i) {
        DsvSlot& one = g_dsvs[(start + i) % kDsvSlots];
        SIZE_T was = one.dsv.load(std::memory_order_acquire);
        if (was == 0) {
            SIZE_T expected = 0;
            if (one.dsv.compare_exchange_strong(expected, dsv, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
                was = dsv;
            } else {
                was = expected;
            }
        }
        if (was == dsv) {
            one.rtv.store(rtv, std::memory_order_relaxed);
            one.hits.fetch_add(1, std::memory_order_relaxed);
            if (dsv == g_busyDsv.load(std::memory_order_relaxed)) {
                g_busyDrawsNow.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
    }
}

void __stdcall detourDrawIndexed(ID3D12GraphicsCommandList* list, UINT indexCount,
                                 UINT instanceCount, UINT startIndex, INT baseVertex,
                                 UINT startInstance)
{
    frametrace::onDraw(list, indexCount, instanceCount, true);
    noteDraw(list);
    ghost::beforeDraw(list);
    if (g_drawIndexed != nullptr) {
        g_drawIndexed(list, indexCount, instanceCount, startIndex, baseVertex, startInstance);
    }
}

void __stdcall detourDraw(ID3D12GraphicsCommandList* list, UINT vertexCount,
                          UINT instanceCount, UINT startVertex, UINT startInstance)
{
    frametrace::onDraw(list, vertexCount, instanceCount, false);
    noteDraw(list);
    ghost::beforeDraw(list);
    if (g_draw != nullptr) {
        g_draw(list, vertexCount, instanceCount, startVertex, startInstance);
    }
}

bool pickCurrentTarget(SIZE_T& rtv, SIZE_T& dsv)
{
    unsigned best = 0;
    for (const DsvSlot& one : g_dsvs) {
        const unsigned hits = one.hits.load(std::memory_order_relaxed);
        if (hits > best) {
            best = hits;
            dsv = one.dsv.load(std::memory_order_relaxed);
            rtv = one.rtv.load(std::memory_order_relaxed);
        }
    }
    return best != 0 && dsv != 0 && rtv != 0;
}

void pickBusyTarget()
{
    SIZE_T bestDsv = 0;
    SIZE_T bestRtv = 0;
    unsigned best = 0;
    SIZE_T nextDsv = 0;
    SIZE_T nextRtv = 0;
    unsigned next = 0;
    for (DsvSlot& one : g_dsvs) {
        const unsigned hits = one.hits.exchange(0, std::memory_order_relaxed);
        if (hits == 0) {
            continue;
        }
        const SIZE_T dsv = one.dsv.load(std::memory_order_relaxed);
        const SIZE_T rtv = one.rtv.load(std::memory_order_relaxed);
        if (hits > best) {
            next = best;  nextDsv = bestDsv;  nextRtv = bestRtv;
            best = hits;  bestDsv = dsv;      bestRtv = rtv;
        } else if (hits > next) {
            next = hits;  nextDsv = dsv;      nextRtv = rtv;
        }
    }
    if (best != 0) {
        g_busyHits.store(best, std::memory_order_relaxed);
        g_busyRtv.store(bestRtv, std::memory_order_relaxed);
        g_busyDsv.store(bestDsv, std::memory_order_relaxed);
    }
}

void drawBeforeDepthClear(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE dsv)
{
    if (g_drewThisFrame.load(std::memory_order_relaxed)) {
        return;
    }
    const SIZE_T rtv = g_busyRtv.load(std::memory_order_relaxed);
    const SIZE_T busy = g_busyDsv.load(std::memory_order_relaxed);
    if (rtv == 0 || busy == 0 || !ensureGpu(list)) {
        return;
    }
    (void)dsv;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{rtv};
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{busy};
    if (g_om != nullptr) {
        g_om(list, 1, &rtvHandle, FALSE, &dsvHandle);
    }
    drawInto(list);
    g_drewThisFrame.store(true, std::memory_order_relaxed);
}

HRESULT __stdcall detourClose(ID3D12GraphicsCommandList* list)
{
    frametrace::onClose(list);
    if (list != nullptr && !g_teardown.load(std::memory_order_acquire) && boxesOn()) {
        ListSlot& slot = slotFor(list);
        const SIZE_T world = g_worldDsv.load(std::memory_order_relaxed);
        if (slot.key.load(std::memory_order_acquire) == list && world != 0
            && slot.dsv.load(std::memory_order_relaxed) == world
            && slot.numRt.load(std::memory_order_relaxed) > 0) {
            g_closeThisFrame.fetch_add(1, std::memory_order_relaxed);
            slot.world.store(false, std::memory_order_relaxed);
            slot.dsv.store(0, std::memory_order_relaxed);
            slot.numRt.store(0, std::memory_order_relaxed);
        }
    }
    return g_close != nullptr ? g_close(list) : S_OK;
}

constexpr std::size_t kRsSetViewportsIndex = 21;
using RsSetViewportsFn = void(__stdcall*)(ID3D12GraphicsCommandList*, UINT,
                                          const D3D12_VIEWPORT*);
RsSetViewportsFn g_rsSetViewports = nullptr;

void __stdcall detourRsSetViewports(ID3D12GraphicsCommandList* list, UINT count,
                                    const D3D12_VIEWPORT* viewports)
{
    if (count > 0 && viewports != nullptr) {
        frametrace::onViewport(list, viewports[0].Width, viewports[0].Height,
                               viewports[0].MinDepth, viewports[0].MaxDepth);
    }
    if (list != nullptr && count > 0 && viewports != nullptr
        && !g_teardown.load(std::memory_order_acquire)) {
        const ListSlot& slot = slotFor(list);
        const SIZE_T world = g_worldDsv.load(std::memory_order_relaxed);
        if (slot.key.load(std::memory_order_acquire) == list && world != 0
            && slot.dsv.load(std::memory_order_relaxed) == world
            && viewports[0].Width > 16.0F && viewports[0].Height > 16.0F) {
            g_vpTopLeftX.store(viewports[0].TopLeftX, std::memory_order_relaxed);
            g_vpTopLeftY.store(viewports[0].TopLeftY, std::memory_order_relaxed);
            g_vpWidth.store(viewports[0].Width, std::memory_order_relaxed);
            g_vpHeight.store(viewports[0].Height, std::memory_order_relaxed);
            g_vpMinDepth.store(viewports[0].MinDepth, std::memory_order_relaxed);
            g_vpMaxDepth.store(viewports[0].MaxDepth, std::memory_order_relaxed);
            g_haveViewport.store(true, std::memory_order_release);
        }
    }
    if (g_rsSetViewports != nullptr) {
        g_rsSetViewports(list, count, viewports);
    }
}

constexpr std::size_t kClearDepthStencilViewIndex = 47;
using ClearDsvFn = void(__stdcall*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE,
                                    D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT,
                                    const D3D12_RECT*);
ClearDsvFn g_clearDsv = nullptr;

void __stdcall detourClearDsv(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                              D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT rects,
                              const D3D12_RECT* rect)
{
    frametrace::onClearDsv(list, static_cast<std::uint64_t>(dsv.ptr),
                           static_cast<unsigned>(flags));
    if (!g_teardown.load(std::memory_order_acquire)) {
        const unsigned index = g_clearThisFrame.fetch_add(1, std::memory_order_relaxed) + 1;
        g_frameClears.fetch_add(1, std::memory_order_relaxed);
        if (index == 1) {
            g_worldDsv.store(dsv.ptr, std::memory_order_relaxed);
            g_guessDepth.store((flags & D3D12_CLEAR_FLAG_STENCIL) != 0 ? 45u : 40u,
                               std::memory_order_relaxed);
        } else if (false) {
            drawBeforeDepthClear(list, dsv);
        }
        if (g_busyDrawsNow.load(std::memory_order_relaxed) > 10
            && g_clearLogged.load(std::memory_order_relaxed) < 30) {
            g_clearLogged.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (g_clearDsv != nullptr) {
        g_clearDsv(list, dsv, flags, depth, stencil, rects, rect);
    }
}

constexpr std::size_t kBeginRenderPassIndex = 68;
constexpr std::size_t kEndRenderPassIndex = 69;

using BeginRenderPassFn = void(__stdcall*)(ID3D12GraphicsCommandList4*, UINT,
                                           const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
                                           const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,
                                           D3D12_RENDER_PASS_FLAGS);
using EndRenderPassFn = void(__stdcall*)(ID3D12GraphicsCommandList4*);
BeginRenderPassFn g_beginRenderPass = nullptr;
EndRenderPassFn g_endRenderPass = nullptr;

std::atomic<unsigned> g_renderPassLogged{0};
constexpr std::size_t kPassSlots2 = 64;
struct PassOpen {
    std::atomic<void*> key{nullptr};
    std::atomic<bool> world{false};
};
PassOpen g_passOpen[kPassSlots2];

PassOpen& passSlotFor(void* list)
{
    return g_passOpen[(reinterpret_cast<std::uintptr_t>(list) >> 4) % kPassSlots2];
}

void __stdcall detourBeginRenderPass(ID3D12GraphicsCommandList4* list, UINT numRt,
                                     const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rts,
                                     const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds,
                                     D3D12_RENDER_PASS_FLAGS flags)
{
    if (list != nullptr && !g_teardown.load(std::memory_order_acquire)) {
        const bool world = numRt > 0 && rts != nullptr && ds != nullptr;
        PassOpen& slot = passSlotFor(list);
        slot.world.store(world, std::memory_order_relaxed);
        slot.key.store(list, std::memory_order_release);
        if (world) {
            g_lastDsv.store(ds->cpuDescriptor.ptr, std::memory_order_relaxed);
        }
        if (g_renderPassLogged.load(std::memory_order_relaxed) < 8) {
            g_renderPassLogged.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (g_beginRenderPass != nullptr) {
        g_beginRenderPass(list, numRt, rts, ds, flags);
    }
}

void __stdcall detourEndRenderPass(ID3D12GraphicsCommandList4* list)
{
    if (list != nullptr && !g_teardown.load(std::memory_order_acquire) && boxesOn()
        && !g_drewThisFrame.load(std::memory_order_relaxed)) {
        PassOpen& slot = passSlotFor(list);
        if (slot.key.load(std::memory_order_acquire) == list
            && slot.world.load(std::memory_order_relaxed) && ensureGpu(list)) {
            drawInto(list);
            g_drewThisFrame.store(true, std::memory_order_relaxed);
            if (g_drawWhenLogged.load(std::memory_order_relaxed) < 6) {
                g_drawWhenLogged.fetch_add(1, std::memory_order_relaxed);
            }
        }
        slot.world.store(false, std::memory_order_relaxed);
    }
    if (g_endRenderPass != nullptr) {
        g_endRenderPass(list);
    }
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

}

void noteGamePso(const void* graphicsPipelineStateDesc)
{
    if (graphicsPipelineStateDesc == nullptr) {
        return;
    }
    const auto& desc =
        *static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(graphicsPipelineStateDesc);

    {
        const unsigned long long key =
            (static_cast<unsigned long long>(desc.NumRenderTargets) << 56)
            ^ (static_cast<unsigned long long>(desc.RTVFormats[0]) << 40)
            ^ (static_cast<unsigned long long>(desc.DSVFormat) << 24)
            ^ static_cast<unsigned long long>(desc.SampleDesc.Count);
        const std::size_t start = static_cast<std::size_t>(key % kPsoSlots);
        for (std::size_t i = 0; i < kPsoSlots; ++i) {
            PsoSlot& slot = g_psoKinds[(start + i) % kPsoSlots];
            unsigned long long was = slot.key.load(std::memory_order_acquire);
            if (was == 0) {
                unsigned long long expected = 0;
                if (slot.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
                    slot.numRt.store(desc.NumRenderTargets, std::memory_order_relaxed);
                    slot.rt0.store(static_cast<unsigned>(desc.RTVFormats[0]),
                                   std::memory_order_relaxed);
                    slot.dsv.store(static_cast<unsigned>(desc.DSVFormat),
                                   std::memory_order_relaxed);
                    slot.samples.store(desc.SampleDesc.Count, std::memory_order_relaxed);
                    was = key;
                } else {
                    was = expected;
                }
            }
            if (was == key) {
                slot.hits.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }

    if (g_formats.have.load(std::memory_order_acquire)) {
        return;
    }
    if (desc.NumRenderTargets == 0 || desc.NumRenderTargets > 8
        || desc.DSVFormat == DXGI_FORMAT_UNKNOWN
        || desc.RTVFormats[0] == DXGI_FORMAT_UNKNOWN) {
        return;
    }
    if (const unsigned want = g_guessSamples.load(std::memory_order_relaxed);
        want != 0 && desc.SampleDesc.Count != want) {
        return;
    }
    if (const unsigned want = g_guessColor.load(std::memory_order_relaxed);
        want != 0 && static_cast<unsigned>(desc.RTVFormats[0]) != want) {
        return;
    }
    for (unsigned i = 0; i < desc.NumRenderTargets; ++i) {
        g_formats.rt[i].store(static_cast<unsigned>(desc.RTVFormats[i]),
                              std::memory_order_relaxed);
    }
    g_formats.numRt.store(desc.NumRenderTargets, std::memory_order_relaxed);
    g_formats.dsv.store(static_cast<unsigned>(desc.DSVFormat), std::memory_order_relaxed);
    g_formats.sampleCount.store(desc.SampleDesc.Count == 0 ? 1 : desc.SampleDesc.Count,
                                std::memory_order_relaxed);
    g_formats.sampleQuality.store(desc.SampleDesc.Quality, std::memory_order_relaxed);
    g_formats.have.store(true, std::memory_order_release);
}

void onPresent()
{
    const unsigned count = g_departThisFrame.exchange(0, std::memory_order_relaxed);
    if (count != 0) {
        g_drawAt.store(count, std::memory_order_relaxed);
    } else {
        g_drawAt.store(1, std::memory_order_relaxed);
    }
    const unsigned binds = g_bindThisFrame.exchange(0, std::memory_order_relaxed);
    g_bindAt.store(binds != 0 ? binds : 1, std::memory_order_relaxed);
    g_closeLastFrame.store(g_closeThisFrame.exchange(0, std::memory_order_relaxed),
                           std::memory_order_relaxed);
    g_endLastFrame.store(g_endThisFrame.exchange(0, std::memory_order_relaxed),
                         std::memory_order_relaxed);
    g_clearLastFrame.store(g_clearThisFrame.exchange(0, std::memory_order_relaxed),
                           std::memory_order_relaxed);
    g_drewThisFrame.store(false, std::memory_order_relaxed);
    g_frameClears.store(0, std::memory_order_relaxed);
    g_frameWorldRtv.store(0, std::memory_order_relaxed);
    g_frameWorldDsv.store(0, std::memory_order_relaxed);
    pickBusyTarget();
    g_busyDrawsNow.store(0, std::memory_order_relaxed);
}

bool depthReady()
{
    return g_ready.load(std::memory_order_acquire) && !g_teardown.load(std::memory_order_acquire);
}

bool installDepthHooks()
{
    std::error_code ec;
    const auto path = paths::dataDir() / L"diag-depth.txt";
    if (std::filesystem::exists(path, ec)) {
        std::string text;
        if (FILE* file = nullptr; _wfopen_s(&file, path.c_str(), L"rb") == 0 && file != nullptr) {
            char buffer[32] = {};
            const std::size_t got = std::fread(buffer, 1, sizeof(buffer) - 1, file);
            std::fclose(file);
            text.assign(buffer, got);
        }
        if (text.find("greater") != std::string::npos) {
            g_depthFunc.store(static_cast<int>(D3D12_COMPARISON_FUNC_GREATER_EQUAL),
                              std::memory_order_relaxed);
        } else if (text.rfind("at", 0) == 0 && text.size() >= 3) {
            unsigned pct = 0;
            for (std::size_t i = 2; i < text.size(); ++i) {
                if (text[i] < '0' || text[i] > '9') {
                    break;
                }
                pct = pct * 10 + static_cast<unsigned>(text[i] - '0');
            }
            if (pct > 0 && pct <= 100) {
                g_drawAtPercent.store(pct, std::memory_order_relaxed);
            }
        } else if (text.find("alt") != std::string::npos) {
            g_useAlt.store(true, std::memory_order_relaxed);
        } else if (text.find("d32s8") != std::string::npos) {
            g_forceDepthFormat.store(20u, std::memory_order_relaxed);
        } else if (text.find("d32") != std::string::npos) {
            g_forceDepthFormat.store(40u, std::memory_order_relaxed);
        } else if (text.find("d16") != std::string::npos) {
            g_forceDepthFormat.store(55u, std::memory_order_relaxed);
        } else if (text.find("never") != std::string::npos) {
            g_depthFunc.store(static_cast<int>(D3D12_COMPARISON_FUNC_NEVER),
                              std::memory_order_relaxed);
        }
    }

    const HMODULE d3d12Module = GetModuleHandleW(L"d3d12.dll");
    if (d3d12Module == nullptr) {
        return false;
    }
    const auto createDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
        GetProcAddress(d3d12Module, "D3D12CreateDevice"));
    if (createDevice == nullptr) {
        return false;
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        return false;
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator)))) {
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                         nullptr, IID_PPV_ARGS(&list)))) {
        return false;
    }
    list->Close();

    void* const target = vtableEntry(list.Get(), kOmSetRenderTargetsIndex);
    if (target == nullptr) {
        return false;
    }
    HookManager& hooks = HookManager::instance();
    bool ok = hooks.create(target, &detourOm, reinterpret_cast<void**>(&g_om),
                           L"D3D12OMSetRenderTargets");
    if (void* const barrier = vtableEntry(list.Get(), kResourceBarrierIndex);
        barrier != nullptr) {
        ok = hooks.create(barrier, &detourBarrier, reinterpret_cast<void**>(&g_barrier),
                          L"D3D12ResourceBarrier")
             && ok;
    }
    if (void* const di = vtableEntry(list.Get(), kDrawIndexedInstancedIndex); di != nullptr) {
        ok = hooks.create(di, &detourDrawIndexed, reinterpret_cast<void**>(&g_drawIndexed),
                          L"D3D12DrawIndexedInstanced")
             && ok;
    }
    if (void* const d = vtableEntry(list.Get(), kDrawInstancedIndex); d != nullptr) {
        ok = hooks.create(d, &detourDraw, reinterpret_cast<void**>(&g_draw),
                          L"D3D12DrawInstanced")
             && ok;
    }
    if (void* const vp = vtableEntry(list.Get(), kRsSetViewportsIndex); vp != nullptr) {
        ok = hooks.create(vp, &detourRsSetViewports,
                          reinterpret_cast<void**>(&g_rsSetViewports), L"D3D12RSSetViewports")
             && ok;
    }
    if (void* const close = vtableEntry(list.Get(), kCloseIndex); close != nullptr) {
        ok = hooks.create(close, &detourClose, reinterpret_cast<void**>(&g_close),
                          L"D3D12CommandListClose")
             && ok;
    }
    if (void* const clear = vtableEntry(list.Get(), kClearDepthStencilViewIndex);
        clear != nullptr) {
        ok = hooks.create(clear, &detourClearDsv, reinterpret_cast<void**>(&g_clearDsv),
                          L"D3D12ClearDepthStencilView")
             && ok;
    }
    if (ok) {
        log().info(L"BoxDepth: depth-aware drawing hooked (OMSetRenderTargets {:#x})",
                   reinterpret_cast<std::uintptr_t>(target));
    }
    return ok;
}

void shutdownDepth()
{
    g_teardown.store(true, std::memory_order_release);
    g_ready.store(false, std::memory_order_release);
}

}
