#include "ngx_context.h"
#include "config.h"
#include "context_census.h"
#include "gpu_vendor.h"
#include "temporal_inputs.h"
#include "log.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <d3d11.h>
#include <dxgi.h>
#include <mutex>
#include <string>
#include <windows.h>
#include <wrl/client.h>

#if defined(EDPE_ENABLE_NGX) && EDPE_ENABLE_NGX
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_d3d.h"
#endif

namespace edpe {

// NGX feature state.
struct NgxFeatureState {
    bool initialized = false;
    bool dlss_available = false;
    bool dlaa_available = false;
    uint32_t quality_mode = 0; // 0=Unknown, 1=Quality, 2=Balanced, 3=Performance, 4=UltraPerformance
    uint32_t preset = 0;       // 0=Auto(K), 5=E(CNN Fast), 10=J, 11=K, 12=L, 13=M
    uint32_t active_perf_quality = 0;
    uint32_t active_preset = 0;
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    bool is_hdr = false;
    bool reset_requested = false;
};

// EDPE's own NGX project ID, for NVSDK_NGX_D3D11_Init_with_ProjectID.
// Generated for EDPE with PowerShell New-Guid on 2026-10-05; it is an EDPE-owned ID
// and not the example GUID from the SDK header.
constexpr char kNgxProjectId[] = "48d353f3-d07b-4048-876b-09f8622f5a27";
constexpr char kNgxEngineVersion[] = "EDPE-unreleased";

// NGX feature state.
static NgxFeatureState g_ngx_state{};
static wchar_t g_status_msg[256] = L"NGX uninitialized";
static std::atomic<bool> s_ngx_has_evaluated{false};
static std::mutex g_override_mutex;
static NgxOverrideInfo g_override_info{};
static std::atomic<bool> g_ngx_verbose_stdout{false};
static std::atomic<bool> g_ngx_capture_overrides{true};

static const char* findInsensitive(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return nullptr;
    for (const char* h = hay; *h; ++h) {
        const char* p = h;
        const char* n = needle;
        while (*p && *n &&
               std::tolower(static_cast<unsigned char>(*p)) ==
                   std::tolower(static_cast<unsigned char>(*n))) {
            ++p;
            ++n;
        }
        if (*n == '\0') return h;
    }
    return nullptr;
}

static bool containsInsensitive(const char* hay, const char* needle) {
    return findInsensitive(hay, needle) != nullptr;
}

static uint32_t resolveSrPreset(uint32_t preset) {
    return (preset == 5u || (preset >= 10u && preset <= 13u)) ? preset : 11u;
}

static bool extractVersionToken(const char* p, char* out, size_t outSize) {
    if (!p || !out || outSize < 2) return false;
    while (*p == ' ' || *p == '\t' || *p == ':') ++p;
    if (*p == 'v' || *p == 'V') ++p;
    if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
    size_t len = 0;
    while (*p && (std::isdigit(static_cast<unsigned char>(*p)) || *p == '.') && len + 1 < outSize) {
        out[len++] = *p++;
    }
    while (len > 0 && out[len - 1] == '.') {
        --len;
    }
    if (len == 0) return false;
    out[len] = '\0';
    return true;
}

const char* ngxPresetDisplayNameA(uint32_t preset) {
    static constexpr const char* kNames[] = {
        "Auto", "A", "B", "C", "D", "E", "F", "G",
        "H", "I", "J", "K", "L", "M", "N", "O"
    };
    return (preset < sizeof(kNames) / sizeof(kNames[0])) ? kNames[preset] : "Unknown";
}

const wchar_t* ngxPresetDisplayNameW(uint32_t preset) {
    static constexpr const wchar_t* kNames[] = {
        L"Auto", L"A", L"B", L"C", L"D", L"E", L"F", L"G",
        L"H", L"I", L"J", L"K", L"L", L"M", L"N", L"O"
    };
    return (preset < sizeof(kNames) / sizeof(kNames[0])) ? kNames[preset] : L"Unknown";
}

bool parseNgxLogLineForOverrides(const char* line, NgxOverrideInfo* info) {
    if (!line || !info) return false;
    bool matched = false;

    if (findInsensitive(line, "Feature dlss override enabled")) {
        info->dllOverrideEnabled = true;
        matched = true;
    }
    if (findInsensitive(line, "Using global override for DLSS-SR preset")) {
        info->drsGlobalPresetOverride = true;
        matched = true;
    }
    if (findInsensitive(line, "Override Reported: ModelPreset") &&
        !findInsensitive(line, "FrameGeneration")) {
        uint32_t val = 0u;
        if (const char* v = findInsensitive(line, "Value:")) {
            v += 6;
            while (*v == ' ' || *v == '\t') ++v;
            char* endPtr = nullptr;
            const unsigned long parsed = std::strtoul(v, &endPtr, 10);
            if (endPtr && endPtr != v) {
                val = static_cast<uint32_t>(parsed);
            }
        }
        if (findInsensitive(line, "Applied: Yes")) {
            info->presetOverrideApplied = true;
            info->overriddenPresetValue = val;
            info->presetOverrideIsLatest = (findInsensitive(line, "Is Latest: Yes") != nullptr);
            matched = true;
        } else if (findInsensitive(line, "Applied: No")) {
            info->presetOverrideApplied = false;
            info->overriddenPresetValue = 0u;
            info->presetOverrideIsLatest = false;
            matched = true;
        }
    }

    if (const char* snip = findInsensitive(line, "feature dlss snippet:")) {
        if (const char* ver = findInsensitive(snip, "version:")) {
            if (extractVersionToken(ver + 8, info->dlssVersion, sizeof(info->dlssVersion))) {
                matched = true;
            }
        }
    } else if (findInsensitive(line, "ngxFeatureVersion =") && !findInsensitive(line, "dlssg")) {
        const char* fv = findInsensitive(line, "ngxFeatureVersion =");
        if (extractVersionToken(fv + 19, info->dlssVersion, sizeof(info->dlssVersion))) {
            matched = true;
        }
    } else if (info->dlssVersion[0] == '\0' &&
               findInsensitive(line, "NGXInitLog") &&
               !findInsensitive(line, "dlssg")) {
        if (const char* b = findInsensitive(line, "Build v")) {
            if (extractVersionToken(b + 7, info->dlssVersion, sizeof(info->dlssVersion))) {
                matched = true;
            }
        }
    }

    return matched;
}

#if defined(EDPE_ENABLE_NGX) && EDPE_ENABLE_NGX
struct NgxFeatureSlot {
    NVSDK_NGX_Handle* handle = nullptr;
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    uint32_t active_perf_quality = 0;
    uint32_t active_preset = 0;
    UpscaleMode requested_mode = UpscaleMode::Native;
    uint32_t requested_preset = 0;
    bool reset_requested = false;
};

static NVSDK_NGX_Parameter* g_ngx_params = nullptr;
static NgxFeatureSlot g_feature_slots[2]{}; // [0] = LDR, [1] = HDR
static ID3D11Device* g_ngx_device = nullptr;
static ID3D11Texture2D* g_exposure_one = nullptr;

static void NVSDK_CONV ngxLogCallback(const char* message,
    NVSDK_NGX_Logging_Level /*loggingLevel*/,
    NVSDK_NGX_Feature /*sourceComponent*/) {
    if (!message) return;
    const bool verboseStdout = g_ngx_verbose_stdout.load(std::memory_order_relaxed);
    if (!g_ngx_capture_overrides.load(std::memory_order_relaxed) && !verboseStdout) {
        return;
    }
    if (g_ngx_capture_overrides.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_override_mutex);
        parseNgxLogLineForOverrides(message, &g_override_info);
    }
    if (!verboseStdout) return;

