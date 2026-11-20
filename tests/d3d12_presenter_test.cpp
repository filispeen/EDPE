#include "d3d12_presenter.h"
#include "system_dll.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

LRESULT CALLBACK presenterTestWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int fail(const char* msg) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

int runUnitTests() {
    // 1. Flip-model sanitization invariants.
    {
        const auto d1 = edpe::sanitizeFlipSwapChainDesc(
            0, 0, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1, DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE, true);
        if (d1.Width != 1 || d1.Height != 1) return fail("Zero dimensions should clamp to 1x1");
        if (d1.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return fail("sRGB RGBA8 must map to UNORM");
        if (d1.BufferCount != 3) return fail("BufferCount < 2 must promote to 3");
        if (d1.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD) return fail("SwapEffect must be FLIP_DISCARD");
        if (d1.Flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE) return fail("GDI_COMPATIBLE must be stripped");
        if (!(d1.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) return fail("ALLOW_TEARING should be set when supported");

        const auto d2 = edpe::sanitizeFlipSwapChainDesc(
            2560, 1440, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 2,
            DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING, false);
        if (d2.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return fail("sRGB BGRA8 must map to UNORM");
        if (d2.BufferCount != 2) return fail("BufferCount=2 should be preserved");
        if (d2.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) return fail("ALLOW_TEARING must be cleared when unsupported");
    }

    // 2. Hidden dummy window creation & cleanup.
    {
        edpe::HiddenDummyWindow dummy;
        if (!dummy.create() || !dummy.hwnd()) return fail("HiddenDummyWindow::create failed");
        if (IsWindowVisible(dummy.hwnd())) return fail("HiddenDummyWindow must stay hidden");
        dummy.destroy();
        if (dummy.hwnd() != nullptr) return fail("HiddenDummyWindow::destroy did not clear HWND");
    }

    // 3. RenderDoc cross-API guard matches renderdoc.dll presence.
    const bool rdocLoaded = (GetModuleHandleW(L"renderdoc.dll") != nullptr);
    if (edpe::D3D12ProxyPresenter::isCrossApiPresentBlockedByRenderDoc() != rdocLoaded) {
        return fail("isCrossApiPresentBlockedByRenderDoc mismatch");
    }

    return 0;
}

bool createDeviceForTest(ComPtr<ID3D11Device>& outDevice,
                         ComPtr<ID3D11DeviceContext>& outContext,
                         ComPtr<IDXGIAdapter3>& outAdapter3,
                         std::wstring& outName,
                         bool& outIsHardware) {
    outIsHardware = false;
    HMODULE sysDxgi = LoadSystemDll(L"dxgi.dll");
    using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    auto createFactory1 = sysDxgi
        ? reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(sysDxgi, "CreateDXGIFactory1"))
        : nullptr;

    if (createFactory1) {
        ComPtr<IDXGIFactory4> factory;
        if (SUCCEEDED(createFactory1(IID_PPV_ARGS(&factory)))) {
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> adapter;
                if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC1 desc{};
                adapter->GetDesc1(&desc);
                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

                D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
                if (SUCCEEDED(D3D11CreateDevice(
                        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1,
                        D3D11_SDK_VERSION, &outDevice, nullptr, &outContext))) {
                    edpe::D3D11D3D12Bridge probe;
                    if (probe.initialize(outDevice.Get())) {
                        probe.shutdown();
                        adapter.As(&outAdapter3);
                        outName = desc.Description;
                        outIsHardware = true;
                        return true;
                    }
                    outContext.Reset();
                    outDevice.Reset();
                }
            }
        }
    }

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1,
                                 D3D11_SDK_VERSION, &outDevice, nullptr, &outContext))) {
        return false;
    }
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(outDevice.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
        adapter.As(&outAdapter3);
    }
    outName = L"WARP Software Adapter";
    return true;
}

uint64_t queryLocalVramBytes(IDXGIAdapter3* adapter3) {
    if (!adapter3) return 0;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        return 0;
    }
    return info.CurrentUsage;
}

double meanOf(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (double x : v) s += x;
    return s / static_cast<double>(v.size());
}

double stddevOf(const std::vector<double>& v) {
    if (v.size() < 2) return 0.0;
    const double m = meanOf(v);
    double acc = 0.0;
    for (double x : v) acc += (x - m) * (x - m);
    return std::sqrt(acc / static_cast<double>(v.size()));
}

