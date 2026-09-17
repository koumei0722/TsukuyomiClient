#include "render/FrameTrace.h"

#include "core/Logger.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "hooks/HookManager.h"

#include <Windows.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tsukuyomi::frametrace {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int kIdle = 0;
constexpr int kArmed = 1;
constexpr int kRecording = 2;
constexpr int kDumping = 3;
std::atomic<int> g_state{kIdle};
std::atomic<int> g_framesLeft{0};
std::atomic<bool> g_teardown{false};
constexpr std::size_t kMaxEvents = 400000;
constexpr int kFrames = 2;

enum Kind : std::uint16_t {
    kReset = 1,
    kClose,
    kSetPso,
    kOm,
    kClearDsv,
    kClearRtv,
    kDraw,
    kBarrier,
    kResolve,
    kExec,
    kPresent,
    kMarker,
    kBeginEvent,
    kEndEvent,
    kIndirect,
    kViewport,
    kOurs,
};

struct Event {
    std::uint64_t seq = 0;
    std::uint32_t tid = 0;
    std::uint16_t kind = 0;
    const void* list = nullptr;
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::uint64_t c = 0;
    std::uint64_t d = 0;
    std::string text;
};

std::mutex g_mu;
std::vector<Event> g_events;
std::atomic<std::uint64_t> g_seq{0};
std::atomic<std::uint64_t> g_dropped{0};
int g_dumpIndex = 0;

struct PsoInfo {
    std::uint32_t id = 0;
    unsigned numRt = 0;
    unsigned rt0 = 0;
    unsigned dsv = 0;
    unsigned samples = 0;
    unsigned depthEnable = 0;
    unsigned depthWrite = 0;
    unsigned depthFunc = 0;
    unsigned stencil = 0;
    unsigned blendEnable = 0;
    unsigned srcBlend = 0;
    unsigned dstBlend = 0;
    unsigned blendOp = 0;
    unsigned writeMask = 0;
    unsigned cull = 0;
    int depthBias = 0;
    float slopeBias = 0.0F;
    unsigned topology = 0;
    std::string inputs;
    std::uint64_t vsHash = 0;
    std::uint64_t psHash = 0;
    std::uint32_t vsSize = 0;
    std::uint32_t psSize = 0;
};
std::mutex g_psoMu;
std::unordered_map<const void*, PsoInfo> g_psos;
std::uint32_t g_psoNext = 1;