    static std::mutex s_ngx_log_mutex;
    static uint32_t s_ngx_log_count = 0;
    const bool important =
        containsInsensitive(message, "preset") ||
        containsInsensitive(message, "model") ||
        containsInsensitive(message, "create") ||
        containsInsensitive(message, "init") ||
        containsInsensitive(message, "override") ||
        containsInsensitive(message, "warn") ||
        containsInsensitive(message, "err") ||
        containsInsensitive(message, "fail") ||
        containsInsensitive(message, "dlss") ||
        containsInsensitive(message, "version") ||
        containsInsensitive(message, "cnn") ||
        containsInsensitive(message, "transformer") ||
        containsInsensitive(message, "subrect") ||
        containsInsensitive(message, "jitter") ||
        containsInsensitive(message, "hdr") ||
        containsInsensitive(message, "exposure");

    std::lock_guard<std::mutex> lock(s_ngx_log_mutex);
    const uint32_t idx = s_ngx_log_count++;
    if (idx >= 200u && !important) return;

    std::string line(message);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    std::printf("[NGX] %s\n", line.c_str());
}

// Presets L (12) and M (13) are calibrated for Ultra-Performance / Performance ratios;
// clamp DLAA, UltraQuality, and MaxQuality to Preset K (11) to prevent high-contrast ringing.
static uint32_t dlaaSafePreset(uint32_t preset, NVSDK_NGX_PerfQuality_Value quality) {
    const uint32_t base = resolveSrPreset(preset);
    if ((quality == NVSDK_NGX_PerfQuality_Value_DLAA ||
         quality == NVSDK_NGX_PerfQuality_Value_UltraQuality ||
         quality == NVSDK_NGX_PerfQuality_Value_MaxQuality) &&
        (base == 12u || base == 13u)) {
        return 11u;
    }
    return base;
}

