#include "ngx_context.h"

#include <atomic>
#include <cstdint>
#include <d3d11.h>
#include <windows.h>

namespace edpe {

namespace {
// Custom engine project ID for NGX (GUID-like, not EDVR's).
// Must be unique per engine; do not reuse EDVR's NGX Project ID.
constexpr unsigned kNgxProjectId[4] = {0xED, 0xPE, 0x11, 0x22};

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

// Global NGX state (protected by module lifetime).
inline NgxFeatureState g_ngx_state{};

} // namespace anonymous

bool NgxContext::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) return false;
    if (g_ngx_state.initialized) return true; // already initialized

    // NGX D3D11 initialization with custom project ID.
    // NVSDK_NGX_D3D11_Init_with_ProjectID is the entry point for custom engines.
    // The SDK documentation warns: shutdown may hang >20s; handle accordingly.

    // TODO: Load nvngx_dlss.dll and call NVSDK_NGX_D3D11_Init_with_ProjectID.
    // For now, mark as uninitialized and return safely.
    // TODO: On success, query capability parameters via GetCapabilityParameters.
    // TODO: Populate feature availability (DLAA vs DLSS SR).

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