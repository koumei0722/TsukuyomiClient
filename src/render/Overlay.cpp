#include "render/Overlay.h"

#include "core/Logger.h"
#include "game/GameModeIds.h"
#include "hooks/HookManager.h"
#include "render/GameModeOverlay.h"
#include "ui/Theme.h"

#include <Windows.h>

#include <d2d1_1.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dwrite.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace tsukuyomi::render {

namespace {

using Microsoft::WRL::ComPtr;

constexpr size_t kPresentIndex = 8;
constexpr size_t kPresent1Index = 22;
constexpr size_t kExecuteCommandListsIndex = 10;

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(__stdcall*)(IDXGISwapChain1*, UINT, UINT,
                                       const DXGI_PRESENT_PARAMETERS*);
using ExecuteCommandListsFn = void(__stdcall*)(ID3D12CommandQueue*, UINT,
                                               ID3D12CommandList* const*);

PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;
ExecuteCommandListsFn g_executeCommandLists = nullptr;

std::atomic<ID3D12CommandQueue*> g_gameQueue{nullptr};

std::atomic<bool> g_active{true};

std::atomic<int> g_drawing{0};

std::atomic<bool> g_selectionVisible{false};
std::atomic<int> g_selectionMode{gamemode::kUnknown};

std::atomic<HWND> g_outputWindow{nullptr};
std::atomic<float> g_viewWidth{0.0f};
std::atomic<float> g_viewHeight{0.0f};

std::atomic<bool> g_loggedPresent{false};
std::atomic<bool> g_loggedPresent1{false};
std::atomic<bool> g_loggedQueue{false};
std::atomic<bool> g_loggedReady{false};
std::atomic<bool> g_loggedFailure{false};

std::mutex g_resourceMutex;

bool g_deviceFailed = false;

std::atomic<bool> g_deviceLive{false};

constexpr int kIdleFramesBeforeRelease = 180;

int g_idleFrames = 0;

struct Resources {
    ComPtr<ID3D11Device> d3d11Device;
    ComPtr<ID3D11DeviceContext> d3d11Context;
    ComPtr<ID3D11On12Device> d3d11On12;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dContext;
    ComPtr<IDWriteFactory> writeFactory;
    ComPtr<ID2D1SolidColorBrush> brush;

    ComPtr<IDWriteTextFormat> labelFormat;
    ComPtr<IDWriteTextFormat> cellFormat;
    ComPtr<IDWriteTextFormat> hintFormat;
    UINT fontHeight = 0;

    bool deviceReady = false;
};

Resources g_res;

std::atomic<WNDPROC> g_originalProc{nullptr};
HWND g_hookedWindow = nullptr;

std::atomic<unsigned int> g_frameIndex{0};

constexpr unsigned int kFramesAfterResize = 30;

std::atomic<unsigned int> g_resumeAtFrame{0};

void releaseAll();

LRESULT CALLBACK subclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_SIZE:
    case WM_ENTERSIZEMOVE:
    case WM_EXITSIZEMOVE:
    case WM_DISPLAYCHANGE:

        {

            g_resumeAtFrame.store(g_frameIndex.load(std::memory_order_relaxed)
                                      + kFramesAfterResize,
                                  std::memory_order_release);

            const std::lock_guard<std::mutex> lock(g_resourceMutex);
            releaseAll();
        }
        break;
    default:
        break;
    }

    const WNDPROC original = g_originalProc.load(std::memory_order_acquire);
    if (original != nullptr) {
        return CallWindowProcW(original, window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void hookGameWindow(HWND window)
{
    if (window == nullptr || g_hookedWindow == window) {
        return;
    }
    if (g_hookedWindow != nullptr) {
        return;
    }

    const auto previous = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&subclassProc)));
    if (previous == nullptr) {
        return;
    }

    g_originalProc.store(previous, std::memory_order_release);
    g_hookedWindow = window;
}

