#include "fg_backend.h"
#include "fg_ui_pack_pass.h"
#include "nvidia_sl_fg_backend.h"

#if defined(EDPE_ENABLE_STREAMLINE) && EDPE_ENABLE_STREAMLINE
#include <sl.h>
#include <sl_consts.h>
#include <sl_core_api.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_version.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <string>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

#define REQUIRE(cond)                                                                          \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::fprintf(stderr, "REQUIRE failed at %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            return 1;                                                                          \
        }                                                                                      \
    } while (0)

namespace {

bool createTestTexture2D(ID3D11Device* dev,
                           uint32_t w,
                           uint32_t h,
                           DXGI_FORMAT fmt,
                           UINT bindFlags,
                           const void* initialData,
                           uint32_t rowPitchBytes,
                           ID3D11Texture2D** outTex) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bindFlags;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = initialData;
    init.SysMemPitch = rowPitchBytes;
    return SUCCEEDED(dev->CreateTexture2D(&desc, initialData ? &init : nullptr, outTex));
}

int runUnitChecks() {
    REQUIRE(std::strcmp(edpe::NvidiaStreamlineFgBackend::dlssgStatusToString(0), "eOk") == 0);
    REQUIRE(std::strcmp(edpe::NvidiaStreamlineFgBackend::dlssgStatusToString(1u << 1),
                        "eFailReflexNotDetectedAtRuntime") == 0);
    REQUIRE(std::strcmp(edpe::NvidiaStreamlineFgBackend::dlssgStatusToString(1u << 0),
                        "eFailResolutionTooLow") == 0);
    REQUIRE(std::strcmp(edpe::NvidiaStreamlineFgBackend::dlssgStatusToString(1u << 2),
                        "eFailHDRFormatNotSupported") == 0);
    REQUIRE(std::strcmp(edpe::NvidiaStreamlineFgBackend::dlssgStatusToString(1u << 3),
                        "eFailCommonConstantsInvalid") == 0);
    REQUIRE(std::strcmp(edpe::NvidiaStreamlineFgBackend::dlssgStatusToString(1u << 4),
                        "eFailGetCurrentBackBufferIndexNotCalled") == 0);

#if defined(EDPE_ENABLE_STREAMLINE) && EDPE_ENABLE_STREAMLINE
    sl::Constants c{};
    REQUIRE(c.depthInverted == sl::Boolean::eInvalid);
    REQUIRE(SL_VERSION_MAJOR == 2 && SL_VERSION_MINOR == 14 && SL_VERSION_PATCH == 16);
#endif

    edpe::NvidiaStreamlineFgBackend backend;
    REQUIRE(backend.backendType() == edpe::FrameGenerationBackend::NvidiaDlssG);
    uint32_t genCount = 99;
    edpe::FrameGenerationInputs dummy{};
    REQUIRE(!backend.evaluate(nullptr, dummy, true, 1, &genCount));
    REQUIRE(genCount == 0);

    backend.setLowLatencyBoost(true);
    REQUIRE(backend.isLowLatencyBoost());
    backend.setDynamicMfgMode(true, 120.0f);
    REQUIRE(backend.isDynamicMfgEnabled());
    REQUIRE(!backend.isDebugViewEnabled());
    REQUIRE(backend.presentsRealFrame());
    backend.setDebugView(true);
    REQUIRE(backend.isDebugViewEnabled());
    REQUIRE(!backend.presentsRealFrame());
    backend.setDebugView(false);
    REQUIRE(!backend.isDebugViewEnabled());
    REQUIRE(backend.presentsRealFrame());
    return 0;
}

