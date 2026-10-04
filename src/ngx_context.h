#pragma once

#include <cstdint>
#include <d3d11.h>

namespace edpe {

// NGX context management.
// Handles NVIDIA DLSS Super Resolution and DLAA initialization, evaluation,
// and shutdown through the NVIDIA NGX D3D11 SDK.
// 
// Important: NGX shutdown may hang >20s (empirically observed in SDK probe).
// Do not block the game exit path. Fail open: if NGX cannot be safely
// initialized or evaluated, disable upscaler and present original frame.
// Input conventions must be verified in Elite before NGX is consumed as
// production input (depth, motion, jitter, reset semantics).

class NgxContext {
public:
    NgxContext() = default;
    ~NgxContext() { shutdown(); }

    // Initialize NGX with the D3D11 device and context.
    // Returns true on success; false if initialization cannot proceed safely.
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    // Shut down NGX. Must not block; defer to non-critical cleanup path.
    void shutdown();

    // Feature availability.
    bool is_dlss_available() const;
    bool is_dlaa_available() const;

    // Quality mode selection (Quality/Balanced/Performance/UltraPerformance).
    uint32_t get_quality_mode() const;
    void set_quality_mode(uint32_t mode);

    // Render dimensions (input/output resolution).
    uint32_t get_render_width() const;
    uint32_t get_render_height() const;
    uint32_t get_output_width() const;
    uint32_t get_output_height() const;

    // History reset request.
    bool reset_requested() const;
    void set_reset_requested(bool reset);

private:
    // NGX feature state (populated after successful initialization).
    struct State {
        bool initialized = false;
        bool dlss_available = false;
        bool dlaa_available = false;
        uint32_t quality_mode = 0;
        uint32_t render_width = 0;
        uint32_t render_height = 0;
        uint32_t output_width = 0;
        uint32_t output_height = 0;
        bool reset_requested = false;
    } m_state{};
};

} // namespace edpe