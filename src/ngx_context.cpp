#include "ngx_context.h"

#include <atomic>
#include <cstdint>
#include <d3d11.h>
#include <windows.h>

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

// Constant for NGX project GUID.
// This is a project-owned identifier; do not reuse EDVR's NGX Project ID.
// Format: GUID-like string matching NVSDK_NGX_D3D11_Init_with_ProjectID expectation.
// TODO: Obtain a proper NVIDIA-assigned application ID for NVSDK_NGX_D3D11_Init.
// Until then, use NVSDK_NGX_D3D11_Init(0, ...) as documented:
// "Until NVIDIA has assigned you an applicationId, use 0."
// "If you do not have one please contact us."
// project_id_string is used only with Init_with_ProjectID.
constexpr char kNgxProjectIdString[] = "edpe-custom-engine-2026";

// NGX feature state.
inline NgxFeatureState g_ngx_state{};

} // namespace anonymous

bool NgxContext::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) return false;
    if (g_ngx_state.initialized) return true; // already initialized

    // NGX D3D11 initialization.
    // Two pathways:
    // 1) NVSDK_NGX_D3D11_Init_with_ProjectID() — for custom engines without an NVIDIA application ID.
    //    Project ID must be GUID-like; this string is project-owned and not EDVR's.
    // 2) NVSDK_NGX_D3D11_Init() — with InApplicationId = 0 until NVIDIA assigns one.
    //    Per SDK docs: "Until NVIDIA has assigned you an applicationId, use 0."
    //    "If an application ID is not available, use NVSDK_NGX_Init_with_ProjectID to supply your own identifier."

    // Intentionally disabled until shutdown safety is verified.
    // Do not consume NGX as production input until conventions are validated
    // in Elite (jitter sign/motion-scale/depth-flag/reset semantics).
    g_ngx_state.initialized = false; // placeholder until SDK integration is verified
    return false; // intentionally disabled until shutdown lifecycle is understood
}

void NgxContext::shutdown() {
    // TODO: Call NVSDK_NGX_D3D11_Shutdown1 with care.
    // Empirical note: SDK probe hung >20s in shutdown; do not block.
    // Implement non-blocking cleanup or defer to game exit path.
    g_ngx_state = {};
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