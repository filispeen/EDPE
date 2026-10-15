#include "ngx_context.h"
#include "temporal_inputs.h"
#include "log.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <d3d11.h>
#include <string>
#include <windows.h>

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
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    bool reset_requested = false;
};

// EDPE's own NGX project ID, for NVSDK_NGX_D3D11_Init_with_ProjectID.
// Generated for EDPE with PowerShell New-Guid on 2026-10-05; it is not EDVR's ID
// and not the example GUID from the SDK header.
constexpr char kNgxProjectId[] = "48d353f3-d07b-4048-876b-09f8622f5a27";
constexpr char kNgxEngineVersion[] = "EDPE-unreleased";

// NGX feature state.
static NgxFeatureState g_ngx_state{};
static wchar_t g_status_msg[256] = L"NGX uninitialized";

#if defined(EDPE_ENABLE_NGX) && EDPE_ENABLE_NGX
static NVSDK_NGX_Parameter* g_ngx_params = nullptr;
static NVSDK_NGX_Handle* g_dlaa_feature = nullptr;
static ID3D11Device* g_ngx_device = nullptr;

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
#endif

bool NgxContext::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) {
        swprintf_s(g_status_msg, L"NGX init failed: device or context is null");
        return false;
    }
    if (g_ngx_state.initialized) return true;

#if defined(EDPE_ENABLE_NGX) && EDPE_ENABLE_NGX
    g_ngx_device = device;

    std::wstring dll_dir = getModuleDir();
    std::wstring appdata = dll_dir + L"\\edpe_ngx_appdata";
    CreateDirectoryW(appdata.c_str(), nullptr);

    const wchar_t* paths[1] = { dll_dir.c_str() };
    NVSDK_NGX_FeatureCommonInfo common{};
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = 1;

    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(
        kNgxProjectId,
        NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        kNgxEngineVersion,
        appdata.c_str(),
        device,
        &common,
        NVSDK_NGX_Version_API);

    if (r != NVSDK_NGX_Result_Success) {
        swprintf_s(g_status_msg, L"NVSDK_NGX_D3D11_Init failed: 0x%08X", static_cast<unsigned>(r));
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
        swprintf_s(g_status_msg, L"NGX SuperSampling unavailable (result=0x%08X)", static_cast<unsigned>(init_result));
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
        swprintf_s(g_status_msg, L"NVIDIA DLAA available (optimal %ux%u)", ow, oh);
    } else {
        swprintf_s(g_status_msg, L"NGX initialized, but DLAA mode unavailable: 0x%08X", static_cast<unsigned>(r));
    }

    g_ngx_state.initialized = true;
    EdpeLog(g_status_msg);
    return true;
#else
    swprintf_s(g_status_msg, L"NGX not enabled at compile time");
    return false;
#endif
}

void NgxContext::shutdown() {
#if defined(EDPE_ENABLE_NGX) && EDPE_ENABLE_NGX
    releaseFeature();
    if (g_ngx_params) {
        NVSDK_NGX_D3D11_DestroyParameters(g_ngx_params);
        g_ngx_params = nullptr;
    }
    if (g_ngx_device) {
        NVSDK_NGX_D3D11_Shutdown1(g_ngx_device);
        g_ngx_device = nullptr;
    }
#endif
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

bool NgxContext::ensureDlaaFeature(ID3D11DeviceContext* context, uint32_t width, uint32_t height) {
#if defined(EDPE_ENABLE_NGX) && EDPE_ENABLE_NGX
    if (!g_ngx_state.initialized || !g_ngx_params || !context) return false;

    if (g_dlaa_feature && g_ngx_state.render_width == width && g_ngx_state.render_height == height) {
        return true;
    }

    releaseFeature();

    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = width;
    cp.Feature.InHeight = height;
    cp.Feature.InTargetWidth = width;
    cp.Feature.InTargetHeight = height;
    cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                              NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                              NVSDK_NGX_DLSS_Feature_Flags_IsHDR;

    NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(context, &g_dlaa_feature, g_ngx_params, &cp);
    if (r != NVSDK_NGX_Result_Success || !g_dlaa_feature) {
        swprintf_s(g_status_msg, L"NGX_D3D11_CREATE_DLSS_EXT failed: 0x%08X", static_cast<unsigned>(r));
        EdpeLog(g_status_msg);
        return false;
    }

    g_ngx_state.render_width = width;
    g_ngx_state.render_height = height;
    g_ngx_state.output_width = width;
    g_ngx_state.output_height = height;
    swprintf_s(g_status_msg, L"NVIDIA DLAA feature active (%ux%u)", width, height);
    EdpeLog(g_status_msg);
    return true;
#else
    (void)context; (void)width; (void)height;
    return false;
#endif
}

void NgxContext::releaseFeature() {
#if defined(EDPE_ENABLE_NGX) && EDPE_ENABLE_NGX
    if (g_dlaa_feature) {
        NVSDK_NGX_D3D11_ReleaseFeature(g_dlaa_feature);
        g_dlaa_feature = nullptr;
    }
#endif
    g_ngx_state.render_width = 0;
    g_ngx_state.render_height = 0;
    g_ngx_state.output_width = 0;
    g_ngx_state.output_height = 0;
}

bool NgxContext::evaluateDlaa(ID3D11DeviceContext* context,
    const NvidiaDlssInputParams& params,
    ID3D11Resource* colorIn,
    ID3D11Resource* outputUav,
    ID3D11Resource* depthIn,
    ID3D11Resource* motionIn,
    ID3D11Resource* reactiveMaskIn,
    uint32_t width,
    uint32_t height) {
#if defined(EDPE_ENABLE_NGX) && EDPE_ENABLE_NGX
    if (!context || !colorIn || !outputUav || !depthIn || !motionIn) return false;
    if (!ensureDlaaFeature(context, width, height)) return false;

    NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = colorIn;
    ep.Feature.pInOutput = outputUav;
    ep.pInDepth = depthIn;
    ep.pInMotionVectors = motionIn;
    ep.InJitterOffsetX = params.jitterX;
    ep.InJitterOffsetY = params.jitterY;
    ep.InRenderSubrectDimensions.Width = width;
    ep.InRenderSubrectDimensions.Height = height;
    ep.InReset = (params.inReset || g_ngx_state.reset_requested) ? 1 : 0;
    ep.InMVScaleX = params.motionScaleX;
    ep.InMVScaleY = params.motionScaleY;
    ep.pInBiasCurrentColorMask = reactiveMaskIn;

    NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(context, g_dlaa_feature, g_ngx_params, &ep);
    if (r != NVSDK_NGX_Result_Success) {
        swprintf_s(g_status_msg, L"NGX_D3D11_EVALUATE_DLSS_EXT failed: 0x%08X", static_cast<unsigned>(r));
        return false;
    }

    if (ep.InReset) {
        g_ngx_state.reset_requested = false;
    }
    return true;
#else
    (void)context; (void)params; (void)colorIn; (void)outputUav;
    (void)depthIn; (void)motionIn; (void)reactiveMaskIn; (void)width; (void)height;
    return false;
#endif
}

const wchar_t* NgxContext::get_status_message() {
    return g_status_msg;
}

} // namespace edpe