#if defined(EDPE_ENABLE_STREAMLINE) && EDPE_ENABLE_STREAMLINE
int runManualHookingProxyTest(ID3D11Device* dev,
                              ID3D11DeviceContext4* ctx4,
                              edpe::D3D12ProxyPresenter& presenter) {
    wchar_t exePath[MAX_PATH]{};
    const DWORD pathLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    REQUIRE(pathLen > 0 && pathLen < MAX_PATH);
    std::wstring exeDir(exePath, pathLen);
    const size_t slash = exeDir.find_last_of(L"\\/");
    REQUIRE(slash != std::wstring::npos);
    exeDir.resize(slash);

    const std::wstring slPath = exeDir + L"\\sl.interposer.dll";
    REQUIRE(GetFileAttributesW(slPath.c_str()) != INVALID_FILE_ATTRIBUTES);

    HMODULE slMod = LoadLibraryW(slPath.c_str());
    REQUIRE(slMod != nullptr);

    auto slInit = reinterpret_cast<PFun_slInit*>(GetProcAddress(slMod, "slInit"));
    auto slShutdown = reinterpret_cast<PFun_slShutdown*>(GetProcAddress(slMod, "slShutdown"));
    auto slSetD3DDevice = reinterpret_cast<PFun_slSetD3DDevice*>(GetProcAddress(slMod, "slSetD3DDevice"));
    auto slUpgradeInterface =
        reinterpret_cast<PFun_slUpgradeInterface*>(GetProcAddress(slMod, "slUpgradeInterface"));
    auto slGetNewFrameToken =
        reinterpret_cast<PFun_slGetNewFrameToken*>(GetProcAddress(slMod, "slGetNewFrameToken"));
    auto slGetFeatureFunction =
        reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(slMod, "slGetFeatureFunction"));
    REQUIRE(slInit && slShutdown && slSetD3DDevice && slUpgradeInterface &&
            slGetNewFrameToken && slGetFeatureFunction);

    const wchar_t* pluginPaths[] = {exeDir.c_str()};
    const sl::Feature features[] = {sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = pluginPaths;
    pref.numPathsToPlugins = 1u;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                 sl::PreferenceFlags::eDisableDebugText |
                 sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(features));
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0";
    pref.projectId = "48d353f3-d07b-4048-876b-09f8622f5a27";
    pref.renderAPI = sl::RenderAPI::eD3D12;

    REQUIRE(slInit(pref, sl::kSDKVersion) == sl::Result::eOk);
    REQUIRE(slSetD3DDevice(presenter.bridge().d3d12Device()) == sl::Result::eOk);

    void* fn = nullptr;
    PFun_slReflexSetOptions* slReflexSetOptions = nullptr;
    PFun_slReflexSleep* slReflexSleep = nullptr;
    PFun_slPCLSetMarker* slPCLSetMarker = nullptr;
    if (slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", fn) == sl::Result::eOk) {
        slReflexSetOptions = reinterpret_cast<PFun_slReflexSetOptions*>(fn);
    }
    if (slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", fn) == sl::Result::eOk) {
        slReflexSleep = reinterpret_cast<PFun_slReflexSleep*>(fn);
    }
    if (slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", fn) == sl::Result::eOk) {
        slPCLSetMarker = reinterpret_cast<PFun_slPCLSetMarker*>(fn);
    }
    if (slReflexSetOptions) {
        sl::ReflexOptions ro{};
        ro.mode = sl::ReflexMode::eLowLatency;
        REQUIRE(slReflexSetOptions(ro) == sl::Result::eOk);
    }

    // Regression check for GitHub Issue #23: slUpgradeInterface does not accept ID3D12CommandQueue*.
    void* rawQueuePtr = presenter.bridge().commandQueue();
    REQUIRE(slUpgradeInterface(&rawQueuePtr) == sl::Result::eErrorUnsupportedInterface);

    // Correct ManualHooking path: upgrade ID3D12Device + IDXGIFactory, then CreateCommandQueue.
    void* devPtr = presenter.bridge().d3d12Device();
    REQUIRE(slUpgradeInterface(&devPtr) == sl::Result::eOk);
    REQUIRE(devPtr != nullptr);
    ComPtr<ID3D12Device> slProxyDevice;
    slProxyDevice.Attach(static_cast<ID3D12Device*>(devPtr));

    void* factoryPtr = presenter.dxgiFactory();
    REQUIRE(slUpgradeInterface(&factoryPtr) == sl::Result::eOk);
    REQUIRE(factoryPtr != nullptr);
    ComPtr<IDXGIFactory4> slProxyFactory;
    slProxyFactory.Attach(static_cast<IDXGIFactory4*>(factoryPtr));

    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    ComPtr<ID3D12CommandQueue> slProxyQueue;
    REQUIRE(SUCCEEDED(slProxyDevice->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&slProxyQueue))));
    REQUIRE(slProxyQueue != nullptr);

    IDXGISwapChain4* rawSc = presenter.detachSwapChainForWrap();
    if (rawSc) rawSc->Release();
    REQUIRE(presenter.bridge().setOverrideCommandQueue(slProxyQueue.Get()));

    const UINT cleanFlags =
        presenter.swapChainFlags() & ~DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    const DXGI_SWAP_CHAIN_DESC1 scDesc = edpe::sanitizeFlipSwapChainDesc(
        presenter.width(), presenter.height(),
        presenter.format(), presenter.bufferCount(),
        cleanFlags, presenter.isTearingSupported());
    ComPtr<IDXGISwapChain1> proxySc1;
    REQUIRE(SUCCEEDED(slProxyFactory->CreateSwapChainForHwnd(
        slProxyQueue.Get(), presenter.targetHwnd(), &scDesc, nullptr, nullptr, &proxySc1)));
    ComPtr<IDXGISwapChain4> proxySc4;
    REQUIRE(SUCCEEDED(proxySc1.As(&proxySc4)));
    proxySc1.Reset();
    REQUIRE(presenter.attachWrappedSwapChain(proxySc4.Detach()));

    ComPtr<ID3D11Texture2D> colorTex;
    REQUIRE(createTestTexture2D(dev, presenter.width(), presenter.height(),
                                DXGI_FORMAT_R8G8B8A8_UNORM,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                nullptr, 0, &colorTex));

    struct MarkerCtx {
        PFun_slPCLSetMarker* setMarker = nullptr;
        sl::FrameToken* token = nullptr;
    } markerCtx{slPCLSetMarker, nullptr};

    presenter.setPresentMarkerHooks(
        [](void* p) {
            auto* mc = static_cast<MarkerCtx*>(p);
            if (mc && mc->setMarker && mc->token) {
                mc->setMarker(sl::PCLMarker::ePresentStart, *mc->token);
            }
        },
        [](void* p) {
            auto* mc = static_cast<MarkerCtx*>(p);
            if (mc && mc->setMarker && mc->token) {
                mc->setMarker(sl::PCLMarker::ePresentEnd, *mc->token);
            }
        },
        &markerCtx);

    for (uint32_t frameIdx = 1; frameIdx <= 3; ++frameIdx) {
        sl::FrameToken* token = nullptr;
        REQUIRE(slGetNewFrameToken(token, &frameIdx) == sl::Result::eOk);
        REQUIRE(token != nullptr);
        markerCtx.token = token;
        if (slReflexSleep) REQUIRE(slReflexSleep(*token) == sl::Result::eOk);
        if (slPCLSetMarker) {
            slPCLSetMarker(sl::PCLMarker::eSimulationStart, *token);
            slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *token);
            slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *token);
            slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *token);
        }
        HRESULT phr = S_OK;
        REQUIRE(presenter.presentFromD3D11Texture(ctx4, colorTex.Get(), 0, 0, &phr));
        REQUIRE(SUCCEEDED(phr));
    }

    presenter.setPresentMarkerHooks(nullptr, nullptr, nullptr);
    presenter.bridge().waitGpuIdle();
    if (slReflexSetOptions) {
        sl::ReflexOptions offOpts{};
        offOpts.mode = sl::ReflexMode::eOff;
        slReflexSetOptions(offOpts);
    }
    IDXGISwapChain4* proxySc = presenter.detachSwapChainForWrap();
    if (proxySc) proxySc->Release();
    REQUIRE(presenter.bridge().setOverrideCommandQueue(nullptr));
    slProxyQueue.Reset();
    slProxyDevice.Reset();
    slProxyFactory.Reset();
    REQUIRE(slShutdown() == sl::Result::eOk);
    FreeLibrary(slMod);
    REQUIRE(presenter.recreatePlainSwapChain());

    HRESULT plainHr = S_OK;
    REQUIRE(presenter.presentFromD3D11Texture(ctx4, colorTex.Get(), 0, 0, &plainHr));
    REQUIRE(SUCCEEDED(plainHr));

    std::printf(
        "Streamline ManualHooking proxy regression test PASSED "
        "(rawQueue=eErrorUnsupportedInterface, slProxyDevice::CreateCommandQueue + 3 proxy presents + plain restore).\n");
    return 0;
}
#endif

} // namespace