static ID3D11Texture2D* ensureExposureOneTexture() {
    if (g_exposure_one) return g_exposure_one;
    if (!g_ngx_device) return nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const float one = 1.0f;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = &one;
    init.SysMemPitch = sizeof(float);
    if (FAILED(g_ngx_device->CreateTexture2D(&desc, &init, &g_exposure_one))) {
        g_exposure_one = nullptr;
    }
    return g_exposure_one;
}

static std::wstring getModuleDir() {
    wchar_t path[MAX_PATH]{};
    HMODULE hMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&NgxContext::initialize), &hMod);
    if (GetModuleFileNameW(hMod, path, MAX_PATH)) {
        std::wstring p(path);
        size_t slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) return p.substr(0, slash);
    }
    return L".";
}

static NVSDK_NGX_PerfQuality_Value mapModeToNgxPerfQuality(UpscaleMode mode) {
    switch (mode) {
    case UpscaleMode::Quality:
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case UpscaleMode::Balanced:
        return NVSDK_NGX_PerfQuality_Value_Balanced;
    case UpscaleMode::Performance:
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case UpscaleMode::UltraPerformance:
        return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    case UpscaleMode::NativeAA:
    case UpscaleMode::Native:
    default:
        return NVSDK_NGX_PerfQuality_Value_DLAA;
    }
}

static NVSDK_NGX_PerfQuality_Value selectPerfQualityByRatio(uint32_t renderWidth, uint32_t outputWidth) {
    if (!outputWidth || renderWidth >= outputWidth) return NVSDK_NGX_PerfQuality_Value_DLAA;
    const float ratio = static_cast<float>(renderWidth) / static_cast<float>(outputWidth);
    if (ratio >= 0.62f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (ratio >= 0.54f) return NVSDK_NGX_PerfQuality_Value_Balanced;
    if (ratio >= 0.42f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}
#endif

bool NgxContext::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) {
        swprintf_s(g_status_msg, L"NGX init failed: device or context is null");
        return false;
    }

    const bool underWine = isRunningUnderWine();
    {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
            SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                const GpuVendor vendor = classifyGpuVendor(
                    desc.VendorId, desc.DeviceId, desc.Description, underWine);
                if (vendor != GpuVendor::Nvidia && vendor != GpuVendor::Unknown) {
                    swprintf_s(g_status_msg,
                        L"NVIDIA DLSS unavailable: detected %ls GPU (%ls); skipping NGX init",
                        gpuVendorNameW(vendor), desc.Description);
                    EdpeLog(g_status_msg);
                    return false;
                }
            }
        }
    }

