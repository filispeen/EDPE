#include "context_census.h"
#include "fg_integration.h"
#include "system_dll.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;

#define REQUIRE(cond)                                                                          \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::fprintf(stderr, "REQUIRE failed at %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            return 1;                                                                          \
        }                                                                                      \
    } while (0)

namespace {

edpe::FgSceneSnapshot g_testSnap{};

LRESULT CALLBACK fgIntegrationTestWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT fgRes = 0;
    if (edpe::FgRuntime::handleWindowMessage(hwnd, msg, wp, lp, &fgRes)) {
        return fgRes;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool createTexAndSrv(ID3D11Device* dev,
                     uint32_t w,
                     uint32_t h,
                     DXGI_FORMAT texFmt,
                     DXGI_FORMAT srvFmt,
                     UINT bindFlags,
                     const void* initData,
                     uint32_t rowPitch,
                     ComPtr<ID3D11Texture2D>& outTex,
                     ComPtr<ID3D11ShaderResourceView>& outSrv) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = texFmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bindFlags;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = initData;
    init.SysMemPitch = rowPitch;
    if (FAILED(dev->CreateTexture2D(&td, initData ? &init : nullptr, &outTex))) return false;
    if (srvFmt != DXGI_FORMAT_UNKNOWN) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = srvFmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(outTex.Get(), &sd, &outSrv))) return false;
    }
    return true;
}

} // namespace