std::uint64_t fnv(const void* data, std::size_t size)
{
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

void push(Event&& e)
{
    if (g_state.load(std::memory_order_acquire) != kRecording) {
        return;
    }
    e.seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    e.tid = GetCurrentThreadId();
    const std::lock_guard<std::mutex> lock(g_mu);
    if (g_events.size() >= kMaxEvents) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_events.push_back(std::move(e));
}

Event make(std::uint16_t kind, const void* list, std::uint64_t a = 0, std::uint64_t b = 0,
           std::uint64_t c = 0, std::uint64_t d = 0)
{
    Event e;
    e.kind = kind;
    e.list = list;
    e.a = a;
    e.b = b;
    e.c = c;
    e.d = d;
    return e;
}

constexpr std::size_t kResetIndex = 10;
constexpr std::size_t kResolveIndex = 19;
constexpr std::size_t kClearRtvIndex = 48;
constexpr std::size_t kSetMarkerIndex = 56;
constexpr std::size_t kBeginEventIndex = 57;
constexpr std::size_t kEndEventIndex = 58;
constexpr std::size_t kExecuteIndirectIndex = 59;

using ResetFn = HRESULT(__stdcall*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*,
                                    ID3D12PipelineState*);
using ResolveFn = void(__stdcall*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT,
                                   ID3D12Resource*, UINT, DXGI_FORMAT);
using ClearRtvFn = void(__stdcall*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE,
                                    const FLOAT[4], UINT, const D3D12_RECT*);
using MarkerFn = void(__stdcall*)(ID3D12GraphicsCommandList*, UINT, const void*, UINT);
using EndEventFn = void(__stdcall*)(ID3D12GraphicsCommandList*);
using ExecuteIndirectFn = void(__stdcall*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*,
                                           UINT, ID3D12Resource*, UINT64, ID3D12Resource*,
                                           UINT64);
ResetFn g_reset = nullptr;
ResolveFn g_resolve = nullptr;
ClearRtvFn g_clearRtv = nullptr;
MarkerFn g_setMarker = nullptr;
MarkerFn g_beginEvent = nullptr;
EndEventFn g_endEvent = nullptr;
ExecuteIndirectFn g_executeIndirect = nullptr;

std::string markerText(UINT metadata, const void* data, UINT size)
{
    if (data == nullptr || size == 0 || size > 4096) {
        return {};
    }
    std::string out;
    const auto* bytes = static_cast<const unsigned char*>(data);
    if (metadata == 0) {
        const auto* w = static_cast<const wchar_t*>(data);
        const std::size_t n = size / sizeof(wchar_t);
        for (std::size_t i = 0; i < n && w[i] != 0 && out.size() < 96; ++i) {
            out.push_back((w[i] >= 0x20 && w[i] < 0x7F) ? static_cast<char>(w[i]) : '?');
        }
        return out;
    }
    for (UINT i = 0; i < size && out.size() < 96; ++i) {
        const unsigned char ch = bytes[i];
        if (ch >= 0x20 && ch < 0x7F) {
            out.push_back(static_cast<char>(ch));
        } else if (!out.empty() && out.back() != ' ') {
            out.push_back(' ');
        }
    }
    return out;
}

HRESULT __stdcall detourReset(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator,
                              ID3D12PipelineState* pso)
{
    if (g_state.load(std::memory_order_relaxed) == kRecording) {
        push(make(kReset, list, reinterpret_cast<std::uint64_t>(pso),
                  static_cast<std::uint64_t>(list != nullptr ? list->GetType() : 0)));
    }
    return g_reset != nullptr ? g_reset(list, allocator, pso) : E_FAIL;
}

void __stdcall detourResolve(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, UINT dstSub,
                             ID3D12Resource* src, UINT srcSub, DXGI_FORMAT format)
{
    if (g_state.load(std::memory_order_relaxed) == kRecording) {
        push(make(kResolve, list, reinterpret_cast<std::uint64_t>(dst),
                  reinterpret_cast<std::uint64_t>(src), static_cast<std::uint64_t>(format),
                  (static_cast<std::uint64_t>(dstSub) << 32) | srcSub));
    }
    if (g_resolve != nullptr) {
        g_resolve(list, dst, dstSub, src, srcSub, format);
    }
}

void __stdcall detourClearRtv(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                              const FLOAT color[4], UINT rects, const D3D12_RECT* rect)
{
    if (g_state.load(std::memory_order_relaxed) == kRecording) {
        std::uint64_t packed = 0;
        if (color != nullptr) {
            const auto q = [](float v) {
                return static_cast<std::uint64_t>(v < 0.0F ? 0 : (v > 1.0F ? 255 : v * 255.0F));
            };
            packed = q(color[0]) | (q(color[1]) << 8) | (q(color[2]) << 16) | (q(color[3]) << 24);
        }
        push(make(kClearRtv, list, rtv.ptr, packed, rects));
    }
    if (g_clearRtv != nullptr) {
        g_clearRtv(list, rtv, color, rects, rect);
    }
}

void __stdcall detourSetMarker(ID3D12GraphicsCommandList* list, UINT metadata, const void* data,
                               UINT size)
{
    if (g_state.load(std::memory_order_relaxed) == kRecording) {
        Event e = make(kMarker, list, metadata, size);
        e.text = markerText(metadata, data, size);
        push(std::move(e));
    }
    if (g_setMarker != nullptr) {
        g_setMarker(list, metadata, data, size);
    }
}

void __stdcall detourBeginEvent(ID3D12GraphicsCommandList* list, UINT metadata, const void* data,
                                UINT size)
{
    if (g_state.load(std::memory_order_relaxed) == kRecording) {
        Event e = make(kBeginEvent, list, metadata, size);
        e.text = markerText(metadata, data, size);
        push(std::move(e));
    }
    if (g_beginEvent != nullptr) {
        g_beginEvent(list, metadata, data, size);
    }
}

void __stdcall detourEndEvent(ID3D12GraphicsCommandList* list)
{
    if (g_state.load(std::memory_order_relaxed) == kRecording) {
        push(make(kEndEvent, list));
    }
    if (g_endEvent != nullptr) {
        g_endEvent(list);
    }
}

void __stdcall detourExecuteIndirect(ID3D12GraphicsCommandList* list,
                                     ID3D12CommandSignature* signature, UINT maxCount,
                                     ID3D12Resource* args, UINT64 argsOffset,
                                     ID3D12Resource* countBuffer, UINT64 countOffset)
{
    if (g_state.load(std::memory_order_relaxed) == kRecording) {
        push(make(kIndirect, list, maxCount, reinterpret_cast<std::uint64_t>(signature),
                  reinterpret_cast<std::uint64_t>(countBuffer)));
    }
    if (g_executeIndirect != nullptr) {
        g_executeIndirect(list, signature, maxCount, args, argsOffset, countBuffer, countOffset);
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

const char* kindName(std::uint16_t kind)
{
    switch (kind) {
    case kReset: return "RESET";
    case kClose: return "CLOSE";
    case kSetPso: return "PSO";
    case kOm: return "OM";
    case kClearDsv: return "CLRD";
    case kClearRtv: return "CLRR";
    case kDraw: return "DRAW";
    case kBarrier: return "BAR";
    case kResolve: return "RESOLVE";
    case kExec: return "EXEC";
    case kPresent: return "PRESENT";
    case kMarker: return "MARK";
    case kBeginEvent: return "BEGIN";
    case kEndEvent: return "END";
    case kIndirect: return "INDIRECT";
    case kViewport: return "VP";
    case kOurs: return "OURS";
    default: return "?";
    }
}

void dump()
{
    std::vector<Event> events;
    {
        const std::lock_guard<std::mutex> lock(g_mu);
        events.swap(g_events);
    }
    std::unordered_map<const void*, PsoInfo> psos;
    {
        const std::lock_guard<std::mutex> lock(g_psoMu);
        psos = g_psos;
    }
    ++g_dumpIndex;
    const auto path = paths::dataDir() / (L"frametrace-" + std::to_wstring(g_dumpIndex) + L".txt");
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
        log().warn(L"FrameTrace: could not write the frame dump ({})", path.wstring());
        return;
    }
    std::fprintf(file, "# frametrace %d / events %zu / dropped %llu / psos %zu\n", g_dumpIndex,
                 events.size(),
                 static_cast<unsigned long long>(g_dropped.load(std::memory_order_relaxed)),
                 psos.size());
    std::fprintf(file, "# P id ptr rt=N:fmt dsv=fmt s=samples dep=en/wr/func st=stencil "
                       "bl=en/src/dst/op/mask cull bias/slope topo in=... vs=hash/size ps=hash/size\n");
    for (const auto& [ptr, info] : psos) {
        std::fprintf(file,
                     "P %u %p rt=%u:%u dsv=%u s=%u dep=%u/%u/%u st=%u bl=%u/%u/%u/%u/%x cull=%u "
                     "bias=%d/%.2f topo=%u in=%s vs=%016llx/%u ps=%016llx/%u\n",
                     info.id, ptr, info.numRt, info.rt0, info.dsv, info.samples, info.depthEnable,
                     info.depthWrite, info.depthFunc, info.stencil, info.blendEnable, info.srcBlend,
                     info.dstBlend, info.blendOp, info.writeMask, info.cull, info.depthBias,
                     info.slopeBias, info.topology, info.inputs.c_str(),
                     static_cast<unsigned long long>(info.vsHash), info.vsSize,
                     static_cast<unsigned long long>(info.psHash), info.psSize);
    }
    std::fprintf(file, "# E seq tid list KIND a b c d [text]   (for a PSO, a is the id; 0 = created before injection)\n");
    for (const Event& e : events) {
        std::uint64_t a = e.a;
        if (e.kind == kSetPso) {
            const auto it = psos.find(reinterpret_cast<const void*>(e.a));
            a = (it != psos.end()) ? it->second.id : 0;
        }
        std::fprintf(file, "E %llu %u %p %s %llx %llx %llx %llx%s%s\n",
                     static_cast<unsigned long long>(e.seq), e.tid, e.list, kindName(e.kind),
                     static_cast<unsigned long long>(a), static_cast<unsigned long long>(e.b),
                     static_cast<unsigned long long>(e.c), static_cast<unsigned long long>(e.d),
                     e.text.empty() ? "" : " ", e.text.c_str());
    }
    std::fclose(file);
    log().info(L"FrameTrace: wrote {} events (dropped {} / PSOs {}) -> {}",
               events.size(),
               g_dropped.load(std::memory_order_relaxed),
               psos.size(),
               path.wstring());
}

std::filesystem::file_time_type g_lastTrigger{};
bool g_haveTrigger = false;
unsigned long long g_lastCheck = 0;

}

bool installHooks()
{
    const HMODULE d3d12Module = GetModuleHandleW(L"d3d12.dll");
    if (d3d12Module == nullptr) {
        return false;
    }
    const auto createDevice =
        reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12Module, "D3D12CreateDevice"));
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
    HookManager& hooks = HookManager::instance();
    bool ok = true;
    const auto hook = [&](std::size_t index, void* detour, void** original, const wchar_t* name) {
        void* const target = vtableEntry(list.Get(), index);
        if (target == nullptr || !hooks.create(target, detour, original, name)) {
            ok = false;
        }
    };
    hook(kResetIndex, reinterpret_cast<void*>(&detourReset), reinterpret_cast<void**>(&g_reset),
         L"D3D12CommandListReset");
    hook(kResolveIndex, reinterpret_cast<void*>(&detourResolve),
         reinterpret_cast<void**>(&g_resolve), L"D3D12ResolveSubresource");
    hook(kClearRtvIndex, reinterpret_cast<void*>(&detourClearRtv),
         reinterpret_cast<void**>(&g_clearRtv), L"D3D12ClearRenderTargetView");
    hook(kSetMarkerIndex, reinterpret_cast<void*>(&detourSetMarker),
         reinterpret_cast<void**>(&g_setMarker), L"D3D12SetMarker");
    hook(kBeginEventIndex, reinterpret_cast<void*>(&detourBeginEvent),
         reinterpret_cast<void**>(&g_beginEvent), L"D3D12BeginEvent");
    hook(kEndEventIndex, reinterpret_cast<void*>(&detourEndEvent),
         reinterpret_cast<void**>(&g_endEvent), L"D3D12EndEvent");
    hook(kExecuteIndirectIndex, reinterpret_cast<void*>(&detourExecuteIndirect),
         reinterpret_cast<void**>(&g_executeIndirect), L"D3D12ExecuteIndirect");
    log().info(L"FrameTrace: hooks installed ({})", ok ? L"all" : L"partly failed");
    return ok;
}