#if defined(EDPE_ENABLE_NGX) && EDPE_ENABLE_NGX
    if (g_ngx_state.initialized) {
        if (g_ngx_device == device) return true;
        shutdown();
    }
    g_ngx_device = device;

    std::wstring dll_dir = getModuleDir();
    std::wstring appdata = dll_dir + L"\\edpe_ngx_appdata";
    CreateDirectoryW(appdata.c_str(), nullptr);

    const wchar_t* paths[1] = { dll_dir.c_str() };
    {
        std::lock_guard<std::mutex> lock(g_override_mutex);
        g_override_info = {};
    }
    wchar_t verboseEnv[8]{};
    const bool verboseStdout =
        (GetEnvironmentVariableW(L"EDPE_NGX_VERBOSE_LOG", verboseEnv, 8) > 0 && verboseEnv[0] == L'1');
    g_ngx_verbose_stdout.store(verboseStdout, std::memory_order_relaxed);

    NVSDK_NGX_FeatureCommonInfo common{};
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = 1;
    common.LoggingInfo.LoggingCallback = &ngxLogCallback;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    common.LoggingInfo.DisableOtherLoggingSinks = true;

    g_ngx_capture_overrides.store(true, std::memory_order_release);
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(
        kNgxProjectId,
        NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        kNgxEngineVersion,
        appdata.c_str(),
        device,
        &common,
        NVSDK_NGX_Version_API);

    if (r != NVSDK_NGX_Result_Success) {
        g_ngx_capture_overrides.store(false, std::memory_order_release);
        if (GetModuleHandleW(L"renderdoc.dll") != nullptr) {
            swprintf_s(g_status_msg,
                L"NVSDK_NGX_D3D11_Init failed: 0x%08X (blocked under RenderDoc; use FSR or TAA while capturing)",
                static_cast<unsigned>(r));
        } else if (underWine) {
            swprintf_s(g_status_msg,
                L"NVSDK_NGX_D3D11_Init failed: 0x%08X (Proton: launch with PROTON_ENABLE_NVAPI=1 WINEDLLOVERRIDES=\"dxgi=n,b\" %%command%%)",
                static_cast<unsigned>(r));
        } else {
            swprintf_s(g_status_msg, L"NVSDK_NGX_D3D11_Init failed: 0x%08X", static_cast<unsigned>(r));
        }
        EdpeLog(g_status_msg);
        return false;
    }

    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_ngx_params);
    if (r != NVSDK_NGX_Result_Success || !g_ngx_params) {
        swprintf_s(g_status_msg, L"NGX GetCapabilityParameters failed: 0x%08X", static_cast<unsigned>(r));
        EdpeLog(g_status_msg);
        return false;
    }

    int available = 0, init_result = 0;
    NVSDK_NGX_Parameter_GetI(g_ngx_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(g_ngx_params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);

    if (available != 1) {
        if (underWine) {
            swprintf_s(g_status_msg,
                L"NGX SuperSampling unavailable (result=0x%08X; Proton: launch with PROTON_ENABLE_NVAPI=1 WINEDLLOVERRIDES=\"dxgi=n,b\" %%command%%)",
                static_cast<unsigned>(init_result));
        } else {
            swprintf_s(g_status_msg, L"NGX SuperSampling unavailable (result=0x%08X)", static_cast<unsigned>(init_result));
        }
        EdpeLog(g_status_msg);
        return false;
    }

    g_ngx_state.dlss_available = true;

    // Check DLAA mode at 2560x1440
    unsigned ow = 0, oh = 0, maxw = 0, maxh = 0, minw = 0, minh = 0;
    float sharp = 0.0f;
    r = NGX_DLSS_GET_OPTIMAL_SETTINGS(g_ngx_params, 2560, 1440,
        NVSDK_NGX_PerfQuality_Value_DLAA, &ow, &oh, &maxw, &maxh, &minw, &minh, &sharp);

    if (r == NVSDK_NGX_Result_Success) {
        g_ngx_state.dlaa_available = true;
        swprintf_s(g_status_msg, L"NVIDIA DLSS / DLAA available (DLAA optimal %ux%u)", ow, oh);
    } else {
        swprintf_s(g_status_msg, L"NGX initialized, but DLAA mode unavailable: 0x%08X", static_cast<unsigned>(r));
    }

    g_ngx_state.initialized = true;
    g_ngx_capture_overrides.store(false, std::memory_order_release);
    EdpeLog(g_status_msg);
    return true;
#else
    swprintf_s(g_status_msg, L"NGX not enabled at compile time");
    return false;
#endif
}

void NgxContext::shutdown() {
    s_ngx_has_evaluated.store(false, std::memory_order_release);
    g_ngx_capture_overrides.store(true, std::memory_order_relaxed);
#if defined(EDPE_ENABLE_NGX) && EDPE_ENABLE_NGX
    releaseFeature();
    if (g_exposure_one) {
        g_exposure_one->Release();
        g_exposure_one = nullptr;
    }
    if (g_ngx_params) {
        NVSDK_NGX_D3D11_DestroyParameters(g_ngx_params);
        g_ngx_params = nullptr;
    }
    if (g_ngx_device) {
        NVSDK_NGX_D3D11_Shutdown1(g_ngx_device);
        g_ngx_device = nullptr;
    }
#endif
    {
        std::lock_guard<std::mutex> lock(g_override_mutex);
        g_override_info = {};
    }
    g_ngx_state = {};
    swprintf_s(g_status_msg, L"NGX shutdown complete");
}

