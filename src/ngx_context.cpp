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

// EDPE's own NGX project ID, for NVSDK_NGX_D3D11_Init_with_ProjectID.
// Generated for EDPE with PowerShell New-Guid on 2026-10-05; it is not EDVR's ID
// and not the example GUID from the SDK header. nvsdk_ngx.h requires a GUID-like
// string without braces. Keep identical to kNgxProjectId in tools/ngx_probe/ngx_probe.cpp.
constexpr char kNgxProjectId[] = "48d353f3-d07b-4048-876b-09f8622f5a27";
// Engine type passed alongside it is NVSDK_NGX_ENGINE_TYPE_CUSTOM.
constexpr char kNgxEngineVersion[] = "EDPE-unreleased";

// NGX feature state.
inline NgxFeatureState g_ngx_state{};

bool NgxContext::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) return false;
    if (g_ngx_state.initialized) return true; // already initialized

    // NGX D3D11 initialization would use NVSDK_NGX_D3D11_Init_with_ProjectID
    // (nvsdk_ngx.h:246) with kNgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM and
    // kNgxEngineVersion. NVSDK_NGX_D3D11_Init (nvsdk_ngx.h:150) takes an
    // NVIDIA-assigned unsigned long long ApplicationId, which EDPE does not have.

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

} // namespace edpe