bool recording()
{
    return g_state.load(std::memory_order_relaxed) == kRecording;
}

void notePso(ID3D12PipelineState* pso, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc)
{
    if (pso == nullptr || desc == nullptr || g_teardown.load(std::memory_order_relaxed)) {
        return;
    }
    PsoInfo info;
    info.numRt = desc->NumRenderTargets;
    info.rt0 = static_cast<unsigned>(desc->RTVFormats[0]);
    info.dsv = static_cast<unsigned>(desc->DSVFormat);
    info.samples = desc->SampleDesc.Count;
    info.depthEnable = desc->DepthStencilState.DepthEnable ? 1u : 0u;
    info.depthWrite = static_cast<unsigned>(desc->DepthStencilState.DepthWriteMask);
    info.depthFunc = static_cast<unsigned>(desc->DepthStencilState.DepthFunc);
    info.stencil = desc->DepthStencilState.StencilEnable ? 1u : 0u;
    const D3D12_RENDER_TARGET_BLEND_DESC& b = desc->BlendState.RenderTarget[0];
    info.blendEnable = b.BlendEnable ? 1u : 0u;
    info.srcBlend = static_cast<unsigned>(b.SrcBlend);
    info.dstBlend = static_cast<unsigned>(b.DestBlend);
    info.blendOp = static_cast<unsigned>(b.BlendOp);
    info.writeMask = b.RenderTargetWriteMask;
    info.cull = static_cast<unsigned>(desc->RasterizerState.CullMode);
    info.depthBias = desc->RasterizerState.DepthBias;
    info.slopeBias = desc->RasterizerState.SlopeScaledDepthBias;
    info.topology = static_cast<unsigned>(desc->PrimitiveTopologyType);
    for (UINT i = 0; i < desc->InputLayout.NumElements && i < 12; ++i) {
        const D3D12_INPUT_ELEMENT_DESC& one = desc->InputLayout.pInputElementDescs[i];
        if (!info.inputs.empty()) {
            info.inputs += ',';
        }
        info.inputs += (one.SemanticName != nullptr) ? one.SemanticName : "?";
        info.inputs += std::to_string(one.SemanticIndex);
        info.inputs += ':';
        info.inputs += std::to_string(static_cast<unsigned>(one.Format));
    }
    if (desc->VS.pShaderBytecode != nullptr && desc->VS.BytecodeLength < (1u << 24)) {
        info.vsHash = fnv(desc->VS.pShaderBytecode, desc->VS.BytecodeLength);
        info.vsSize = static_cast<std::uint32_t>(desc->VS.BytecodeLength);
    }
    if (desc->PS.pShaderBytecode != nullptr && desc->PS.BytecodeLength < (1u << 24)) {
        info.psHash = fnv(desc->PS.pShaderBytecode, desc->PS.BytecodeLength);
        info.psSize = static_cast<std::uint32_t>(desc->PS.BytecodeLength);
    }
    const std::lock_guard<std::mutex> lock(g_psoMu);
    if (g_psos.size() >= 8192) {
        return;
    }
    info.id = g_psoNext++;
    g_psos[pso] = std::move(info);
}