bool NgxContext::is_dlss_available() {
    return g_ngx_state.dlss_available;
}

bool NgxContext::is_dlaa_available() {
    return g_ngx_state.dlaa_available;
}

uint32_t NgxContext::get_quality_mode() {
    return g_ngx_state.quality_mode;
}

void NgxContext::set_quality_mode(uint32_t mode) {
    g_ngx_state.quality_mode = mode;
}

uint32_t NgxContext::get_preset() {
    return g_ngx_state.preset;
}

void NgxContext::set_preset(uint32_t preset) {
    g_ngx_state.preset = preset;
}

uint32_t NgxContext::get_render_width() {
    return g_ngx_state.render_width;
}

uint32_t NgxContext::get_render_height() {
    return g_ngx_state.render_height;
}

uint32_t NgxContext::get_output_width() {
    return g_ngx_state.output_width;
}

uint32_t NgxContext::get_output_height() {
    return g_ngx_state.output_height;
}

bool NgxContext::reset_requested() {
    return g_ngx_state.reset_requested;
}

void NgxContext::set_reset_requested(bool reset) {
    g_ngx_state.reset_requested = reset;
}

bool NgxContext::queryOptimalSettings(uint32_t outputWidth, uint32_t outputHeight,
    UpscaleMode mode, DlssOptimalSettings* outSettings) {
    if (!outSettings) return false;
    *outSettings = {};
    if (!outputWidth || !outputHeight) return false;

#if defined(EDPE_ENABLE_NGX) && EDPE_ENABLE_NGX
    if (!g_ngx_state.initialized || !g_ngx_params) return false;

    const NVSDK_NGX_PerfQuality_Value pq = mapModeToNgxPerfQuality(mode);
    unsigned ow = 0, oh = 0, maxw = 0, maxh = 0, minw = 0, minh = 0;
    float sharp = 0.0f;
    const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(
        g_ngx_params, outputWidth, outputHeight, pq,
        &ow, &oh, &maxw, &maxh, &minw, &minh, &sharp);
    if (r != NVSDK_NGX_Result_Success || !ow || !oh) return false;

    outSettings->optimalWidth = ow;
    outSettings->optimalHeight = oh;
    outSettings->minWidth = minw ? minw : ow;
    outSettings->minHeight = minh ? minh : oh;
    outSettings->maxWidth = maxw ? maxw : ow;
    outSettings->maxHeight = maxh ? maxh : oh;
    outSettings->supported = true;
    return true;
#else
    (void)outputWidth; (void)outputHeight; (void)mode;
    return false;
#endif
}

