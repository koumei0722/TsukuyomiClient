#include "render/Overlay.h"

#include "core/FreezeWatch.h"
#include "core/Logger.h"
#include "hooks/HookManager.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <string>

namespace tsukuyomi::render {
namespace {
using Microsoft::WRL::ComPtr;
constexpr size_t kPresentIndex = 8;

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);

PresentFn g_present = nullptr;

std::atomic<bool> g_active{true};
std::atomic<int> g_drawing{0};

std::atomic<HWND> g_outputWindow{nullptr};
std::atomic<float> g_viewWidth{0.0f};
std::atomic<float> g_viewHeight{0.0f};

std::atomic<bool> g_loggedPresent{false};
std::atomic<bool> g_sawD3D12{false};
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

void notePresent(IDXGISwapChain* swapChain)
{
    g_drawing.fetch_add(1, std::memory_order_acq_rel);
    if (g_active.load(std::memory_order_acquire)) {
        DXGI_SWAP_CHAIN_DESC desc{};
        static std::atomic<HWND> s_checkedWindow{nullptr};
        static std::atomic<bool> s_checkedIsGame{false};
        if (swapChain != nullptr && SUCCEEDED(swapChain->GetDesc(&desc)) && desc.OutputWindow != nullptr) {
            bool isGame = false;
            if (s_checkedWindow.load(std::memory_order_acquire) == desc.OutputWindow) {
                isGame = s_checkedIsGame.load(std::memory_order_acquire);
            } else {
                DXGI_SWAP_CHAIN_DESC checked{};
                isGame = isGameSwapChain(swapChain, checked);
                if (isGame) {
                    ID3D12Device* device = nullptr;
                    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device)))) {
                        device->Release();
                        g_sawD3D12.store(true, std::memory_order_relaxed);
                    }
                }
                s_checkedIsGame.store(isGame, std::memory_order_release);
                s_checkedWindow.store(desc.OutputWindow, std::memory_order_release);
            }
            if (isGame) {
                g_outputWindow.store(desc.OutputWindow, std::memory_order_relaxed);
                g_viewWidth.store(static_cast<float>(desc.BufferDesc.Width), std::memory_order_relaxed);
                g_viewHeight.store(static_cast<float>(desc.BufferDesc.Height), std::memory_order_relaxed);
            }
        }
    }
    g_drawing.fetch_sub(1, std::memory_order_acq_rel);
}
void logSwapChain(IDXGISwapChain* swapChain)
{
    if (g_loggedPresent.exchange(true, std::memory_order_relaxed)) {
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    if (swapChain == nullptr || FAILED(swapChain->GetDesc(&desc))) {
        log().warn(L"Overlay: Present reached but the description could not be read");
        return;
    }

    log().success(L"Overlay: Present reached ({}x{}, hwnd {:#x}, format {}, buffers {})",
                  desc.BufferDesc.Width,
                  desc.BufferDesc.Height, reinterpret_cast<uintptr_t>(desc.OutputWindow),
                  static_cast<int>(desc.BufferDesc.Format), desc.BufferCount);
}

HRESULT __stdcall detourPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    freezewatch::notePresent();
    logSwapChain(swapChain);

    if ((flags & DXGI_PRESENT_TEST) == 0) {
        notePresent(swapChain);
    }

    if (g_present == nullptr) return DXGI_ERROR_INVALID_CALL;
    return g_present(swapChain, syncInterval, flags);
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

bool captureTargets(void*& present)
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

    present = vtableEntry(swapChain.Get(), kPresentIndex);

    return present != nullptr;
}

}

bool installOverlayHooks()
{
    void* present = nullptr;
    if (!captureTargets(present)) {
        log().warn(L"Overlay: the Present hooks are unavailable");
        return false;
    }

    HookManager& hooks = HookManager::instance();

    return hooks.create(present, &detourPresent, reinterpret_cast<void**>(&g_present), L"Present");
}

bool sawD3D12()
{
    return g_sawD3D12.load(std::memory_order_relaxed);
}

Viewport overlayViewport()
{
    Viewport view;
    view.window = g_outputWindow.load(std::memory_order_relaxed);
    view.width = g_viewWidth.load(std::memory_order_relaxed);
    view.height = g_viewHeight.load(std::memory_order_relaxed);
    view.valid = view.window != nullptr && view.width > 0.0f && view.height > 0.0f;
    return view;
}

void shutdownOverlay()
{
    g_active.store(false, std::memory_order_release);
    for (int waited = 0; waited < 400 && g_drawing.load(std::memory_order_acquire) != 0; ++waited) {
        Sleep(1);
    }
}

}