bool ContextCensusGetFgSnapshot(edpe::FgSceneSnapshot* outSnapshot) {
    if (!outSnapshot) return false;
    *outSnapshot = g_testSnap;
    return g_testSnap.valid;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    HMODULE sysDxgi = LoadSystemDll(L"dxgi.dll");
    REQUIRE(sysDxgi != nullptr);
    using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto createFactory2 = reinterpret_cast<PFN_CreateDXGIFactory2>(
        GetProcAddress(sysDxgi, "CreateDXGIFactory2"));
    REQUIRE(createFactory2 != nullptr);

    ComPtr<IDXGIFactory4> factory;
    REQUIRE(SUCCEEDED(createFactory2(0, IID_PPV_ARGS(&factory))));

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                 D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        std::printf("fg_integration_test: skipped (no hardware D3D11 adapter).\n");
        return 0;
    }

    constexpr uint32_t kW = 1280;
    constexpr uint32_t kH = 720;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &fgIntegrationTestWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"EDPE_FgIntegrationTestWnd";
    RegisterClassExW(&wc);

    HWND mainHwnd = CreateWindowExW(
        0, wc.lpszClassName, L"EDPE FG Integration Test",
        WS_OVERLAPPEDWINDOW, 100, 100, static_cast<int>(kW), static_cast<int>(kH),
        nullptr, nullptr, wc.hInstance, nullptr);
    REQUIRE(mainHwnd != nullptr);

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = kW;
    scDesc.Height = kH;
    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 3;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> sc1;
    REQUIRE(SUCCEEDED(factory->CreateSwapChainForHwnd(
        dev.Get(), mainHwnd, &scDesc, nullptr, nullptr, &sc1)));

    auto& fg = edpe::FgRuntime::instance();
    fg.ensureCapabilitiesProbed(dev.Get());

    const bool amdAvail = fg.isBackendAvailable(edpe::FrameGenerationBackend::AmdFsrFg);
    const bool nvAvail = fg.isBackendAvailable(edpe::FrameGenerationBackend::NvidiaDlssG);
    std::printf("FgRuntime caps: AMD FSR FG=%d ('%S'), NVIDIA DLSS-G=%d ('%S')\n",
                amdAvail ? 1 : 0, fg.backendUnavailableReason(edpe::FrameGenerationBackend::AmdFsrFg),
                nvAvail ? 1 : 0, fg.backendUnavailableReason(edpe::FrameGenerationBackend::NvidiaDlssG));

    // Off mode must bypass proxy presentation cleanly.
    fg.applyConfig(0, 2, false, 0.0f, false);
    REQUIRE(!fg.isMotionNeeded());
    bool presentedViaProxy = true;
    fg.onPresent(sc1.Get(), 0, 0, &presentedViaProxy);
    REQUIRE(!presentedViaProxy);

    // Prepare typeless R32G8X24 depth (matching Elite's DSV #2!) + MV + HUD-less + UI layers.
    ComPtr<ID3D11Texture2D> depthTex;
    ComPtr<ID3D11ShaderResourceView> depthSrv;
    ComPtr<ID3D11Texture2D> mvecTex;
    ComPtr<ID3D11ShaderResourceView> mvecSrv;
    ComPtr<ID3D11Texture2D> hudlessTex;
    ComPtr<ID3D11ShaderResourceView> hudlessSrv;
    ComPtr<ID3D11Texture2D> hdrUiTex, ldrUiTex, multUiTex;
    ComPtr<ID3D11ShaderResourceView> hdrUiSrv, ldrUiSrv, multUiSrv;

    REQUIRE(createTexAndSrv(dev.Get(), kW, kH, DXGI_FORMAT_R32G8X24_TYPELESS,
                            DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
                            D3D11_BIND_SHADER_RESOURCE, nullptr, 0, depthTex, depthSrv));
    REQUIRE(createTexAndSrv(dev.Get(), kW, kH, DXGI_FORMAT_R16G16_FLOAT,
                            DXGI_FORMAT_R16G16_FLOAT,
                            D3D11_BIND_SHADER_RESOURCE, nullptr, 0, mvecTex, mvecSrv));
    REQUIRE(createTexAndSrv(dev.Get(), kW, kH, DXGI_FORMAT_R8G8B8A8_UNORM,
                            DXGI_FORMAT_R8G8B8A8_UNORM,
                            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                            nullptr, 0, hudlessTex, hudlessSrv));
    REQUIRE(createTexAndSrv(dev.Get(), kW, kH, DXGI_FORMAT_R16G16B16A16_FLOAT,
                            DXGI_FORMAT_R16G16B16A16_FLOAT,
                            D3D11_BIND_SHADER_RESOURCE, nullptr, 0, hdrUiTex, hdrUiSrv));
    REQUIRE(createTexAndSrv(dev.Get(), kW, kH, DXGI_FORMAT_R8G8B8A8_UNORM,
                            DXGI_FORMAT_R8G8B8A8_UNORM,
                            D3D11_BIND_SHADER_RESOURCE, nullptr, 0, ldrUiTex, ldrUiSrv));
    REQUIRE(createTexAndSrv(dev.Get(), kW, kH, DXGI_FORMAT_R8G8B8A8_UNORM,
                            DXGI_FORMAT_R8G8B8A8_UNORM,
                            D3D11_BIND_SHADER_RESOURCE, nullptr, 0, multUiTex, multUiSrv));

    edpe::CameraProjection cam{};
    cam.worldFromView[0] = 1.0f;
    cam.worldFromView[5] = 1.0f;
    cam.worldFromView[10] = 1.0f;
    cam.scaleX = 1.0f;
    cam.scaleY = static_cast<float>(kW) / static_cast<float>(kH);
    cam.depthB = 0.025f;

    g_testSnap.valid = true;
    g_testSnap.hudlessColorTex = hudlessTex.Get();
    g_testSnap.depthSrv = depthSrv.Get();
    g_testSnap.motionVectorsTex = mvecTex.Get();
    g_testSnap.uiHdrSrv = hdrUiSrv.Get();
    g_testSnap.uiLdrSrv = ldrUiSrv.Get();
    g_testSnap.uiMultSrv = multUiSrv.Get();
    g_testSnap.renderWidth = kW;
    g_testSnap.renderHeight = kH;
    g_testSnap.outputWidth = kW;
    g_testSnap.outputHeight = kH;
    g_testSnap.frameTimeMs = 16.6667f;
    g_testSnap.cameraNow = cam;
    g_testSnap.cameraPrevious = cam;

    if (amdAvail) {
        // Toggle AMD FSR FG ON dynamically while parent D3D11 swapchain owns mainHwnd.
        fg.applyConfig(1, 2, false, 0.0f, false);
        REQUIRE(fg.isMotionNeeded());

        for (uint64_t f = 1; f <= 12; ++f) {
            g_testSnap.frameIndex = f;
            g_testSnap.reset = (f == 1);
            g_testSnap.isPauseOrMenu = (f == 9 || f == 10);
            bool viaProxy = false;
            fg.onPresent(sc1.Get(), 0, 0, &viaProxy);
            if (!viaProxy) {
                const auto errTel = fg.telemetry();
                std::fprintf(stderr, "onPresent fail f=%llu active=%d initFailed=%d fsBlocked=%d status='%S'\n",
                             static_cast<unsigned long long>(f),
                             errTel.active ? 1 : 0,
                             errTel.initFailed ? 1 : 0,
                             errTel.exclusiveFullscreenBlocked ? 1 : 0,
                             errTel.statusMessage);
            }
            REQUIRE(viaProxy);
        }

        const auto tel = fg.telemetry();
        REQUIRE(tel.active);
        REQUIRE(tel.activeBackend == edpe::FrameGenerationBackend::AmdFsrFg);
        std::printf("FgRuntime AMD FSR FG active: totalGen=%llu copyMs=%.3f status='%S'\n",
                    static_cast<unsigned long long>(tel.totalGeneratedFrames),
                    tel.d3d12CopyMs, tel.statusMessage);

        // Simulate ResizeBuffers -> clean teardown + automatic recovery on next Present.
        fg.onResize(sc1.Get());
        REQUIRE(SUCCEEDED(sc1->ResizeBuffers(3, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 0)));
        g_testSnap.valid = false; // Simulate menu/loading frame right after resize (fail-open 1x proxy)
        bool viaProxyAfterResize = false;
        fg.onPresent(sc1.Get(), 0, 0, &viaProxyAfterResize);
        REQUIRE(viaProxyAfterResize);
        REQUIRE(fg.telemetry().active);
        REQUIRE(fg.telemetry().pausedForMenu);

        // Toggle FG OFF dynamically back to parent D3D11 swapchain.
        fg.applyConfig(0, 2, false, 0.0f, false);
        bool viaProxyOff = true;
        fg.onPresent(sc1.Get(), 0, 0, &viaProxyOff);
        REQUIRE(!viaProxyOff);
        REQUIRE(!fg.telemetry().active);
        REQUIRE(SUCCEEDED(sc1->Present(0, 0)));
    }

    // Verify NVIDIA DLSS-G selection either runs cleanly (Ada RTX 40+) or fails open cleanly (Ampere RTX 3060).
    std::printf("Testing NVIDIA DLSS-G fail-open / live path...\n");
    fg.applyConfig(2, 2, false, 0.0f, true);
    bool nvViaProxy = false;
    fg.onPresent(sc1.Get(), 0, 0, &nvViaProxy);
    std::printf("NVIDIA onPresent done: nvViaProxy=%d initFailed=%d status='%S'\n",
                nvViaProxy ? 1 : 0,
                fg.telemetry().initFailed ? 1 : 0,
                fg.telemetry().statusMessage);
    if (!nvAvail) {
        REQUIRE(!nvViaProxy);
        REQUIRE(fg.telemetry().initFailed);
        REQUIRE(SUCCEEDED(sc1->Present(0, 0)));
    } else {
        REQUIRE(nvViaProxy);
    }

    std::printf("Testing teardown...\n");
    fg.applyConfig(0, 2, false, 0.0f, false);
    fg.onSwapChainRelease(sc1.Get());
    fg.shutdown();
    sc1.Reset();
    DestroyWindow(mainHwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::printf("fg_integration_test passed.\n");
    return 0;
}