bool NgxContext::ensureDlssFeature(ID3D11DeviceContext* context,
    uint32_t renderWidth, uint32_t renderHeight,
    uint32_t outputWidth, uint32_t outputHeight,
    UpscaleMode mode, bool isHdr, uint32_t preset) {
#if defined(EDPE_ENABLE_NGX) && EDPE_ENABLE_NGX
    if (!g_ngx_state.initialized || !g_ngx_params || !context) return false;
    if (!renderWidth || !renderHeight || !outputWidth || !outputHeight) return false;
    if (renderWidth > outputWidth || renderHeight > outputHeight) return false;

    NgxFeatureSlot& slot = g_feature_slots[isHdr ? 1 : 0];
    if (slot.handle &&
        slot.render_width == renderWidth &&
        slot.render_height == renderHeight &&
        slot.output_width == outputWidth &&
        slot.output_height == outputHeight &&
        slot.requested_mode == mode &&
        slot.requested_preset == preset) {
        g_ngx_state.render_width = renderWidth;
        g_ngx_state.render_height = renderHeight;
        g_ngx_state.output_width = outputWidth;
        g_ngx_state.output_height = outputHeight;
        g_ngx_state.active_perf_quality = slot.active_perf_quality;
        g_ngx_state.active_preset = slot.active_preset;
        g_ngx_state.is_hdr = isHdr;
        return true;
    }

    NVSDK_NGX_PerfQuality_Value perfQuality = NVSDK_NGX_PerfQuality_Value_DLAA;
    if (renderWidth < outputWidth || renderHeight < outputHeight) {
        perfQuality = mapModeToNgxPerfQuality(mode);
        if (perfQuality == NVSDK_NGX_PerfQuality_Value_DLAA) {
            perfQuality = selectPerfQualityByRatio(renderWidth, outputWidth);
        } else {
            unsigned ow = 0, oh = 0, maxw = 0, maxh = 0, minw = 0, minh = 0;
            float sharp = 0.0f;
            if (NGX_DLSS_GET_OPTIMAL_SETTINGS(g_ngx_params, outputWidth, outputHeight, perfQuality,
                    &ow, &oh, &maxw, &maxh, &minw, &minh, &sharp) == NVSDK_NGX_Result_Success) {
                const uint32_t loW = minw ? minw : ow;
                const uint32_t hiW = maxw ? maxw : ow;
                const uint32_t loH = minh ? minh : oh;
                const uint32_t hiH = maxh ? maxh : oh;
                if (renderWidth < loW || renderWidth > hiW || renderHeight < loH || renderHeight > hiH) {
                    perfQuality = selectPerfQualityByRatio(renderWidth, outputWidth);
                }
            }
        }
    }

    const uint32_t effectivePreset = dlaaSafePreset(preset, perfQuality);
    const uint32_t perfQualityInt = static_cast<uint32_t>(perfQuality);

    if (slot.handle &&
        slot.render_width == renderWidth &&
        slot.render_height == renderHeight &&
        slot.output_width == outputWidth &&
        slot.output_height == outputHeight &&
        slot.active_perf_quality == perfQualityInt &&
        slot.active_preset == effectivePreset) {
        slot.requested_mode = mode;
        slot.requested_preset = preset;
        g_ngx_state.render_width = renderWidth;
        g_ngx_state.render_height = renderHeight;
        g_ngx_state.output_width = outputWidth;
        g_ngx_state.output_height = outputHeight;
        g_ngx_state.active_perf_quality = perfQualityInt;
        g_ngx_state.active_preset = effectivePreset;
        g_ngx_state.is_hdr = isHdr;
        return true;
    }

    NgxFeatureSlot& otherSlot = g_feature_slots[isHdr ? 0 : 1];
    if (otherSlot.handle) {
        NVSDK_NGX_D3D11_ReleaseFeature(otherSlot.handle);
        otherSlot = {};
    }

    if (slot.handle) {
        NVSDK_NGX_D3D11_ReleaseFeature(slot.handle);
        slot = {};
    }

    // Apply selected DLSS Transformer/CNN preset (defaulting to Preset K = 11).
    const uint32_t srPreset = resolveSrPreset(preset);
    NVSDK_NGX_Parameter_SetUI(g_ngx_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, effectivePreset);
    NVSDK_NGX_Parameter_SetUI(g_ngx_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, effectivePreset);
    NVSDK_NGX_Parameter_SetUI(g_ngx_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, srPreset);
    NVSDK_NGX_Parameter_SetUI(g_ngx_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, srPreset);
    NVSDK_NGX_Parameter_SetUI(g_ngx_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, srPreset);

    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = renderWidth;
    cp.Feature.InHeight = renderHeight;
    cp.Feature.InTargetWidth = outputWidth;
    cp.Feature.InTargetHeight = outputHeight;
    cp.Feature.InPerfQualityValue = perfQuality;
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (isHdr) {
        // On the linear HDR route H is pre-tonemap radiance that the game's own tone pass exposes
        // after the resolve. Do NOT enable AutoExposure when supplying fixed 1.0f HDR exposure.
        cp.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    }
    // On the LDR route the input is already tonemapped and auto-exposed by the game; enabling
    // AutoExposure on LDR causes DLSS to re-estimate exposure on dark/bright transitions (e.g.
    // night settlement interiors) and blow out lighting.

    {
        std::lock_guard<std::mutex> lock(g_override_mutex);
        g_override_info.presetOverrideApplied = false;
        g_override_info.presetOverrideIsLatest = false;
        g_override_info.overriddenPresetValue = 0u;
    }

    g_ngx_capture_overrides.store(true, std::memory_order_release);
    NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(context, &slot.handle, g_ngx_params, &cp);
    g_ngx_capture_overrides.store(false, std::memory_order_release);
    if (r != NVSDK_NGX_Result_Success || !slot.handle) {
        slot.handle = nullptr;
        swprintf_s(g_status_msg, L"NGX_D3D11_CREATE_DLSS_EXT (%ux%u -> %ux%u) failed: 0x%08X",
            renderWidth, renderHeight, outputWidth, outputHeight, static_cast<unsigned>(r));
        EdpeLog(g_status_msg);
        return false;
    }

    slot.render_width = renderWidth;
    slot.render_height = renderHeight;
    slot.output_width = outputWidth;
    slot.output_height = outputHeight;
    slot.active_perf_quality = perfQualityInt;
    slot.active_preset = effectivePreset;
    slot.requested_mode = mode;
    slot.requested_preset = preset;
    slot.reset_requested = true;

    g_ngx_state.render_width = renderWidth;
    g_ngx_state.render_height = renderHeight;
    g_ngx_state.output_width = outputWidth;
    g_ngx_state.output_height = outputHeight;
    g_ngx_state.active_perf_quality = perfQualityInt;
    g_ngx_state.active_preset = effectivePreset;
    g_ngx_state.is_hdr = isHdr;
    g_ngx_state.reset_requested = true;

    const NgxOverrideInfo ov = get_override_info();
    const wchar_t* presetName = ngxPresetDisplayNameW(effectivePreset);
    if (ov.presetOverrideApplied && ov.overriddenPresetValue != 0u) {
        const wchar_t* forcedName = ngxPresetDisplayNameW(ov.overriddenPresetValue);
        swprintf_s(g_status_msg,
            L"NVIDIA %ls feature active (%ux%u -> %ux%u, Preset %ls, %ls) [DRS Override -> Preset %ls]",
            (renderWidth == outputWidth && renderHeight == outputHeight) ? L"DLAA" : L"DLSS SR",
            renderWidth, renderHeight, outputWidth, outputHeight,
            presetName, isHdr ? L"HDR" : L"LDR", forcedName);
        if (ov.overriddenPresetValue != effectivePreset) {
            wchar_t warnMsg[320];
            swprintf_s(warnMsg,
                L"EDPE WARNING: NVIDIA App / DRS overrode DLSS preset (requested Preset %ls, driver forced Preset %ls%ls). Disable 'DLSS Override - Model Presets' in NVIDIA App to use EDPE preset selection.",
                presetName, forcedName, ov.presetOverrideIsLatest ? L" / Latest" : L"");
            EdpeLog(warnMsg);
        }
    } else {
        swprintf_s(g_status_msg, L"NVIDIA %ls feature active (%ux%u -> %ux%u, Preset %ls, %ls)",
            (renderWidth == outputWidth && renderHeight == outputHeight) ? L"DLAA" : L"DLSS SR",
            renderWidth, renderHeight, outputWidth, outputHeight,
            presetName, isHdr ? L"HDR" : L"LDR");
    }
    EdpeLog(g_status_msg);
    return true;
#else
    (void)context; (void)renderWidth; (void)renderHeight;
    (void)outputWidth; (void)outputHeight; (void)mode; (void)isHdr; (void)preset;
    return false;
#endif
}