int runPresenterIntegrationAndBenchmark() {
    ComPtr<ID3D11Device> d3d11Dev;
    ComPtr<ID3D11DeviceContext> d3d11Ctx;
    ComPtr<IDXGIAdapter3> adapter3;
    std::wstring adapterName;
    bool isHardware = false;
    if (!createDeviceForTest(d3d11Dev, d3d11Ctx, adapter3, adapterName, isHardware)) {
        return fail("createDeviceForTest failed");
    }

    ComPtr<ID3D11DeviceContext4> ctx4;
    if (FAILED(d3d11Ctx.As(&ctx4))) return fail("QueryInterface(ID3D11DeviceContext4) failed");

    WNDCLASSW wc{};
    wc.lpfnWndProc = presenterTestWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"EDPE_D3D12Presenter_TestWnd";
    RegisterClassW(&wc);

    const uint32_t W = isHardware ? 2560u : 256u;
    const uint32_t H = isHardware ? 1440u : 256u;
    HWND targetHwnd = CreateWindowExW(
        0, wc.lpszClassName, L"EDPE Phase 13B Test", WS_OVERLAPPEDWINDOW,
        0, 0, static_cast<int>(W), static_cast<int>(H),
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!targetHwnd) return fail("CreateWindowExW failed");

    // Create offscreen 1x1 D3D11 swapchain on hidden dummy window (Option A2 pattern).
    edpe::HiddenDummyWindow dummyWnd;
    if (!dummyWnd.create()) {
        DestroyWindow(targetHwnd);
        return fail("HiddenDummyWindow::create failed");
    }

    HMODULE sysDxgi = LoadSystemDll(L"dxgi.dll");
    using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto createFactory2 = reinterpret_cast<PFN_CreateDXGIFactory2>(
        GetProcAddress(sysDxgi, "CreateDXGIFactory2"));
    if (!createFactory2) {
        DestroyWindow(targetHwnd);
        return fail("GetProcAddress(CreateDXGIFactory2) failed");
    }
    ComPtr<IDXGIFactory4> sysFactory;
    if (FAILED(createFactory2(0, IID_PPV_ARGS(&sysFactory)))) {
        DestroyWindow(targetHwnd);
        return fail("CreateDXGIFactory2 failed");
    }

    DXGI_SWAP_CHAIN_DESC1 dummyDesc{};
    dummyDesc.Width = 1;
    dummyDesc.Height = 1;
    dummyDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dummyDesc.SampleDesc.Count = 1;
    dummyDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    dummyDesc.BufferCount = 2;
    dummyDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> dummySc;
    if (FAILED(sysFactory->CreateSwapChainForHwnd(
            d3d11Dev.Get(), dummyWnd.hwnd(), &dummyDesc, nullptr, nullptr, &dummySc))) {
        DestroyWindow(targetHwnd);
        return fail("Dummy D3D11 swapchain creation failed");
    }

    // D3D11 render target simulating Elite's final backbuffer texture.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> d3d11FrameTex;
    ComPtr<ID3D11RenderTargetView> d3d11FrameRtv;
    if (FAILED(d3d11Dev->CreateTexture2D(&td, nullptr, &d3d11FrameTex)) ||
        FAILED(d3d11Dev->CreateRenderTargetView(d3d11FrameTex.Get(), nullptr, &d3d11FrameRtv))) {
        DestroyWindow(targetHwnd);
        return fail("Failed to create source D3D11 frame texture");
    }

    // 1. Measure native D3D11 Flip-model Present baseline on the same window (if hardware).
    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);
    const double qpcMs = static_cast<double>(qpcFreq.QuadPart) / 1000.0;
    constexpr uint32_t kWarmup = 10;
    constexpr uint32_t kMeasured = 120;

    std::vector<double> nativeIntervalsMs;
    if (isHardware) {
        DXGI_SWAP_CHAIN_DESC1 natDesc = edpe::sanitizeFlipSwapChainDesc(
            W, H, DXGI_FORMAT_R8G8B8A8_UNORM, 3, 0, false);
        ComPtr<IDXGISwapChain1> natSc;
        if (SUCCEEDED(sysFactory->CreateSwapChainForHwnd(
                d3d11Dev.Get(), targetHwnd, &natDesc, nullptr, nullptr, &natSc))) {
            ComPtr<ID3D11Texture2D> natBb;
            natSc->GetBuffer(0, IID_PPV_ARGS(&natBb));
            LARGE_INTEGER prevT{};
            for (uint32_t f = 0; f < kWarmup + kMeasured; ++f) {
                const float col[4] = {0.1f, 0.2f, 0.3f, 1.0f};
                d3d11Ctx->ClearRenderTargetView(d3d11FrameRtv.Get(), col);
                if (natBb) d3d11Ctx->CopyResource(natBb.Get(), d3d11FrameTex.Get());
                natSc->Present(0, 0);
                LARGE_INTEGER nowT{};
                QueryPerformanceCounter(&nowT);
                if (f > kWarmup) {
                    nativeIntervalsMs.push_back(static_cast<double>(nowT.QuadPart - prevT.QuadPart) / qpcMs);
                }
                prevT = nowT;
            }
            natBb.Reset();
            natSc.Reset();
            d3d11Ctx->ClearState();
            d3d11Ctx->Flush();
        }
    }

    // 2. Initialize and test D3D12ProxyPresenter on targetHwnd.
    const uint64_t vramBefore = queryLocalVramBytes(adapter3.Get());
    edpe::D3D12ProxyPresenter presenter;
    if (!presenter.initialize(
            d3d11Dev.Get(), targetHwnd, W, H, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1,
            DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE)) {
        DestroyWindow(targetHwnd);
        return fail("D3D12ProxyPresenter::initialize failed");
    }
    const uint64_t vramAfter = queryLocalVramBytes(adapter3.Get());

    if (presenter.bufferCount() != 3 || presenter.format() != DXGI_FORMAT_R8G8B8A8_UNORM) {
        presenter.shutdown();
        DestroyWindow(targetHwnd);
        return fail("D3D12ProxyPresenter did not sanitize SwapChain format/bufferCount");
    }

    std::vector<double> d12IntervalsMs;
    std::vector<double> d12CpuCallMs;
    std::vector<double> d12GpuCopyMs;
    LARGE_INTEGER prevT{};
    for (uint32_t f = 0; f < kWarmup + (isHardware ? kMeasured : 10u); ++f) {
        const float v = static_cast<float>(f % 60) / 60.0f;
        const float col[4] = {v, 0.4f, 0.8f - v * 0.5f, 1.0f};
        d3d11Ctx->ClearRenderTargetView(d3d11FrameRtv.Get(), col);

        LARGE_INTEGER t0{}, t1{};
        QueryPerformanceCounter(&t0);
        HRESULT phr = E_FAIL;
        double gpuMs = 0.0;
        if (!presenter.presentFromD3D11Texture(ctx4.Get(), d3d11FrameTex.Get(), 0, 0, &phr, &gpuMs)) {
            presenter.shutdown();
            DestroyWindow(targetHwnd);
            return fail("presentFromD3D11Texture failed");
        }
        QueryPerformanceCounter(&t1);

        if (f >= kWarmup) {
            d12CpuCallMs.push_back(static_cast<double>(t1.QuadPart - t0.QuadPart) / qpcMs);
            if (gpuMs > 0.0) d12GpuCopyMs.push_back(gpuMs);
            if (f > kWarmup) {
                d12IntervalsMs.push_back(static_cast<double>(t1.QuadPart - prevT.QuadPart) / qpcMs);
            }
        }
        prevT = t1;
    }

    // 3. ResizeBuffers + ColorSpace stress test.
    const uint32_t resizeW = isHardware ? 1920u : 128u;
    const uint32_t resizeH = isHardware ? 1080u : 128u;
    if (!presenter.resizeBuffers(3, resizeW, resizeH, DXGI_FORMAT_R8G8B8A8_UNORM, 0)) {
        presenter.shutdown();
        DestroyWindow(targetHwnd);
        return fail("D3D12ProxyPresenter::resizeBuffers(1080p) failed");
    }
    if (!presenter.setColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)) {
        presenter.shutdown();
        DestroyWindow(targetHwnd);
        return fail("D3D12ProxyPresenter::setColorSpace failed");
    }

    td.Width = resizeW;
    td.Height = resizeH;
    ComPtr<ID3D11Texture2D> resizedTex;
    if (FAILED(d3d11Dev->CreateTexture2D(&td, nullptr, &resizedTex))) {
        presenter.shutdown();
        DestroyWindow(targetHwnd);
        return fail("Failed to create resized source texture");
    }
    for (int i = 0; i < 6; ++i) {
        if (!presenter.presentFromD3D11Texture(ctx4.Get(), resizedTex.Get(), 0, 0)) {
            presenter.shutdown();
            DestroyWindow(targetHwnd);
            return fail("presentFromD3D11Texture failed after ResizeBuffers");
        }
    }

    if (!presenter.resizeBuffers(3, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, 0)) {
        presenter.shutdown();
        DestroyWindow(targetHwnd);
        return fail("D3D12ProxyPresenter::resizeBuffers(restore) failed");
    }
    if (!presenter.presentFromD3D11Texture(ctx4.Get(), d3d11FrameTex.Get(), 0, 0)) {
        presenter.shutdown();
        DestroyWindow(targetHwnd);
        return fail("presentFromD3D11Texture failed after restore ResizeBuffers");
    }

    std::printf("D3D12ProxyPresenter test PASSED on %ls (%ux%u, tearing=%d).\n",
                adapterName.c_str(), W, H, presenter.isTearingSupported() ? 1 : 0);

    if (isHardware) {
        const double vramDeltaMb =
            vramAfter >= vramBefore ? static_cast<double>(vramAfter - vramBefore) / (1024.0 * 1024.0) : 0.0;
        std::printf("=== Phase 13B D3D12 Proxy Swapchain Benchmark (%ux%u, %u frames) ===\n",
                    W, H, kMeasured);
        std::printf("Adapter: %ls\n", adapterName.c_str());
        std::printf("Swapchain + Shared Input VRAM delta: %.2f MB\n", vramDeltaMb);
        std::printf("Native D3D11 Flip Present interval: mean = %.4f ms (stddev = %.4f ms)\n",
                    meanOf(nativeIntervalsMs), stddevOf(nativeIntervalsMs));
        std::printf("D3D12 Proxy Present interval:       mean = %.4f ms (stddev = %.4f ms)\n",
                    meanOf(d12IntervalsMs), stddevOf(d12IntervalsMs));
        std::printf("D3D12 Proxy Present CPU call cost:  mean = %.4f ms (stddev = %.4f ms)\n",
                    meanOf(d12CpuCallMs), stddevOf(d12CpuCallMs));
        std::printf("D3D12 CopyResource GPU cost (%zu samples): mean = %.4f ms\n",
                    d12GpuCopyMs.size(), meanOf(d12GpuCopyMs));
    }

    presenter.shutdown();
    dummySc.Reset();
    dummyWnd.destroy();

    // 4. Runtime Toggle Simulation: parentHwnd owns native D3D11 Flip swapchain; childHwnd (WS_CHILD)
    // mounts a D3D12 Flip proxy swapchain on top without destroying the game's D3D11 swapchain,
    // presents via D3D12, resizes cleanly, then tears down back to parent D3D11 Present().
    if (isHardware) {
        DXGI_SWAP_CHAIN_DESC1 natDesc = edpe::sanitizeFlipSwapChainDesc(
            W, H, DXGI_FORMAT_R8G8B8A8_UNORM, 3, 0, false);
        ComPtr<IDXGISwapChain1> parentD11Sc;
        if (FAILED(sysFactory->CreateSwapChainForHwnd(
                d3d11Dev.Get(), targetHwnd, &natDesc, nullptr, nullptr, &parentD11Sc))) {
            DestroyWindow(targetHwnd);
            return fail("Failed to create parent D3D11 swapchain for child-overlay test");
        }
        HWND childHwnd = CreateWindowExW(
            WS_EX_TRANSPARENT | WS_EX_NOPARENTNOTIFY | WS_EX_NOACTIVATE,
            wc.lpszClassName,
            L"EDPE_ChildOverlay",
            WS_CHILD | WS_VISIBLE | WS_DISABLED,
            0, 0, static_cast<int>(W), static_cast<int>(H),
            targetHwnd, nullptr, wc.hInstance, nullptr);
        if (!childHwnd) {
            DestroyWindow(targetHwnd);
            return fail("CreateWindowExW(WS_CHILD) failed");
        }

        edpe::D3D12ProxyPresenter childPresenter;
        if (!childPresenter.initialize(d3d11Dev.Get(), childHwnd, W, H,
                                       DXGI_FORMAT_R8G8B8A8_UNORM, 3, 0)) {
            DestroyWindow(childHwnd);
            DestroyWindow(targetHwnd);
            return fail("D3D12ProxyPresenter failed on WS_CHILD HWND while parent D3D11 swapchain active");
        }
        for (int f = 0; f < 6; ++f) {
            HRESULT phr = E_FAIL;
            if (!childPresenter.presentFromD3D11Texture(ctx4.Get(), d3d11FrameTex.Get(), 0, 0, &phr) ||
                FAILED(phr)) {
                childPresenter.shutdown();
                DestroyWindow(childHwnd);
                DestroyWindow(targetHwnd);
                return fail("childPresenter.presentFromD3D11Texture failed");
            }
        }
        childPresenter.shutdown();
        DestroyWindow(childHwnd);

        // Verify parent D3D11 swapchain still presents cleanly after child teardown.
        ComPtr<ID3D11Texture2D> pBb;
        if (SUCCEEDED(parentD11Sc->GetBuffer(0, IID_PPV_ARGS(&pBb))) && pBb) {
            d3d11Ctx->CopyResource(pBb.Get(), d3d11FrameTex.Get());
        }
        if (FAILED(parentD11Sc->Present(0, 0))) {
            DestroyWindow(targetHwnd);
            return fail("Parent D3D11 swapchain failed Present after child teardown");
        }
        std::printf("WS_CHILD runtime-toggle overlay test PASSED on %ls.\n", adapterName.c_str());
    }

    DestroyWindow(targetHwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (int rc = runUnitTests(); rc != 0) return rc;
    return runPresenterIntegrationAndBenchmark();
}