void onSetPso(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, bool swapped)
{
    if (g_state.load(std::memory_order_relaxed) == kRecording) {
        push(make(kSetPso, list, reinterpret_cast<std::uint64_t>(pso), swapped ? 1 : 0));
    }
}

void onOm(ID3D12GraphicsCommandList* list, unsigned numRt, std::uint64_t rtv0, std::uint64_t dsv)
{
    if (g_state.load(std::memory_order_relaxed) == kRecording) {
        push(make(kOm, list, numRt, rtv0, dsv));
    }
}

void onDraw(ID3D12GraphicsCommandList* list, unsigned count, unsigned instances, bool indexed)
{
    if (g_state.load(std::memory_order_relaxed) == kRecording) {
        push(make(kDraw, list, count, instances, indexed ? 1 : 0));
    }
}

void onClearDsv(ID3D12GraphicsCommandList* list, std::uint64_t dsv, unsigned flags)
{
    if (g_state.load(std::memory_order_relaxed) == kRecording) {
        push(make(kClearDsv, list, dsv, flags));
    }
}

void onBarrier(ID3D12GraphicsCommandList* list, unsigned count,
               const D3D12_RESOURCE_BARRIER* barriers)
{
    if (g_state.load(std::memory_order_relaxed) != kRecording || barriers == nullptr) {
        return;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            continue;
        }
        const auto& t = barriers[i].Transition;
        constexpr UINT kInteresting = D3D12_RESOURCE_STATE_RENDER_TARGET
                                      | D3D12_RESOURCE_STATE_DEPTH_WRITE
                                      | D3D12_RESOURCE_STATE_DEPTH_READ
                                      | D3D12_RESOURCE_STATE_RESOLVE_DEST
                                      | D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        const UINT both = static_cast<UINT>(t.StateBefore) | static_cast<UINT>(t.StateAfter);
        if ((both & kInteresting) == 0 && t.StateAfter != D3D12_RESOURCE_STATE_PRESENT) {
            continue;
        }
        std::uint64_t shape = 0;
        if (t.pResource != nullptr) {
            const D3D12_RESOURCE_DESC d = t.pResource->GetDesc();
            shape = (static_cast<std::uint64_t>(d.Width) & 0xFFFF)
                    | ((static_cast<std::uint64_t>(d.Height) & 0xFFFF) << 16)
                    | ((static_cast<std::uint64_t>(d.Format) & 0xFF) << 32)
                    | ((static_cast<std::uint64_t>(d.SampleDesc.Count) & 0xFF) << 40);
        }
        push(make(kBarrier, list, reinterpret_cast<std::uint64_t>(t.pResource),
                  static_cast<std::uint64_t>(t.StateBefore), static_cast<std::uint64_t>(t.StateAfter),
                  shape));
    }
}