bool NgxContext::ensureDlaaFeature(ID3D11DeviceContext* context, uint32_t width, uint32_t height, bool isHdr) {
    return ensureDlssFeature(context, width, height, width, height, UpscaleMode::NativeAA, isHdr, g_ngx_state.preset);
}

void NgxContext::releaseFeature() {
#if defined(EDPE_ENABLE_NGX) && EDPE_ENABLE_NGX
    for (auto& slot : g_feature_slots) {
        if (slot.handle) {
            NVSDK_NGX_D3D11_ReleaseFeature(slot.handle);
        }
        slot = {};
    }
#endif
    g_ngx_state.render_width = 0;
    g_ngx_state.render_height = 0;
    g_ngx_state.output_width = 0;
    g_ngx_state.output_height = 0;
    g_ngx_state.active_perf_quality = 0;
    g_ngx_state.active_preset = 0;
}

bool NgxContext::evaluateDlss(ID3D11DeviceContext* context,
    const NvidiaDlssInputParams& params,
    ID3D11Resource* colorIn,
    ID3D11Resource* outputUav,
    ID3D11Resource* depthIn,
    ID3D11Resource* motionIn,
    uint32_t renderWidth,
    uint32_t renderHeight,
    uint32_t outputWidth,
    uint32_t outputHeight,
    UpscaleMode mode,
    uint32_t preset) {
#if defined(EDPE_ENABLE_NGX) && EDPE_ENABLE_NGX
    if (!context || !colorIn || !outputUav || !depthIn || !motionIn) return false;
    const uint32_t usePreset = preset ? preset : g_ngx_state.preset;
    if (!ensureDlssFeature(context, renderWidth, renderHeight, outputWidth, outputHeight, mode, params.isHdr, usePreset)) {
        return false;
    }
    NgxFeatureSlot& slot = g_feature_slots[params.isHdr ? 1 : 0];
    if (!slot.handle) return false;

    NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = colorIn;
    ep.Feature.pInOutput = outputUav;
    ep.Feature.InSharpness = 0.0f;
    ep.pInDepth = depthIn;
    ep.pInMotionVectors = motionIn;
    ep.InJitterOffsetX = params.jitterX;
    ep.InJitterOffsetY = params.jitterY;
    ep.InRenderSubrectDimensions.Width = renderWidth;
    ep.InRenderSubrectDimensions.Height = renderHeight;
    ep.InReset = (params.inReset || g_ngx_state.reset_requested || slot.reset_requested) ? 1 : 0;
    ep.InMVScaleX = params.motionScaleX;
    ep.InMVScaleY = params.motionScaleY;
    ep.pInBiasCurrentColorMask = nullptr;

    ep.InPreExposure = 1.0f;
    ep.InExposureScale = 1.0f;
    if (params.isHdr) {
        ep.pInExposureTexture = ensureExposureOneTexture();
    }
    ep.InFrameTimeDeltaInMsec = params.frameTimeMs;

    NVSDK_NGX_Result r;
    {
        EvaluatingUpscalerGuard evalGuard;
        r = NGX_D3D11_EVALUATE_DLSS_EXT(context, slot.handle, g_ngx_params, &ep);
    }
    if (r != NVSDK_NGX_Result_Success) {
        swprintf_s(g_status_msg, L"NGX_D3D11_EVALUATE_DLSS_EXT failed: 0x%08X", static_cast<unsigned>(r));
        EdpeLog(g_status_msg);
        return false;
    }

    s_ngx_has_evaluated.store(true, std::memory_order_release);
    static unsigned s_ngx_eval_count = 0;
    if (++s_ngx_eval_count == 1) {
        wchar_t msg[192];
        swprintf_s(msg, L"EDPE: NGX %ls evaluated successfully #%u (%ux%u -> %ux%u)",
            (renderWidth == outputWidth && renderHeight == outputHeight) ? L"DLAA" : L"DLSS SR",
            s_ngx_eval_count, renderWidth, renderHeight, outputWidth, outputHeight);
        EdpeLog(msg);
    }

    if (ep.InReset) {
        slot.reset_requested = false;
        g_ngx_state.reset_requested = false;
    }
    return true;
#else
    (void)context; (void)params; (void)colorIn; (void)outputUav;
    (void)depthIn; (void)motionIn;
    (void)renderWidth; (void)renderHeight; (void)outputWidth; (void)outputHeight;
    (void)mode; (void)preset;
    return false;
#endif
}

