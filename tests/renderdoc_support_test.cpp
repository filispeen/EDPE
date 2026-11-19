#include "renderdoc_support.h"
#include "system_dll.h"

#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <filesystem>
#include <string>
#include <windows.h>

namespace {

int fail(const char* msg) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

int runUnitTests() {
    const HMODULE sysDxgi = LoadSystemDll(L"dxgi.dll");
    if (!sysDxgi) return fail("LoadSystemDll(dxgi.dll) failed");

    void* rawFactory1 = ResolvePeExportRaw(sysDxgi, "CreateDXGIFactory1");
    if (!rawFactory1) return fail("ResolvePeExportRaw(CreateDXGIFactory1) returned null");

    if (!GetModuleHandleW(L"renderdoc.dll")) {
        void* procFactory1 = reinterpret_cast<void*>(GetProcAddress(sysDxgi, "CreateDXGIFactory1"));
        if (rawFactory1 != procFactory1) {
            return fail("ResolvePeExportRaw mismatch vs GetProcAddress without RenderDoc");
        }
    }

    if (ResolvePeExportRaw(sysDxgi, "NonExistentExport_EDPE") != nullptr) {
        return fail("ResolvePeExportRaw should return null for missing export");
    }
    if (ResolvePeExportRaw(nullptr, "CreateDXGIFactory1") != nullptr) {
        return fail("ResolvePeExportRaw should return null for null module");
    }
    if (ResolvePeExportRaw(sysDxgi, nullptr) != nullptr) {
        return fail("ResolvePeExportRaw should return null for null procName");
    }

    if (IsAddressInRenderDocModule(nullptr)) {
        return fail("IsAddressInRenderDocModule(nullptr) should be false");
    }
    if (IsAddressInRenderDocModule(reinterpret_cast<const void*>(&runUnitTests))) {
        return fail("IsAddressInRenderDocModule(local fn) should be false");
    }

    auto& rdoc = edpe::RenderDocBridge::instance();
    rdoc.resetForTest();
    if (!GetModuleHandleW(L"renderdoc.dll")) {
        if (rdoc.refresh()) return fail("refresh() should return false without renderdoc.dll");
        if (rdoc.isAvailable()) return fail("isAvailable() should be false without renderdoc.dll");
        if (rdoc.areAnnotationsActive()) return fail("areAnnotationsActive() should be false");
        if (rdoc.triggerCapture(1)) return fail("triggerCapture() should return false");
        if (rdoc.isFrameCapturing()) return fail("isFrameCapturing() should be false");
        if (rdoc.isTargetControlConnected()) return fail("isTargetControlConnected() should be false");
        if (rdoc.getNumCaptures() != 0) return fail("getNumCaptures() should be 0");
        if (rdoc.launchOrShowReplayUI() != 0) return fail("launchOrShowReplayUI() should return 0");
        std::string capPath;
        if (rdoc.getCapturePath(0, capPath)) return fail("getCapturePath() should return false");
    }

    rdoc.setAnnotationsEnabled(false);
    if (rdoc.areAnnotationsEnabled()) return fail("areAnnotationsEnabled() should be false");
    rdoc.setAnnotationsEnabled(true);
    if (!rdoc.areAnnotationsEnabled()) return fail("areAnnotationsEnabled() should be true");

    // Verify debug naming and scoped event helpers are safe on a WARP device without RenderDoc.
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    if (SUCCEEDED(hr) && device && context) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ID3D11Buffer* buf = nullptr;
        if (SUCCEEDED(device->CreateBuffer(&bd, nullptr, &buf)) && buf) {
            edpe::SetD3D11DebugName(buf, "EDPE_TestBuffer");
            buf->Release();
        }
        {
            edpe::ScopedRenderDocEvent ev(context, L"EDPE: TestEvent");
            edpe::EmitRenderDocMarker(context, L"EDPE: TestMarker");
        }
        context->Release();
        device->Release();
    }

    return 0;
}

