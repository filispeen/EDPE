#include "amd_fsr_fg_backend.h"
#include "d3d12_presenter.h"
#include "fg_backend.h"
#include "fg_ui_pack_pass.h"
#include "system_dll.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

LRESULT CALLBACK fsrFgTestWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int fail(const char* msg) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

bool createHardwareDevice(ComPtr<ID3D11Device>& outDevice,
                          ComPtr<ID3D11DeviceContext>& outContext,
                          std::wstring& outName) {
    HMODULE sysDxgi = LoadSystemDll(L"dxgi.dll");
    using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    auto createFactory1 = sysDxgi
        ? reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(sysDxgi, "CreateDXGIFactory1"))
        : nullptr;
    if (!createFactory1) return false;

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(createFactory1(IID_PPV_ARGS(&factory)))) return false;

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
                outName = desc.Description;
                return true;
            }
            outContext.Reset();
            outDevice.Reset();
        }
    }
    return false;
}

struct FgCbState {
    edpe::FrameGenerationManager* mgr = nullptr;
    edpe::FrameGenerationInputs* inputs = nullptr;
    bool enabled = false;
    bool ok = false;
};

bool prePresentFgHook(ID3D12GraphicsCommandList* cmdList, void* userCtx) {
    auto* st = static_cast<FgCbState*>(userCtx);
    if (!st || !st->mgr || !st->inputs) return false;
    st->ok = st->mgr->evaluate(cmdList, *st->inputs, st->enabled);
    return st->ok;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // 1. Always verify clean construction & fail-open behavior on null presenter.
    {
        auto backend = std::make_unique<edpe::AmdFsrFgBackend>();
        if (backend->initialize(nullptr)) {
            return fail("AmdFsrFgBackend::initialize(nullptr) must return false");
        }
        edpe::FrameGenerationManager mgr;
        mgr.registerBackend(std::move(backend));
        if (mgr.selectBackend(edpe::FrameGenerationBackend::AmdFsrFg, nullptr)) {
            return fail("FrameGenerationManager must fail-open on null presenter");
        }
        mgr.shutdown();
    }

    edpe::AmdFsrFgBackend probeBackend;
    if (!probeBackend.isAvailable()) {
        std::printf("AMD FSR FG skipped: %ls\n", probeBackend.unavailableReason());
        return 0;
    }

    ComPtr<ID3D11Device> d3d11Dev;
    ComPtr<ID3D11DeviceContext> d3d11Ctx;
    std::wstring adapterName;
    if (!createHardwareDevice(d3d11Dev, d3d11Ctx, adapterName)) {
        std::printf("AMD FSR FG live test skipped: no hardware D3D12 adapter.\n");
        return 0;
    }

    ComPtr<ID3D11DeviceContext4> ctx4;
    if (FAILED(d3d11Ctx.As(&ctx4))) return fail("QueryInterface(ID3D11DeviceContext4) failed");

    constexpr uint32_t W = 2560;
    constexpr uint32_t H = 1440;

    WNDCLASSW wc{};
    wc.lpfnWndProc = fsrFgTestWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"EDPE_AmdFsrFg_TestWnd";
    RegisterClassW(&wc);

    HWND targetHwnd = CreateWindowExW(
        0, wc.lpszClassName, L"EDPE Phase 14 AMD FSR FG Test", WS_OVERLAPPEDWINDOW,
        0, 0, static_cast<int>(W), static_cast<int>(H),
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!targetHwnd) return fail("CreateWindowExW failed");

    edpe::D3D12ProxyPresenter presenter;
    if (!presenter.initialize(d3d11Dev.Get(), targetHwnd, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, 3, 0)) {
        DestroyWindow(targetHwnd);
        return fail("D3D12ProxyPresenter::initialize failed");
    }

    auto fsrFg = std::make_unique<edpe::AmdFsrFgBackend>();
    auto* fsrFgPtr = fsrFg.get();
    edpe::FrameGenerationManager fgMgr;
    fgMgr.registerBackend(std::move(fsrFg));
    if (!fgMgr.selectBackend(edpe::FrameGenerationBackend::AmdFsrFg, &presenter)) {
        presenter.shutdown();
        DestroyWindow(targetHwnd);
        return fail("FrameGenerationManager::selectBackend(AmdFsrFg) failed");
    }

    // Create shared inputs: hudlessColor, depth (R32F), motionVectors (R16G16F), packedUi (RGBA8).
    edpe::SharedBridgeTexture sharedHudless, sharedDepth, sharedMotion, sharedUi;
    if (!presenter.bridge().createSharedTexture(
            W, H, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, sharedHudless) ||
        !presenter.bridge().createSharedTexture(
            W, H, DXGI_FORMAT_R32_FLOAT,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, sharedDepth) ||
        !presenter.bridge().createSharedTexture(
            W, H, DXGI_FORMAT_R16G16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, sharedMotion) ||
        !presenter.bridge().createSharedTexture(
            W, H, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, sharedUi)) {
        fgMgr.shutdown();
        presenter.shutdown();
        DestroyWindow(targetHwnd);
        return fail("Failed to create shared FG textures");
    }

    ComPtr<ID3D11RenderTargetView> hudlessRtv, depthRtv, motionRtv;
    ComPtr<ID3D11UnorderedAccessView> uiUav;
    if (FAILED(d3d11Dev->CreateRenderTargetView(sharedHudless.d3d11.Get(), nullptr, &hudlessRtv)) ||
        FAILED(d3d11Dev->CreateRenderTargetView(sharedDepth.d3d11.Get(), nullptr, &depthRtv)) ||
        FAILED(d3d11Dev->CreateRenderTargetView(sharedMotion.d3d11.Get(), nullptr, &motionRtv)) ||
        FAILED(d3d11Dev->CreateUnorderedAccessView(sharedUi.d3d11.Get(), nullptr, &uiUav))) {
        return fail("Failed to create D3D11 views for shared FG textures");
    }

    // Pack a sample 3-layer UI via FgUiPackPass directly into sharedUi.
    edpe::FgUiPackPass uiPack;
    if (!uiPack.initialize(d3d11Dev.Get(), d3d11Ctx.Get())) {
        return fail("FgUiPackPass::initialize failed");
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ComPtr<ID3D11Texture2D> hdrUiTex, ldrUiTex;
    ComPtr<ID3D11ShaderResourceView> hdrUiSrv, ldrUiSrv;
    d3d11Dev->CreateTexture2D(&td, nullptr, &hdrUiTex);
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d3d11Dev->CreateTexture2D(&td, nullptr, &ldrUiTex);
    d3d11Dev->CreateShaderResourceView(hdrUiTex.Get(), nullptr, &hdrUiSrv);
    d3d11Dev->CreateShaderResourceView(ldrUiTex.Get(), nullptr, &ldrUiSrv);
    uiPack.pack(hdrUiSrv.Get(), ldrUiSrv.Get(), nullptr, false, W, H,
                sharedUi.d3d11.Get(), uiUav.Get());

    edpe::CameraProjection camPrev{{1,0,0,0, 0,1,0,0, 0,0,1,0}, 0.9742786f, 1.7320508f, 0.025f};
    edpe::FrameGenerationInputs fgInputs{};
    fgInputs.hudlessColor = sharedHudless.d3d12.Get();
    fgInputs.packedUiColorAndAlpha = sharedUi.d3d12.Get();
    fgInputs.depth = sharedDepth.d3d12.Get();
    fgInputs.motionVectors = sharedMotion.d3d12.Get();
    fgInputs.renderWidth = W;
    fgInputs.renderHeight = H;
    fgInputs.outputWidth = W;
    fgInputs.outputHeight = H;
    fgInputs.frameTimeMs = 16.666f;
    fgInputs.viewSpaceToMeters = 1.0f;
    fgInputs.cameraPrevious = camPrev;
    fgInputs.cameraNow = camPrev;

    FgCbState cbState{&fgMgr, &fgInputs, true, false};
    presenter.setPrePresentCallback(&prePresentFgHook, &cbState);

    LARGE_INTEGER qpcFreq{}, tStart{}, tEnd{};
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&tStart);

    constexpr uint32_t kFrames = 30;
    for (uint32_t f = 1; f <= kFrames; ++f) {
        const float t = static_cast<float>(f) / static_cast<float>(kFrames);
        const float bg[4] = {0.1f + 0.6f * t, 0.25f, 0.7f - 0.5f * t, 1.0f};
        const float z[4] = {0.01f, 0.01f, 0.01f, 0.01f};
        const float mv[4] = {1.5f, 0.0f, 0.0f, 0.0f};
        d3d11Ctx->ClearRenderTargetView(hudlessRtv.Get(), bg);
        d3d11Ctx->ClearRenderTargetView(depthRtv.Get(), z);
        d3d11Ctx->ClearRenderTargetView(motionRtv.Get(), mv);

        fgInputs.frameIndex = f;
        fgInputs.reset = (f == 1);
        fgInputs.cameraPrevious = fgInputs.cameraNow;
        fgInputs.cameraNow.worldFromView[3] += 0.2f;

        HRESULT phr = E_FAIL;
        double copyMs = 0.0;
        if (!presenter.presentFromD3D11Texture(
                ctx4.Get(), sharedHudless.d3d11.Get(), 0, 0, &phr, &copyMs) ||
            !cbState.ok) {
            presenter.setPrePresentCallback(nullptr, nullptr);
            fgMgr.shutdown();
            presenter.shutdown();
            DestroyWindow(targetHwnd);
            return fail("AMD FSR FG frame presentation failed");
        }
    }

    QueryPerformanceCounter(&tEnd);
    const double wallMs =
        static_cast<double>(tEnd.QuadPart - tStart.QuadPart) /
        (static_cast<double>(qpcFreq.QuadPart) / 1000.0);

    uint64_t fgBytes = 0, scBytes = 0;
    fsrFgPtr->queryGpuMemoryUsage(&fgBytes, &scBytes);

    const auto& diag = fgMgr.diagnostics();
    if (!diag.enabled || diag.generatedFramesThisCycle != 1 ||
        diag.totalGeneratedFrames != kFrames) {
        return fail("AMD FSR FG diagnostics mismatch");
    }

    std::printf("=== Phase 14 AMD FSR FG Prototype Test (%ux%u, %u frames) ===\n", W, H, kFrames);
    std::printf("Adapter: %ls | Provider: %s\n", adapterName.c_str(), fsrFgPtr->providerVersionString());
    std::printf("FG Context VRAM: %.2f MB | FG SwapChain VRAM: %.2f MB\n",
                static_cast<double>(fgBytes) / (1024.0 * 1024.0),
                static_cast<double>(scBytes) / (1024.0 * 1024.0));
    std::printf("Total wall (%u rendered + %llu generated frames): %.2f ms (%.2f ms/frame)\n",
                kFrames, static_cast<unsigned long long>(diag.totalGeneratedFrames),
                wallMs, wallMs / static_cast<double>(kFrames));
    std::printf("Diagnostics: %ls | Rendered FPS=%.1f | Presented FPS=%.1f\n",
                diag.statusMessage, diag.renderedFps, diag.presentedFps);

    presenter.setPrePresentCallback(nullptr, nullptr);
    uiPack.shutdown();
    sharedUi.reset();
    sharedMotion.reset();
    sharedDepth.reset();
    sharedHudless.reset();
    fgMgr.shutdown();
    presenter.shutdown();
    DestroyWindow(targetHwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