void unhookGameWindow()
{
    const WNDPROC original = g_originalProc.load(std::memory_order_acquire);
    if (g_hookedWindow != nullptr && original != nullptr) {
        SetWindowLongPtrW(g_hookedWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
    }
    g_hookedWindow = nullptr;
    g_originalProc.store(nullptr, std::memory_order_release);
}

void* vtableEntry(void* object, size_t index)
{
    if (object == nullptr) {
        return nullptr;
    }
    void*** const objectAsVtable = reinterpret_cast<void***>(object);
    void** const vtable = *objectAsVtable;
    return vtable != nullptr ? vtable[index] : nullptr;
}

bool isGameSwapChain(IDXGISwapChain* swapChain, DXGI_SWAP_CHAIN_DESC& descOut)
{
    if (swapChain == nullptr || FAILED(swapChain->GetDesc(&descOut))) {
        return false;
    }
    if (descOut.OutputWindow == nullptr) {
        return false;
    }

    constexpr int kClassNameMax = 64;
    wchar_t className[kClassNameMax]{};
    if (GetClassNameW(descOut.OutputWindow, className, kClassNameMax) == 0) {
        return false;
    }

    return wcscmp(className, L"Bedrock") == 0;
}

void releaseAll()
{
    if (g_res.d2dContext) {
        g_res.d2dContext->SetTarget(nullptr);
    }
    if (g_res.d3d11Context) {
        g_res.d3d11Context->ClearState();
        g_res.d3d11Context->Flush();
    }

    g_res.brush.Reset();
    g_res.labelFormat.Reset();
    g_res.cellFormat.Reset();
    g_res.hintFormat.Reset();
    g_res.fontHeight = 0;
    g_res.writeFactory.Reset();
    g_res.d2dContext.Reset();
    g_res.d2dDevice.Reset();
    g_res.d2dFactory.Reset();
    g_res.d3d11On12.Reset();
    g_res.d3d11Context.Reset();
    g_res.d3d11Device.Reset();
    g_res.deviceReady = false;
    g_deviceLive.store(false, std::memory_order_relaxed);
}

void reportFailure(const wchar_t* what, HRESULT result)
{
    if (g_loggedFailure.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    log().error(L"Overlay: {} failed ({:#x}). The in-game overlay is off", what,
                static_cast<unsigned int>(result));
}

bool failDevice(const wchar_t* what, HRESULT result)
{
    reportFailure(what, result);
    g_deviceFailed = true;
    return false;
}

bool createDevice()
{
    if (g_res.deviceReady) {
        return true;
    }
    if (g_deviceFailed) {
        return false;
    }

    ID3D12CommandQueue* const queue = g_gameQueue.load(std::memory_order_relaxed);
    if (queue == nullptr) {

        return false;
    }

    ComPtr<ID3D12Device> device;
    HRESULT result = queue->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(result)) {
        return failDevice(L"getting the D3D12 device", result);
    }

    const HMODULE d3d11Module = LoadLibraryW(L"d3d11.dll");
    if (d3d11Module == nullptr) {
        return failDevice(L"loading d3d11.dll", HRESULT_FROM_WIN32(GetLastError()));
    }

    const auto createOn12 = reinterpret_cast<PFN_D3D11ON12_CREATE_DEVICE>(
        GetProcAddress(d3d11Module, "D3D11On12CreateDevice"));
    if (createOn12 == nullptr) {
        return failDevice(L"resolving D3D11On12CreateDevice", E_NOINTERFACE);
    }

    IUnknown* queues[] = {queue};
    result = createOn12(device.Get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, queues, 1, 0,
                        &g_res.d3d11Device, &g_res.d3d11Context, nullptr);
    if (FAILED(result)) {
        return failDevice(L"creating the D3D11On12 device", result);
    }

    result = g_res.d3d11Device.As(&g_res.d3d11On12);
    if (FAILED(result)) {
        return failDevice(L"querying ID3D11On12Device", result);
    }

    D2D1_FACTORY_OPTIONS options{};
    result = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &options,
                               reinterpret_cast<void**>(g_res.d2dFactory.GetAddressOf()));
    if (FAILED(result)) {
        return failDevice(L"creating the Direct2D factory", result);
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    result = g_res.d3d11Device.As(&dxgiDevice);
    if (FAILED(result)) {
        return failDevice(L"querying IDXGIDevice", result);
    }

    result = g_res.d2dFactory->CreateDevice(dxgiDevice.Get(), &g_res.d2dDevice);
    if (FAILED(result)) {
        return failDevice(L"creating the Direct2D device", result);
    }

    result = g_res.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                  &g_res.d2dContext);
    if (FAILED(result)) {
        return failDevice(L"creating the Direct2D context", result);
    }

    result = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(g_res.writeFactory.GetAddressOf()));
    if (FAILED(result)) {
        return failDevice(L"creating the DirectWrite factory", result);
    }

    result = g_res.d2dContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
                                                     &g_res.brush);
    if (FAILED(result)) {
        return failDevice(L"creating the brush", result);
    }

    g_res.deviceReady = true;
    g_deviceLive.store(true, std::memory_order_relaxed);
    g_idleFrames = 0;
    return true;
}