bool NgxContext::evaluateDlaa(ID3D11DeviceContext* context,
    const NvidiaDlssInputParams& params,
    ID3D11Resource* colorIn,
    ID3D11Resource* outputUav,
    ID3D11Resource* depthIn,
    ID3D11Resource* motionIn,
    uint32_t width,
    uint32_t height) {
    return evaluateDlss(context, params, colorIn, outputUav, depthIn, motionIn,
        width, height, width, height, UpscaleMode::NativeAA, g_ngx_state.preset);
}

const wchar_t* NgxContext::get_status_message() {
    return g_status_msg;
}

bool NgxContext::has_evaluated() {
    return s_ngx_has_evaluated.load(std::memory_order_acquire);
}

NgxOverrideInfo NgxContext::get_override_info() {
    std::lock_guard<std::mutex> lock(g_override_mutex);
    return g_override_info;
}

uint32_t NgxContext::get_effective_runtime_preset() {
    const NgxOverrideInfo ov = get_override_info();
    if (ov.presetOverrideApplied && ov.overriddenPresetValue != 0u) {
        return ov.overriddenPresetValue;
    }
    return g_ngx_state.active_preset ? g_ngx_state.active_preset : resolveSrPreset(g_ngx_state.preset);
}

bool NgxContext::is_preset_overridden() {
    const NgxOverrideInfo ov = get_override_info();
    const uint32_t requested = g_ngx_state.active_preset
        ? g_ngx_state.active_preset
        : resolveSrPreset(g_ngx_state.preset);
    return ov.presetOverrideApplied &&
           ov.overriddenPresetValue != 0u &&
           (ov.overriddenPresetValue != requested || ov.presetOverrideIsLatest);
}

} // namespace edpe