void onClose(ID3D12GraphicsCommandList* list)
{
    if (g_state.load(std::memory_order_relaxed) == kRecording) {
        push(make(kClose, list));
    }
}

void onViewport(ID3D12GraphicsCommandList* list, float width, float height, float minDepth,
                float maxDepth)
{
    if (g_state.load(std::memory_order_relaxed) == kRecording) {
        std::uint32_t mn = 0;
        std::uint32_t mx = 0;
        std::memcpy(&mn, &minDepth, sizeof(mn));
        std::memcpy(&mx, &maxDepth, sizeof(mx));
        push(make(kViewport, list, static_cast<std::uint64_t>(width),
                  static_cast<std::uint64_t>(height), mn, mx));
    }
}

void onOurs(ID3D12GraphicsCommandList* list, const char* tag)
{
    if (g_state.load(std::memory_order_relaxed) == kRecording) {
        Event e = make(kOurs, list);
        e.text = (tag != nullptr) ? tag : "";
        push(std::move(e));
    }
}

void onExecute(ID3D12CommandQueue* queue, unsigned count, ID3D12CommandList* const* lists)
{
    if (g_state.load(std::memory_order_relaxed) != kRecording || lists == nullptr) {
        return;
    }
    for (unsigned i = 0; i < count; ++i) {
        push(make(kExec, lists[i], reinterpret_cast<std::uint64_t>(queue), i, count));
    }
}