bool ensureFonts(UINT height)
{
    if (g_res.fontHeight == height && g_res.labelFormat && g_res.cellFormat && g_res.hintFormat) {
        return true;
    }

    g_res.labelFormat.Reset();
    g_res.cellFormat.Reset();
    g_res.hintFormat.Reset();

    const float scale = overlayScale(static_cast<float>(height));

    const auto create = [&scale](float size, DWRITE_FONT_WEIGHT weight,
                                 ComPtr<IDWriteTextFormat>& out) {
        const HRESULT created = g_res.writeFactory->CreateTextFormat(
            theme::kUiFont, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            size * scale, L"en-us", &out);
        if (FAILED(created)) {
            return created;
        }

        out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return S_OK;
    };

    HRESULT result = create(26.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, g_res.labelFormat);
    if (SUCCEEDED(result)) {
        result = create(12.5f, DWRITE_FONT_WEIGHT_NORMAL, g_res.cellFormat);
    }
    if (SUCCEEDED(result)) {
        result = create(13.5f, DWRITE_FONT_WEIGHT_NORMAL, g_res.hintFormat);
    }
    if (FAILED(result)) {
        reportFailure(L"creating the text formats", result);
        g_res.labelFormat.Reset();
        g_res.cellFormat.Reset();
        g_res.hintFormat.Reset();
        return false;
    }

    g_res.fontHeight = height;
    return true;
}

void drawContents(float width, float height)
{
    if (g_selectionVisible.load(std::memory_order_relaxed)) {
        GameModeStyle style;
        style.labelFormat = g_res.labelFormat.Get();
        style.cellFormat = g_res.cellFormat.Get();
        style.hintFormat = g_res.hintFormat.Get();
        style.brush = g_res.brush.Get();

        drawGameModeSwitcher(g_res.d2dContext.Get(), style, width, height,
                             g_selectionMode.load(std::memory_order_relaxed));
    }

}

void drawOverlay(IDXGISwapChain* swapChain)
{
    DXGI_SWAP_CHAIN_DESC desc{};
    if (!isGameSwapChain(swapChain, desc)) {
        return;
    }

    if (!createDevice() || !ensureFonts(desc.BufferDesc.Height)) {
        return;
    }

    g_outputWindow.store(desc.OutputWindow, std::memory_order_relaxed);
    g_viewWidth.store(static_cast<float>(desc.BufferDesc.Width), std::memory_order_relaxed);
    g_viewHeight.store(static_cast<float>(desc.BufferDesc.Height), std::memory_order_relaxed);

    hookGameWindow(desc.OutputWindow);

    ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3)))) {
        reportFailure(L"querying IDXGISwapChain3", E_NOINTERFACE);
        return;
    }

    ComPtr<ID3D12Resource> backBuffer;
    if (FAILED(swapChain3->GetBuffer(swapChain3->GetCurrentBackBufferIndex(),
                                     IID_PPV_ARGS(&backBuffer)))) {

        return;
    }

    D3D11_RESOURCE_FLAGS resourceFlags{};
    resourceFlags.BindFlags = D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Resource> wrapped;
    HRESULT result = g_res.d3d11On12->CreateWrappedResource(
        backBuffer.Get(), &resourceFlags, D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT, IID_PPV_ARGS(&wrapped));
    if (FAILED(result)) {
        reportFailure(L"wrapping the back buffer", result);
        return;
    }

    ComPtr<IDXGISurface> surface;
    if (FAILED(wrapped.As(&surface))) {
        return;
    }

    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(desc.BufferDesc.Format, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);

    ComPtr<ID2D1Bitmap1> bitmap;
    result = g_res.d2dContext->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &bitmap);
    if (FAILED(result)) {
        reportFailure(L"creating the Direct2D target", result);
        return;
    }

    ID3D11Resource* const resources[] = {wrapped.Get()};
    g_res.d3d11On12->AcquireWrappedResources(resources, 1);

    g_res.d2dContext->SetTarget(bitmap.Get());
    g_res.d2dContext->BeginDraw();
    drawContents(static_cast<float>(desc.BufferDesc.Width),
                 static_cast<float>(desc.BufferDesc.Height));
    const HRESULT drawResult = g_res.d2dContext->EndDraw();
    g_res.d2dContext->SetTarget(nullptr);

    g_res.d3d11On12->ReleaseWrappedResources(resources, 1);

    g_res.d3d11Context->ClearState();
    g_res.d3d11Context->Flush();

    if (!g_loggedReady.exchange(true, std::memory_order_relaxed)) {
        log().success(L"Overlay: drawing on the game surface ({}x{})", desc.BufferDesc.Width,
                      desc.BufferDesc.Height);
    }

    if (drawResult == D2DERR_RECREATE_TARGET) {

        releaseAll();
    } else if (FAILED(drawResult)) {
        reportFailure(L"drawing", drawResult);
        releaseAll();
    }

}