LRESULT CALLBACK smokeWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int runCaptureSmoke(const wchar_t* dxgiPath) {
    const HMODULE dxgiMod = LoadLibraryW(dxgiPath);
    if (!dxgiMod) return fail("LoadLibraryW(dxgiPath) failed");

    const auto pD3D11CreateDevice = reinterpret_cast<decltype(&D3D11CreateDevice)>(
        GetProcAddress(dxgiMod, "D3D11CreateDevice"));
    const auto pCreateDXGIFactory1 = reinterpret_cast<decltype(&CreateDXGIFactory1)>(
        GetProcAddress(dxgiMod, "CreateDXGIFactory1"));
    const auto pPresentCount = reinterpret_cast<unsigned long long (WINAPI*)()>(
        GetProcAddress(dxgiMod, "EDPE_ObservedPresentCount"));
    const auto pMenuVisible = reinterpret_cast<BOOL (WINAPI*)()>(
        GetProcAddress(dxgiMod, "EDPE_MenuVisible"));
    if (!pD3D11CreateDevice || !pCreateDXGIFactory1 || !pPresentCount || !pMenuVisible) {
        return fail("Missing EDPE dxgi.dll exports");
    }

    IDXGIFactory1* factory = nullptr;
    if (FAILED(pCreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
        return fail("CreateDXGIFactory1 failed");
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (FAILED(pD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)) || !device || !context) {
        factory->Release();
        return fail("D3D11CreateDevice failed");
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = smokeWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"EDPE_RDOC_SMOKE";
    if (!RegisterClassW(&wc)) return fail("RegisterClassW failed");
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"EDPE RenderDoc Smoke", WS_OVERLAPPEDWINDOW,
        0, 0, 128, 128, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return fail("CreateWindowExW failed");

    DXGI_SWAP_CHAIN_DESC scDesc{};
    scDesc.BufferDesc.Width = 128;
    scDesc.BufferDesc.Height = 128;
    scDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2;
    scDesc.OutputWindow = hwnd;
    scDesc.Windowed = TRUE;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swapChain = nullptr;
    if (FAILED(factory->CreateSwapChain(device, &scDesc, &swapChain)) || !swapChain) {
        return fail("CreateSwapChain failed");
    }

    auto& rdoc = edpe::RenderDocBridge::instance();
    if (!rdoc.refresh() || !rdoc.isAvailable()) {
        return fail("RenderDoc not detected inside capture-smoke mode");
    }
    rdoc.setActiveWindow(device, hwnd);

    // First Present initializes ImGui and hooks the window proc.
    if (FAILED(swapChain->Present(0, 0))) return fail("Initial Present failed");

    // Open EDPE F5 menu so ImGui overlay draw calls are captured inside the RenderDoc frame.
    SendMessageW(hwnd, WM_KEYDOWN, VK_F5, 0);
    SendMessageW(hwnd, WM_KEYUP, VK_F5, 0);

    ID3D11Texture2D* backbuffer = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer))) && backbuffer) {
        device->CreateRenderTargetView(backbuffer, nullptr, &rtv);
        backbuffer->Release();
    }

    const uint32_t beforeCaptures = rdoc.getNumCaptures();
    for (int f = 0; f < 5; ++f) {
        if (f == 2) {
            if (!rdoc.triggerCapture(1)) return fail("triggerCapture(1) failed");
        }
        if (rtv) {
            const float clearCol[4] = {0.05f, 0.10f, 0.20f, 1.0f};
            context->ClearRenderTargetView(rtv, clearCol);
        }
        {
            edpe::ScopedRenderDocEvent ev(context, L"EDPE_Smoke: Frame");
            context->Flush();
        }
        if (FAILED(swapChain->Present(0, 0))) return fail("Present failed");
    }

    if (pPresentCount() < 5) return fail("EDPE_ObservedPresentCount did not advance");
    if (!pMenuVisible()) return fail("EDPE menu did not open via F5");

    const uint32_t afterCaptures = rdoc.getNumCaptures();
    if (afterCaptures <= beforeCaptures) {
        return fail("RenderDoc did not produce a capture");
    }

    std::string capPath;
    if (!rdoc.getCapturePath(afterCaptures - 1, capPath) || capPath.empty()) {
        return fail("getCapturePath failed");
    }
    std::error_code ec;
    const auto sz = std::filesystem::file_size(capPath, ec);
    if (ec || sz == 0) {
        return fail("Captured .rdc file missing or empty");
    }
    std::printf("CAPTURED_RDC=%s (%llu bytes) API=%d.%d.%d\n",
        capPath.c_str(), static_cast<unsigned long long>(sz),
        rdoc.apiMajor(), rdoc.apiMinor(), rdoc.apiPatch());

    if (rtv) rtv->Release();
    swapChain->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    factory->Release();
    context->Release();
    device->Release();
    FreeLibrary(dxgiMod);
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wcscmp(argv[1], L"--capture-smoke") == 0) {
        return runCaptureSmoke(argv[2]);
    }
    return runUnitTests();
}