void onPresent()
{
    const int state = g_state.load(std::memory_order_acquire);
    if (state == kArmed) {
        g_framesLeft.store(kFrames, std::memory_order_relaxed);
        g_seq.store(0, std::memory_order_relaxed);
        g_dropped.store(0, std::memory_order_relaxed);
        g_state.store(kRecording, std::memory_order_release);
        push(make(kPresent, nullptr, 0));
        return;
    }
    if (state == kRecording) {
        const int left = g_framesLeft.fetch_sub(1, std::memory_order_relaxed) - 1;
        push(make(kPresent, nullptr, static_cast<std::uint64_t>(kFrames - left)));
        if (left <= 0) {
            g_state.store(kDumping, std::memory_order_release);
        }
    }
}

void pump()
{
    if (g_teardown.load(std::memory_order_relaxed)) {
        return;
    }
    if (g_state.load(std::memory_order_acquire) == kDumping) {
        dump();
        g_state.store(kIdle, std::memory_order_release);
        return;
    }
    const unsigned long long now = GetTickCount64();
    if (now - g_lastCheck < 1000) {
        return;
    }
    g_lastCheck = now;
    std::error_code ec;
    const auto path = paths::dataDir() / L"diag-frametrace.txt";
    if (!std::filesystem::exists(path, ec)) {
        return;
    }
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec || (g_haveTrigger && stamp == g_lastTrigger)) {
        return;
    }
    g_haveTrigger = true;
    g_lastTrigger = stamp;
    if (g_state.load(std::memory_order_acquire) == kIdle) {
        {
            const std::lock_guard<std::mutex> lock(g_mu);
            g_events.clear();
            g_events.reserve(65536);
        }
        g_state.store(kArmed, std::memory_order_release);
        log().info(L"FrameTrace: dumping {} frames starting from the next Present", kFrames);
    }
}

void shutdown()
{
    g_teardown.store(true, std::memory_order_relaxed);
    g_state.store(kIdle, std::memory_order_release);
}

}