void tryDraw(IDXGISwapChain* swapChain)
{

    const unsigned int frame = g_frameIndex.fetch_add(1, std::memory_order_relaxed) + 1;

    if (!g_active.load(std::memory_order_acquire)) {
        return;
    }

    if (const unsigned int resumeAt = g_resumeAtFrame.load(std::memory_order_acquire);
        resumeAt != 0) {
        if (frame < resumeAt) {

            if (g_deviceLive.load(std::memory_order_relaxed)) {
                const std::lock_guard<std::mutex> lock(g_resourceMutex);
                releaseAll();
            }
            return;
        }

        g_resumeAtFrame.store(0, std::memory_order_relaxed);
        g_idleFrames = 0;
    }

    if (!g_selectionVisible.load(std::memory_order_acquire)) {
        if (g_deviceLive.load(std::memory_order_relaxed)
            && ++g_idleFrames > kIdleFramesBeforeRelease) {
            const std::lock_guard<std::mutex> lock(g_resourceMutex);
            releaseAll();
        }
        return;
    }

    g_idleFrames = 0;

    g_drawing.fetch_add(1, std::memory_order_acq_rel);
    if (g_active.load(std::memory_order_acquire)) {
        const std::lock_guard<std::mutex> lock(g_resourceMutex);
        drawOverlay(swapChain);
    }
    g_drawing.fetch_sub(1, std::memory_order_acq_rel);
}

void logSwapChain(IDXGISwapChain* swapChain, bool viaPresent1)
{
    std::atomic<bool>& flag = viaPresent1 ? g_loggedPresent1 : g_loggedPresent;
    if (flag.exchange(true, std::memory_order_relaxed)) {
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    if (swapChain == nullptr || FAILED(swapChain->GetDesc(&desc))) {
        log().warn(L"Overlay: {} reached but the description could not be read",
                   viaPresent1 ? L"Present1" : L"Present");
        return;
    }

    log().success(L"Overlay: {} reached ({}x{}, hwnd {:#x}, format {}, buffers {})",
                  viaPresent1 ? L"Present1" : L"Present", desc.BufferDesc.Width,
                  desc.BufferDesc.Height, reinterpret_cast<uintptr_t>(desc.OutputWindow),
                  static_cast<int>(desc.BufferDesc.Format), desc.BufferCount);
}

HRESULT __stdcall detourPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    logSwapChain(swapChain, false);

    if ((flags & DXGI_PRESENT_TEST) == 0) {
        tryDraw(swapChain);
    }

    if (g_present == nullptr) {
        return DXGI_ERROR_INVALID_CALL;
    }
    return g_present(swapChain, syncInterval, flags);
}

HRESULT __stdcall detourPresent1(IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags,
                                 const DXGI_PRESENT_PARAMETERS* parameters)
{
    logSwapChain(swapChain, true);

    if (g_present1 == nullptr) {
        return DXGI_ERROR_INVALID_CALL;
    }
    return g_present1(swapChain, syncInterval, flags, parameters);
}

void __stdcall detourExecuteCommandLists(ID3D12CommandQueue* queue, UINT count,
                                         ID3D12CommandList* const* lists)
{
    if (queue != nullptr && g_gameQueue.load(std::memory_order_relaxed) == nullptr) {

        if (queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_gameQueue.store(queue, std::memory_order_relaxed);

            if (!g_loggedQueue.exchange(true, std::memory_order_relaxed)) {
                log().success(L"Overlay: direct command queue captured ({:#x})",
                              reinterpret_cast<uintptr_t>(queue));
            }
        }
    }

    if (g_executeCommandLists != nullptr) {
        g_executeCommandLists(queue, count, lists);
    }
}

class ProbeWindow {
public:
    ProbeWindow() = default;
    ProbeWindow(const ProbeWindow&) = delete;
    ProbeWindow& operator=(const ProbeWindow&) = delete;

    ~ProbeWindow()
    {
        if (m_window != nullptr) {
            DestroyWindow(m_window);
        }
        if (m_registered) {
            UnregisterClassW(m_className.c_str(), m_instance);
        }
    }

