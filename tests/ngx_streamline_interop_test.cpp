#include "d3d12_bridge.h"
#include "ngx_context.h"
#include "nvidia_sl_fg_backend.h"
#include "temporal_inputs.h"

#if defined(EDPE_ENABLE_STREAMLINE) && EDPE_ENABLE_STREAMLINE
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_version.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <string>
#include <wrl/client.h>

std::atomic<bool> g_is_evaluating_upscaler{false};
std::atomic<int> g_evaluating_upscaler_depth{0};

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
    return SUCCEEDED(dev->CreateTexture2D(&desc, nullptr, outTex));
}

std::wstring getExeDir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L".";
    std::wstring s(buf, len);
    const size_t slash = s.find_last_of(L"\\/");
    return (slash != std::wstring::npos) ? s.substr(0, slash) : std::wstring(L".");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                 D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        std::printf("ngx_streamline_interop_test: skipped (no hardware D3D11 adapter).\n");
        return 0;
    }

    if (!edpe::NgxContext::initialize(dev.Get(), ctx.Get())) {
        std::printf("ngx_streamline_interop_test: skipped (%ls).\n",
                    edpe::NgxContext::get_status_message());
        return 0;
    }

    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDesc{};
    REQUIRE(SUCCEEDED(dev.As(&dxgiDev)));
    REQUIRE(SUCCEEDED(dxgiDev->GetAdapter(&adapter)));
    REQUIRE(SUCCEEDED(adapter->GetDesc(&adapterDesc)));

    constexpr uint32_t kWidth = 1280;
    constexpr uint32_t kHeight = 720;
    ComPtr<ID3D11Texture2D> colorIn;
    ComPtr<ID3D11Texture2D> outTex;
    ComPtr<ID3D11Texture2D> depthIn;
    ComPtr<ID3D11Texture2D> motionIn;
    REQUIRE(createTestTexture2D(dev.Get(), kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
                                D3D11_BIND_SHADER_RESOURCE, &colorIn));
    REQUIRE(createTestTexture2D(dev.Get(), kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
                                D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &outTex));
    REQUIRE(createTestTexture2D(dev.Get(), kWidth, kHeight, DXGI_FORMAT_R32G8X24_TYPELESS,
                                D3D11_BIND_SHADER_RESOURCE, &depthIn));
    REQUIRE(createTestTexture2D(dev.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16_FLOAT,
                                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &motionIn));

    edpe::NvidiaDlssInputParams params{};
    params.motionScaleX = 1.0f;
    params.motionScaleY = 1.0f;
    params.inReset = true;
    params.isHdr = false;
    params.frameTimeMs = 16.6667f;

    // Test 1: F5 menu LUID-only capability probe followed by D3D11 DLAA evaluation.
    edpe::StreamlineStatusSnapshot snap{};
    edpe::NvidiaStreamlineFgBackend::probeCapabilitiesForLuid(adapterDesc.AdapterLuid, &snap);
    REQUIRE(snap.interposerFound);
    REQUIRE(snap.signatureVerified);
    REQUIRE(!snap.slInitialized);
    REQUIRE(snap.reflexSupported);
    REQUIRE(snap.pclSupported);

    REQUIRE(edpe::NgxContext::evaluateDlss(
        ctx.Get(), params, colorIn.Get(), outTex.Get(), depthIn.Get(), motionIn.Get(),
        kWidth, kHeight, kWidth, kHeight, edpe::UpscaleMode::NativeAA, 11u));
    std::printf("Test 1 passed: DLAA Preset K succeeded after probeCapabilitiesForLuid.\n");

#if defined(EDPE_ENABLE_STREAMLINE) && EDPE_ENABLE_STREAMLINE
    // Test 2: Reproduces the RTX 40/50 sl.common.dll NGX log callback UAF path (issue #19)
    // across active-NGX Streamline init/setDevice/shutdown/FreeLibrary cycles by loading sl.dlss.dll.
    const std::wstring exeDir = getExeDir();
    const std::wstring slInterposerPath = exeDir + L"\\sl.interposer.dll";
    for (int cycle = 0; cycle < 2; ++cycle) {
        edpe::D3D11D3D12Bridge bridge;
        REQUIRE(bridge.initialize(dev.Get()));

        HMODULE slMod = LoadLibraryW(slInterposerPath.c_str());
        REQUIRE(slMod != nullptr);
        auto pSlInit = reinterpret_cast<PFun_slInit*>(GetProcAddress(slMod, "slInit"));
        auto pSlShutdown = reinterpret_cast<PFun_slShutdown*>(GetProcAddress(slMod, "slShutdown"));
        auto pSlSetD3DDevice = reinterpret_cast<PFun_slSetD3DDevice*>(GetProcAddress(slMod, "slSetD3DDevice"));
        auto pSlIsFeatureSupported = reinterpret_cast<PFun_slIsFeatureSupported*>(GetProcAddress(slMod, "slIsFeatureSupported"));
        REQUIRE(pSlInit && pSlShutdown && pSlSetD3DDevice && pSlIsFeatureSupported);

        const wchar_t* paths[] = {exeDir.c_str()};
        const sl::Feature feats[] = {
            sl::kFeatureDLSS,
            sl::kFeatureDLSS_G,
            sl::kFeatureReflex,
            sl::kFeaturePCL,
        };
        sl::Preferences pref{};
        pref.showConsole = false;
        pref.logLevel = sl::LogLevel::eOff;
        pref.pathsToPlugins = paths;
        pref.numPathsToPlugins = 1;
        pref.pathToLogsAndData = nullptr;
        pref.logMessageCallback = nullptr;
        pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                     sl::PreferenceFlags::eDisableDebugText |
                     sl::PreferenceFlags::eUseManualHooking |
                     sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        pref.featuresToLoad = feats;
        pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(feats));
        pref.engine = sl::EngineType::eCustom;
        pref.engineVersion = "1.0";
        pref.projectId = "48d353f3-d07b-4048-876b-09f8622f5a27";
        pref.renderAPI = sl::RenderAPI::eD3D12;

        REQUIRE(pSlInit(pref, sl::kSDKVersion) == sl::Result::eOk);
        REQUIRE(pSlSetD3DDevice(bridge.d3d12Device()) == sl::Result::eOk);

        LUID luid = bridge.d3d12Device()->GetAdapterLuid();
        sl::AdapterInfo ai{};
        ai.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
        ai.deviceLUIDSizeInBytes = sizeof(luid);
        REQUIRE(pSlIsFeatureSupported(sl::kFeatureDLSS, ai) == sl::Result::eOk);

        REQUIRE(pSlShutdown() == sl::Result::eOk);
        FreeLibrary(slMod);
        bridge.shutdown();

        // Alternate Preset K (11) / Preset J (10) so NGX_D3D11_CREATE_DLSS_EXT runs on every cycle.
        const uint32_t preset = (cycle == 0) ? 11u : 10u;
        REQUIRE(edpe::NgxContext::evaluateDlss(
            ctx.Get(), params, colorIn.Get(), outTex.Get(), depthIn.Get(), motionIn.Get(),
            kWidth, kHeight, kWidth, kHeight, edpe::UpscaleMode::NativeAA, preset));
        std::printf("Test 2 cycle %d passed: DLAA Preset %u succeeded after slShutdown.\n",
                    cycle, preset);
    }
#endif

    ctx->Flush();
    edpe::NgxContext::shutdown();
    std::printf("ngx_streamline_interop_test passed.\n");
    return 0;
}