int main() {
    if (runUnitChecks() != 0) return 1;

#if !defined(EDPE_ENABLE_STREAMLINE) || !EDPE_ENABLE_STREAMLINE
    std::printf("nvidia_sl_fg_test: skipped Streamline checks (EDPE_ENABLE_STREAMLINE=0).\n");
    return 0;
#else
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        std::printf("nvidia_sl_fg_test: skipped GPU checks (no D3D11 hardware device).\n");
        return 0;
    }

    ComPtr<ID3D11DeviceContext4> ctx4;
    REQUIRE(SUCCEEDED(ctx.As(&ctx4)));

    constexpr uint32_t kWidth = 2560;
    constexpr uint32_t kHeight = 1440;

    edpe::HiddenDummyWindow window;
    REQUIRE(window.create());

    edpe::D3D12ProxyPresenter presenter;
    REQUIRE(presenter.initialize(dev.Get(), window.hwnd(), kWidth, kHeight,
                                 DXGI_FORMAT_R8G8B8A8_UNORM, 3, 0));

    edpe::StreamlineStatusSnapshot snap{};
    const bool probeDlssg = edpe::NvidiaStreamlineFgBackend::probeCapabilities(
        presenter.bridge().d3d12Device(), &snap);

    std::printf(
        "Streamline probe: interposerFound=%d signatureVerified=%d slVersion=%s "
        "reflexSupported=%d (rc=%d ver=%s) pclSupported=%d dlssgSupported=%d (rc=%d ver=%s) "
        "maxGen=%u dynamicMfg=%d reason='%S'\n",
        snap.interposerFound ? 1 : 0,
        snap.signatureVerified ? 1 : 0,
        snap.slVersion,
        snap.reflexSupported ? 1 : 0,
        snap.lastReflexSupportCode,
        snap.reflexVersion,
        snap.pclSupported ? 1 : 0,
        snap.dlssgSupported ? 1 : 0,
        snap.lastDlssgSupportCode,
        snap.dlssgVersion,
        snap.maxFramesToGenerate,
        snap.dynamicMfgSupported ? 1 : 0,
        snap.unavailableReason);

    REQUIRE(snap.interposerFound);
    REQUIRE(snap.signatureVerified);
    REQUIRE(!snap.slInitialized); // Probe must shut SL down cleanly afterwards.

    // Check adapter vendor so NVIDIA capability behavior is explicitly validated.
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDesc{};
    if (SUCCEEDED(dev.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
        adapter->GetDesc(&adapterDesc);
        std::printf("GPU Adapter: %S (VendorId=0x%04X DeviceId=0x%04X)\n",
                    adapterDesc.Description, adapterDesc.VendorId, adapterDesc.DeviceId);
        if (adapterDesc.VendorId == 0x10DE) {
            REQUIRE(snap.reflexSupported);
            REQUIRE(snap.pclSupported);
            if (runManualHookingProxyTest(dev.Get(), ctx4.Get(), presenter) != 0) {
                return 1;
            }
        }
    }

    if (probeDlssg) {
        // Full live DLSS-G frame generation verification path (Ada RTX 40+ / Blackwell RTX 50+).
        edpe::NvidiaStreamlineFgBackend backend;
        REQUIRE(backend.initialize(&presenter));
        REQUIRE(backend.isAvailable());
        REQUIRE(backend.maxFramesToGenerate() >= 1);

        ComPtr<ID3D11Texture2D> colorTex;
        ComPtr<ID3D11Texture2D> hudlessTex;
        ComPtr<ID3D11Texture2D> uiTex;
        ComPtr<ID3D11Texture2D> depthTex;
        ComPtr<ID3D11Texture2D> mvecTex;

        REQUIRE(createTestTexture2D(dev.Get(), kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                    nullptr, 0, &colorTex));
        REQUIRE(createTestTexture2D(dev.Get(), kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                    nullptr, 0, &hudlessTex));
        REQUIRE(createTestTexture2D(dev.Get(), kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
                                    D3D11_BIND_SHADER_RESOURCE,
                                    nullptr, 0, &uiTex));
        REQUIRE(createTestTexture2D(dev.Get(), kWidth, kHeight, DXGI_FORMAT_R32_FLOAT,
                                    D3D11_BIND_SHADER_RESOURCE,
                                    nullptr, 0, &depthTex));
        REQUIRE(createTestTexture2D(dev.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16_FLOAT,
                                    D3D11_BIND_SHADER_RESOURCE,
                                    nullptr, 0, &mvecTex));

        edpe::SharedBridgeTexture sharedHudless;
        edpe::SharedBridgeTexture sharedUi;
        edpe::SharedBridgeTexture sharedDepth;
        edpe::SharedBridgeTexture sharedMvec;
        REQUIRE(presenter.bridge().createSharedTexture(
            kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, sharedHudless));
        REQUIRE(presenter.bridge().createSharedTexture(
            kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, sharedUi));
        REQUIRE(presenter.bridge().createSharedTexture(
            kWidth, kHeight, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, sharedDepth));
        REQUIRE(presenter.bridge().createSharedTexture(
            kWidth, kHeight, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, sharedMvec));

        struct HookCtx {
            edpe::NvidiaStreamlineFgBackend* backend = nullptr;
            edpe::FrameGenerationInputs inputs{};
        } hookCtx{&backend, {}};

        edpe::CameraProjection camPrev{};
        camPrev.worldFromView[0] = 1.0f;
        camPrev.worldFromView[5] = 1.0f;
        camPrev.worldFromView[10] = 1.0f;
        camPrev.scaleX = 1.0f;
        camPrev.scaleY = 16.0f / 9.0f;
        camPrev.depthB = 0.05f;

        hookCtx.inputs.hudlessColor = sharedHudless.d3d12.Get();
        hookCtx.inputs.packedUiColorAndAlpha = sharedUi.d3d12.Get();
        hookCtx.inputs.depth = sharedDepth.d3d12.Get();
        hookCtx.inputs.motionVectors = sharedMvec.d3d12.Get();
        hookCtx.inputs.renderWidth = kWidth;
        hookCtx.inputs.renderHeight = kHeight;
        hookCtx.inputs.outputWidth = kWidth;
        hookCtx.inputs.outputHeight = kHeight;
        hookCtx.inputs.frameTimeMs = 16.6667f;
        hookCtx.inputs.cameraNow = camPrev;
        hookCtx.inputs.cameraPrevious = camPrev;

        presenter.setPrePresentCallback(
            [](ID3D12GraphicsCommandList* cmdList, void* userCtx) -> bool {
                auto* hc = static_cast<HookCtx*>(userCtx);
                uint32_t gen = 0;
                return hc->backend->evaluate(cmdList, hc->inputs, true, 1, &gen);
            },
            &hookCtx);

        uint64_t totalPresented = 0;
        for (uint64_t i = 0; i < 10; ++i) {
            ctx4->CopyResource(sharedHudless.d3d11.Get(), hudlessTex.Get());
            ctx4->CopyResource(sharedUi.d3d11.Get(), uiTex.Get());
            ctx4->CopyResource(sharedDepth.d3d11.Get(), depthTex.Get());
            ctx4->CopyResource(sharedMvec.d3d11.Get(), mvecTex.Get());
            hookCtx.inputs.frameIndex = i + 1;
            hookCtx.inputs.reset = (i == 0);
            hookCtx.inputs.isPauseOrMenu = (i == 8);
            HRESULT phr = S_OK;
            REQUIRE(presenter.presentFromD3D11Texture(ctx4.Get(), colorTex.Get(), 0, 0, &phr));
            REQUIRE(SUCCEEDED(phr));
            const auto st = backend.statusSnapshot();
            totalPresented += st.lastPresentedFrames;
            REQUIRE(st.dlssgStatusFlags == 0);
        }

        // Toggle eShowOnlyInterpolatedFrame ON (with and without overlayMenuVisible) then OFF.
        backend.setDebugView(true);
        REQUIRE(!backend.presentsRealFrame());
        for (uint64_t j = 0; j < 4; ++j) {
            ctx4->CopyResource(sharedHudless.d3d11.Get(), hudlessTex.Get());
            ctx4->CopyResource(sharedUi.d3d11.Get(), uiTex.Get());
            ctx4->CopyResource(sharedDepth.d3d11.Get(), depthTex.Get());
            ctx4->CopyResource(sharedMvec.d3d11.Get(), mvecTex.Get());
            hookCtx.inputs.frameIndex = 11 + j;
            hookCtx.inputs.reset = false;
            hookCtx.inputs.isPauseOrMenu = false;
            hookCtx.inputs.overlayMenuVisible = (j == 0);
            HRESULT phr = S_OK;
            REQUIRE(presenter.presentFromD3D11Texture(ctx4.Get(), colorTex.Get(), 0, 0, &phr));
            REQUIRE(SUCCEEDED(phr));
            const auto st = backend.statusSnapshot();
            REQUIRE(st.dlssgStatusFlags == 0);
            REQUIRE(st.lastSlResult == static_cast<int32_t>(sl::Result::eOk));
        }

        backend.setDebugView(false);
        REQUIRE(backend.presentsRealFrame());
        hookCtx.inputs.overlayMenuVisible = false;
        for (uint64_t j = 0; j < 3; ++j) {
            ctx4->CopyResource(sharedHudless.d3d11.Get(), hudlessTex.Get());
            ctx4->CopyResource(sharedUi.d3d11.Get(), uiTex.Get());
            ctx4->CopyResource(sharedDepth.d3d11.Get(), depthTex.Get());
            ctx4->CopyResource(sharedMvec.d3d11.Get(), mvecTex.Get());
            hookCtx.inputs.frameIndex = 15 + j;
            hookCtx.inputs.reset = false;
            hookCtx.inputs.isPauseOrMenu = false;
            HRESULT phr = S_OK;
            REQUIRE(presenter.presentFromD3D11Texture(ctx4.Get(), colorTex.Get(), 0, 0, &phr));
            REQUIRE(SUCCEEDED(phr));
            const auto st = backend.statusSnapshot();
            REQUIRE(st.dlssgStatusFlags == 0);
            REQUIRE(st.lastSlResult == static_cast<int32_t>(sl::Result::eOk));
        }
        std::printf("DLSS-G live loop complete: totalPresented=%llu\n",
                    static_cast<unsigned long long>(totalPresented));

        presenter.setPrePresentCallback(nullptr, nullptr);
        backend.shutdown();
    } else {
        // Capability-driven fail-open path (e.g., Ampere RTX 3060 where DLSS-G is unsupported):
        // Verify initialize() rejects gracefully without breaking D3D12ProxyPresenter's plain swapchain.
        edpe::FrameGenerationManager manager;
        manager.registerBackend(std::make_unique<edpe::NvidiaStreamlineFgBackend>());
        REQUIRE(!manager.selectBackend(edpe::FrameGenerationBackend::NvidiaDlssG, &presenter));
        REQUIRE(manager.selectedBackend() == edpe::FrameGenerationBackend::Off);

        const auto* slBackend = manager.getBackend(edpe::FrameGenerationBackend::NvidiaDlssG);
        REQUIRE(slBackend != nullptr);
        REQUIRE(!slBackend->isAvailable());
        REQUIRE(slBackend->unavailableReason()[0] != L'\0');
        REQUIRE(slBackend->maxFramesToGenerate() == 0);

        ComPtr<ID3D11Texture2D> colorTex;
        REQUIRE(createTestTexture2D(dev.Get(), kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                    nullptr, 0, &colorTex));
        HRESULT phr = S_OK;
        REQUIRE(presenter.presentFromD3D11Texture(ctx4.Get(), colorTex.Get(), 0, 0, &phr));
        REQUIRE(SUCCEEDED(phr));
    }

    presenter.shutdown();
    window.destroy();
    std::printf("nvidia_sl_fg_test passed.\n");
    return 0;
#endif
}