    bool create()
    {
        m_instance = GetModuleHandleW(nullptr);

        m_className = L"TsukuyomiOverlayProbe_"
                      + std::to_wstring(reinterpret_cast<uintptr_t>(&g_present));

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = m_instance;
        windowClass.lpszClassName = m_className.c_str();
        if (RegisterClassExW(&windowClass) == 0) {
            return false;
        }
        m_registered = true;

        m_window = CreateWindowExW(0, m_className.c_str(), L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                                   nullptr, nullptr, m_instance, nullptr);
        return m_window != nullptr;
    }

    HWND handle() const { return m_window; }

private:
    HMODULE m_instance = nullptr;
    std::wstring m_className;
    bool m_registered = false;
    HWND m_window = nullptr;
};

struct Targets {
    void* present = nullptr;
    void* present1 = nullptr;
    void* executeCommandLists = nullptr;

    bool complete() const
    {
        return present != nullptr && present1 != nullptr && executeCommandLists != nullptr;
    }
};

bool captureTargets(Targets& out)
{
    const HMODULE d3d12Module = GetModuleHandleW(L"d3d12.dll");
    const HMODULE dxgiModule = GetModuleHandleW(L"dxgi.dll");
    if (d3d12Module == nullptr || dxgiModule == nullptr) {
        log().warn(L"Overlay: d3d12.dll or dxgi.dll is not loaded yet");
        return false;
    }

    const auto createDevice12 = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
        GetProcAddress(d3d12Module, "D3D12CreateDevice"));
    const auto createFactory = reinterpret_cast<HRESULT(__stdcall*)(REFIID, void**)>(
        GetProcAddress(dxgiModule, "CreateDXGIFactory1"));
    if (createDevice12 == nullptr || createFactory == nullptr) {
        log().error(L"Overlay: could not resolve the D3D12 / DXGI entry points");
        return false;
    }

    ComPtr<ID3D12Device> device;
    if (FAILED(createDevice12(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        log().error(L"Overlay: could not create a probe device");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))) {
        log().error(L"Overlay: could not create a probe command queue");
        return false;
    }

    ProbeWindow window;
    if (!window.create()) {
        log().error(L"Overlay: could not create the probe window");
        return false;
    }

    ComPtr<IDXGIFactory2> factory;
    if (FAILED(createFactory(IID_PPV_ARGS(&factory)))) {
        log().error(L"Overlay: could not create a DXGI factory");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width = 64;
    swapDesc.Height = 64;
    swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 2;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain;
    if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), window.handle(), &swapDesc, nullptr,
                                               nullptr, &swapChain))) {
        log().error(L"Overlay: could not create a probe swap chain");
        return false;
    }

    out.present = vtableEntry(swapChain.Get(), kPresentIndex);
    out.present1 = vtableEntry(swapChain.Get(), kPresent1Index);
    out.executeCommandLists = vtableEntry(queue.Get(), kExecuteCommandListsIndex);

    return out.complete();
}

}

bool installOverlayHooks()
{
    Targets targets;
    if (!captureTargets(targets)) {
        log().warn(L"Overlay: the in-game overlay is unavailable");
        return false;
    }

    HookManager& hooks = HookManager::instance();

    const bool ok =
        hooks.create(targets.present, &detourPresent, reinterpret_cast<void**>(&g_present),
                     L"Present")
        && hooks.create(targets.present1, &detourPresent1, reinterpret_cast<void**>(&g_present1),
                        L"Present1")
        && hooks.create(targets.executeCommandLists, &detourExecuteCommandLists,
                        reinterpret_cast<void**>(&g_executeCommandLists), L"ExecuteCommandLists");

    return ok;
}

void setGameModeSelection(bool visible, int selectedMode)
{

    g_selectionMode.store(selectedMode, std::memory_order_relaxed);
    g_selectionVisible.store(visible, std::memory_order_release);
}

Viewport overlayViewport()
{
    Viewport view;
    view.window = g_outputWindow.load(std::memory_order_relaxed);
    view.width = g_viewWidth.load(std::memory_order_relaxed);
    view.height = g_viewHeight.load(std::memory_order_relaxed);
    view.scale = (view.height > 0.0f) ? overlayScale(view.height) : 1.0f;
    view.valid = view.window != nullptr && view.width > 0.0f && view.height > 0.0f;
    return view;
}

void shutdownOverlay()
{
    g_selectionVisible.store(false, std::memory_order_release);

    g_active.store(false, std::memory_order_release);

    for (int waited = 0; waited < 400 && g_drawing.load(std::memory_order_acquire) != 0; ++waited) {
        Sleep(1);
    }

    unhookGameWindow();

    {
        const std::lock_guard<std::mutex> lock(g_resourceMutex);
        releaseAll();
    }

    g_gameQueue.store(nullptr, std::memory_order_relaxed);
}

}
