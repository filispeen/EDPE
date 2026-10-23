#include "context_census.h"
#include "camera_delta.h"
#include "dxbc_fanout.h"
#include "elite_camera.h"
#include "log.h"
#include "motion_pass.h"
#include "shader_probe.h"
#include "temporal_math.h"
#include "upscaler_backend.h"
#include "ngx_context.h"
#include "config.h"
#include "ui_composite_pass.h"
#include "ui.h"
#include "jitter_logger.h"
#include "jitter_debug_pass.h"
#include "jitter_view_recorder.h"
#include "profiler.h"

#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <d3d11_1.h>
#include <memory>
#include <limits>
#include <mutex>
#include <string>
#include <vector>
#include <wrl/client.h>
#include <windows.h>

namespace {
constexpr size_t kVSSetConstantBuffers = 7;
constexpr size_t kDrawIndexed = 12;
constexpr size_t kDraw = 13;
constexpr size_t kMap = 14;
constexpr size_t kUnmap = 15;
constexpr size_t kDrawIndexedInstanced = 20;
constexpr size_t kDrawInstanced = 21;
constexpr size_t kOMSetRenderTargets = 33;
constexpr unsigned kPairFirstSample = 100;
constexpr unsigned kPairSecondSample = 101;
constexpr unsigned kAlternateProjectionSample = 102;
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using VSSetConstantBuffersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    UINT, ID3D11Buffer* const*);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*,
    UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    UINT, UINT, UINT);

bool patchSlot(void** table, size_t slot, void* expected, void* replacement);
void STDMETHODCALLTYPE observedOMSetRenderTargets(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* dsv);

std::atomic<OMSetRenderTargetsFn> original{nullptr};
std::atomic<VSSetConstantBuffersFn> original_vs_set_buffers{nullptr};
std::atomic<DrawIndexedFn> original_draw_indexed{nullptr};
std::atomic<DrawFn> original_draw{nullptr};
std::atomic<MapFn> original_map{nullptr};
std::atomic<UnmapFn> original_unmap{nullptr};
std::atomic<DrawIndexedInstancedFn> original_draw_indexed_instanced{nullptr};
std::atomic<DrawInstancedFn> original_draw_instanced{nullptr};
std::atomic<ID3D11DeviceContext*> observed_context{nullptr};
IDXGISwapChain* observed_swap_chain = nullptr; // Weak; released by the game.
void** patched_table = nullptr;
bool attempted = false;
std::mutex seen_mutex;
struct SeenDepthView {
    ID3D11DepthStencilView* view = nullptr; // Identity only; never dereferenced later.
    DXGI_FORMAT texture_format = DXGI_FORMAT_UNKNOWN;
    UINT samples = 0;
    bool color_logged = false;
    unsigned long long interval_binds = 0;
    ID3D11RenderTargetView* mrt_hdr_view = nullptr; // Weak; compared within one frame only.
    unsigned long long mrt_frame = 0;
    unsigned long long scene_match_frame = ~0ull;
};
std::array<SeenDepthView, 32> seen{};
size_t seen_count = 0;
std::atomic<unsigned long long> dsv_binds{0};
std::atomic<bool> sequence_requested{false};
std::atomic<bool> sequence_active{false};
std::array<int, 64> bind_sequence{};
size_t sequence_stored = 0;
unsigned sequence_transitions = 0;
int sequence_last = -2;
int snapshot_queued = -1;
int snapshot_active = -1;
int snapshot_index = -1;
ID3D11DepthStencilView* snapshot_view = nullptr; // Retained only until the next UI Present.
ID3D11RenderTargetView* snapshot_color_view = nullptr; // Last RTV0 on the selected DSV.
unsigned snapshot_wait_frames = 0;
std::atomic<bool> snapshot_waiting{false};
std::atomic<unsigned long long> last_present_frame{0};
unsigned long long snapshot_armed_after = 0;
unsigned long long snapshot_first_bind_after = 0;
unsigned long long snapshot_last_bind_after = 0;
unsigned snapshot_bind_count = 0;
int camera_pair_queued = -1;
int camera_pair_target = -1;
unsigned long long camera_pair_armed_after = 0;
unsigned long long camera_pair_bind_frame = ~0ull;
unsigned camera_pair_bind_count = 0;
bool motion_pair_queued = false;
bool motion_pair_active = false;
std::array<edpe::CameraProjection, 2> motion_cameras{};
std::array<unsigned long long, 2> motion_camera_frames{~0ull, ~0ull};
std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 2> gpu_camera_pair{};
// Per-frame camera delta log (observation only, docs/elite-input-conventions.md
// step 4). The 5376-byte VS slot-1 buffer bound at the first scene-MRT bind of
// a frame is copied when that interval ends into a persistent staging ring and
// read at a later Present with DO_NOT_WAIT. Nothing is written to the game.
struct CameraDeltaSlot {
    Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
    unsigned long long frame = 0;
};
constexpr size_t kCameraDeltaRing = 4;
std::array<CameraDeltaSlot, kCameraDeltaRing> camera_delta_ring{};
size_t camera_delta_head = 0;  // next slot to read
size_t camera_delta_count = 0; // slots holding queued copies
Microsoft::WRL::ComPtr<ID3D11Buffer> camera_delta_source; // copied at the next OMSetRenderTargets
unsigned long long camera_delta_source_frame = 0;
unsigned long long camera_delta_taken_frame = ~0ull;
unsigned camera_delta_dropped = 0;  // ring full or map failure, reset each summary
bool camera_delta_disabled = false; // staging creation failed; log stays off
bool camera_delta_announced = false;
edpe::CameraDeltaTracker camera_delta_tracker;
static edpe::CameraProjection s_camera_previous{};
static edpe::CameraProjection s_camera_now{};
static bool s_camera_reset = false;
static unsigned long long s_camera_frame = ~0ull;
static unsigned long long s_camera_previous_frame = ~0ull;
static unsigned long long s_in_scene_camera_latched_frame = ~0ull;
static unsigned long long s_hdr_slot0_seen_frame = ~0ull;
static unsigned s_consecutive_skipped_frames = 0;
static unsigned s_auxiliary_rejection_streak = 0;
static edpe::CameraProjection s_fallback_unmapped_camera{};
static unsigned long long s_fallback_unmapped_camera_frame = ~0ull;
static edpe::CameraProjection s_latest_unmapped_camera{};
static unsigned long long s_latest_unmapped_camera_frame = ~0ull;
static bool s_latest_unmapped_camera_valid = false;
static std::atomic<bool> s_inside_main_scene{false};
static std::atomic<bool> s_in_edpe_present{false};
static edpe::ProjectionJitter s_injected_jitter{};
static unsigned long long s_jitter_injected_frame = ~0ull;
static edpe::ProjectionJitter s_last_rendered_jitter{};

inline bool isSceneCameraAspect(float scaleX, float scaleY, UINT width, UINT height) {
    if (scaleX <= 0.0f || scaleY <= 0.0f || !width || !height) return true;
    const float camAspect = scaleY / scaleX;
    const float screenAspect = static_cast<float>(width) / static_cast<float>(height);
    return std::fabs(camAspect - screenAspect) < 0.05f;
}

inline float cameraRotationDeterminant(const edpe::CameraProjection& cam) {
    const float* m = cam.worldFromView;
    return m[0] * (m[5] * m[10] - m[6] * m[9])
         - m[1] * (m[4] * m[10] - m[6] * m[8])
         + m[2] * (m[4] * m[9]  - m[5] * m[8]);
}

inline float cameraRotationTrace(const edpe::CameraProjection& a, const edpe::CameraProjection& b) {
    const float* c = a.worldFromView;
    const float* p = b.worldFromView;
    return (c[0]*p[0] + c[4]*p[4] + c[8]*p[8]) +
           (c[1]*p[1] + c[5]*p[5] + c[9]*p[9]) +
           (c[2]*p[2] + c[6]*p[6] + c[10]*p[10]);
}

inline bool isReflectionOrAuxiliaryCamera(const edpe::CameraProjection& candidate, const edpe::CameraProjection& baseline) {
    // 1. Planar reflection matrices invert coordinate chirality: det(R) = -1.0.
    const float det = cameraRotationDeterminant(candidate);
    if (det < 0.5f) return true;

    if (baseline.scaleX <= 0.0f || baseline.scaleY <= 0.0f) return false;

    // 2. Projection FOV / scale mismatch against active main camera:
    if (std::fabs(candidate.scaleX - baseline.scaleX) > 0.01f * baseline.scaleX ||
        std::fabs(candidate.scaleY - baseline.scaleY) > 0.01f * baseline.scaleY) {
        return true;
    }

    // 3. Discontinuous single-frame orientation or probe offset relative to active baseline:
    const float* c = candidate.worldFromView;
    const float* b = baseline.worldFromView;
    const float dx = c[3] - b[3];
    const float dy = c[7] - b[7];
    const float dz = c[11] - b[11];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float rotTrace = cameraRotationTrace(candidate, baseline);

    // Abrupt single-frame orientation jump (> 45 deg, rotTrace < 2.40) at any distance is an auxiliary/probe camera:
    if (rotTrace < 2.40f) {
        return true;
    }

    // Auxiliary mirror / panel / probe camera with combined orientation (> 18 deg) and position jump:
    if (dist > 0.05f && rotTrace < 2.90f) {
        return true;
    }

    return false;
}

inline bool sameCamera(const edpe::CameraProjection& a, const edpe::CameraProjection& b) {
    for (int i = 0; i < 12; ++i) {
        if (std::fabs(a.worldFromView[i] - b.worldFromView[i]) > 1e-5f) return false;
    }
    return std::fabs(a.scaleX - b.scaleX) < 1e-5f && std::fabs(a.scaleY - b.scaleY) < 1e-5f;
}

inline bool isRevisitedCamera(const edpe::CameraProjection& camera) {
    static std::array<edpe::CameraProjection, 8> recent{};
    static unsigned count = 0;
    static unsigned head = 0;
    if (s_camera_now.scaleX > 0.0f && sameCamera(camera, s_camera_now)) return false;
    const unsigned stored = count < 8 ? count : 8;
    for (unsigned i = 0; i < stored; ++i) {
        if (sameCamera(camera, recent[i])) return true;
    }
    recent[head] = camera;
    head = (head + 1) % 8;
    if (count < 8) ++count;
    return false;
}

inline void updateMainCamera(const edpe::CameraProjection& camera, unsigned long long cur_frame) {
    static unsigned s_revisit_rejected = 0;
    if (isRevisitedCamera(camera)) {
        if (++s_revisit_rejected <= 8 || (s_revisit_rejected % 512) == 0) {
            wchar_t msg[160];
            swprintf_s(msg, L"EDPE: ignored revisited camera sample #%u frame=%llu",
                s_revisit_rejected, cur_frame);
            EdpeLog(msg);
        }
        return;
    }
    if (s_camera_frame == ~0ull) {
        s_camera_previous = camera;
        s_camera_previous_frame = cur_frame;
    } else if (s_camera_frame != cur_frame) {
        s_camera_previous = s_camera_now;
        s_camera_previous_frame = s_camera_frame;
    }
    s_camera_now = camera;
    s_camera_frame = cur_frame;
}
Microsoft::WRL::ComPtr<ID3D11Texture2D> scene_depth_source;
Microsoft::WRL::ComPtr<ID3D11Texture2D> scene_ldr_source;
unsigned long long scene_ldr_frame = ~0ull;
Microsoft::WRL::ComPtr<ID3D11Texture2D> scene_hdr_color_source;
unsigned long long scene_hdr_color_frame = ~0ull;
std::array<ID3D11Resource*, 4> s_gbuffer_resources{}; // Weak; compared within current frame only.
std::atomic<UINT> s_observed_scene_width{0};
std::atomic<UINT> s_observed_scene_height{0};
unsigned long long s_observed_scene_frame = ~0ull;
std::atomic<UINT> s_swapchain_width{0};
std::atomic<UINT> s_swapchain_height{0};
std::atomic<ID3D11Resource*> s_swapchain_backbuffer{nullptr};
unsigned long long motion_depth_frame = ~0ull;
Microsoft::WRL::ComPtr<ID3D11Texture2D> motion_depth;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motion_depth_view;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> motion_color_source;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motion_color_view;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motion_color_snapshot;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motion_early_color_view;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motion_early_color_snapshot;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motion_depth_snapshot;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> motion_clean_color_target;
UINT motion_color_snapshot_width = 0;
UINT motion_color_snapshot_height = 0;
UINT motion_early_color_width = 0;
UINT motion_early_color_height = 0;
UINT motion_depth_snapshot_width = 0;
UINT motion_depth_snapshot_height = 0;
unsigned motion_depth_snapshot_index = 0;
bool motion_early_color_from_hud = false;
bool hud_first_match_seen = false;
unsigned hud_after_copy_draws = 0;
unsigned glass_after_copy_draws = 0;
unsigned glass_replayed_draws = 0;
unsigned glass_replay_declined = 0;
bool glass_replay_reason_logged = false;
struct HudShaderSample {
    std::uint64_t vs = 0;
    std::uint64_t ps = 0;
    unsigned draws = 0;
    unsigned after_hud = 0;
    bool fanout_checked = false;
};
std::array<HudShaderSample, 256> hud_shader_samples{};
size_t hud_shader_sample_count = 0;
unsigned hud_shader_sample_overflow = 0;
unsigned hud_shader_missing_hash = 0;
unsigned long long motion_color_frame = ~0ull;
bool motion_color_bound = false;
bool motion_color_rebound = false;
bool motion_color_first_exit_logged = false;
std::atomic<bool> hud_draw_probe_active{false};
unsigned hud_draw_hook_mask = 0;
unsigned hud_draw_rearmed_mask = 0;
unsigned hud_target_draws = 0;
Microsoft::WRL::ComPtr<ID3D11Texture2D> motion_grid_readback;
std::unique_ptr<edpe::MotionPass> motion_pass;
std::unique_ptr<edpe::UiCompositePass> ui_composite_pass;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motion_snapshot;
UINT motion_snapshot_width = 0;
UINT motion_snapshot_height = 0;
unsigned motion_grid_wait = 0;
unsigned motion_hook_watch = 0;
struct PairDepthSample {
    ID3D11DepthStencilView* view = nullptr;
    ID3D11Texture2D* staging = nullptr;
    unsigned long long after_present = 0;
    UINT width = 0;
    UINT height = 0;
    unsigned wait = 0;
};
std::array<PairDepthSample, 2> camera_pair_depth{}; // One-shot diagnostic only.
struct ConstantBufferSample {
    ID3D11Buffer* buffer = nullptr;
    unsigned bind = 0;
    unsigned long long after_present = 0;
    unsigned wait = 0;
    std::atomic<bool> claimed{false};
    std::atomic<bool> ready{false};
};
std::array<ConstantBufferSample, 9> constant_buffer_samples{}; // Snapshot, adjacent-frame, and alternate-projection probes.
struct PipelineSample {
    ID3D11Query* query = nullptr;
    unsigned bind = 0;
    unsigned long long after_present = 0;
    unsigned wait = 0;
    bool active = false;
};
std::array<PipelineSample, 4> pipeline_samples{}; // Requested bind intervals 1, 2, 3, and 6.
std::atomic<bool> vs_buffer_probe_active{false};
std::atomic<unsigned> vs_probe_bind_ordinal{0};
std::atomic<unsigned> vs_buffer_calls{0};
std::atomic<unsigned> vs_scene_buffer_calls{0};
std::atomic<unsigned> vs_scene_buffer_switches{0};
std::atomic<ID3D11Buffer*> vs_first_scene_buffer{nullptr}; // Identity only.
std::atomic<ID3D11Buffer*> vs_scene_buffer{nullptr};
std::atomic<bool> vs_scene_buffer_bound{false};
std::atomic<bool> draw_probe_active{false};
std::atomic<unsigned> draw_indexed_calls{0};
std::atomic<unsigned> draw_indexed_scene_calls{0};
std::atomic<unsigned> draw_calls{0};
std::atomic<unsigned> draw_scene_calls{0};
std::atomic<unsigned> draw_indexed_instanced_calls{0};
std::atomic<unsigned> draw_indexed_instanced_scene_calls{0};
std::atomic<unsigned> draw_instanced_calls{0};
std::atomic<unsigned> draw_instanced_scene_calls{0};
std::atomic<bool> first_scene_draw_seen{false};
std::atomic<ID3D11Buffer*> first_scene_draw_buffer{nullptr}; // Identity only.
std::array<unsigned char, 65536> first_scene_shader_bytecode{}; // One-shot diagnostic.
std::atomic<UINT> first_scene_shader_bytes{0};
struct SceneShaderSample {
    Microsoft::WRL::ComPtr<ID3D11VertexShader> shader;
    unsigned draws = 0;
};
std::mutex scene_shader_mutex;
std::array<SceneShaderSample, 64> scene_shaders{}; // Bounded one-shot diagnostic.
size_t scene_shader_count = 0;
unsigned scene_shader_overflow_draws = 0;
std::atomic<bool> scene_shader_probe_active{false};
std::atomic<bool> draw_hook_active{false};
std::atomic<bool> draw_indexed_instanced_hook_active{false};
std::atomic<bool> draw_instanced_hook_active{false};
std::atomic<ID3D11Buffer*> scene_map_target{nullptr}; // Weak; valid only for this bind interval.
struct MappedBufferEntry {
    ID3D11Resource* resource = nullptr;
    void* mapped_ptr = nullptr;
};
static constexpr size_t kMaxTrackedMappedBuffers = 16;
static MappedBufferEntry s_tracked_mapped_buffers[kMaxTrackedMappedBuffers]{};
static std::mutex s_tracked_mapped_buffers_mutex;

inline void trackMappedBuffer(ID3D11Resource* resource, void* ptr) {
    if (!resource) return;
    std::lock_guard lock(s_tracked_mapped_buffers_mutex);
    for (size_t i = 0; i < kMaxTrackedMappedBuffers; ++i) {
        if (s_tracked_mapped_buffers[i].resource == resource || s_tracked_mapped_buffers[i].resource == nullptr) {
            s_tracked_mapped_buffers[i].resource = resource;
            s_tracked_mapped_buffers[i].mapped_ptr = ptr;
            return;
        }
    }
}

inline void* popMappedBuffer(ID3D11Resource* resource) {
    if (!resource) return nullptr;
    std::lock_guard lock(s_tracked_mapped_buffers_mutex);
    for (size_t i = 0; i < kMaxTrackedMappedBuffers; ++i) {
        if (s_tracked_mapped_buffers[i].resource == resource) {
            void* ptr = s_tracked_mapped_buffers[i].mapped_ptr;
            s_tracked_mapped_buffers[i].resource = nullptr;
            s_tracked_mapped_buffers[i].mapped_ptr = nullptr;
            return ptr;
        }
    }
    return nullptr;
}

inline void clearTrackedMappedBuffers() {
    std::lock_guard lock(s_tracked_mapped_buffers_mutex);
    for (size_t i = 0; i < kMaxTrackedMappedBuffers; ++i) {
        s_tracked_mapped_buffers[i].resource = nullptr;
        s_tracked_mapped_buffers[i].mapped_ptr = nullptr;
    }
}

inline void clearTrackedMappedBuffersExcept(ID3D11Resource* keep) {
    std::lock_guard lock(s_tracked_mapped_buffers_mutex);
    for (size_t i = 0; i < kMaxTrackedMappedBuffers; ++i) {
        if (s_tracked_mapped_buffers[i].resource != keep) {
            s_tracked_mapped_buffers[i].resource = nullptr;
            s_tracked_mapped_buffers[i].mapped_ptr = nullptr;
        }
    }
}

static std::atomic<bool> s_hud_probe_armed_once{false};
std::atomic<bool> map_hook_active{false};
std::atomic<bool> unmap_hook_active{false};
std::atomic<unsigned> scene_maps{0};
std::atomic<unsigned> scene_unmaps{0};
std::atomic<unsigned> scene_maps_after_draw{0};
std::atomic<unsigned> scene_maps_before_bind{0};
std::atomic<void*> scene_mapped_data{nullptr};
std::atomic<unsigned> scene_hashed_writes{0};
std::atomic<unsigned> scene_camera_changes{0};
std::atomic<unsigned> scene_projection_changes{0};
std::atomic<unsigned long long> scene_first_camera_hash{0};
std::atomic<unsigned long long> scene_last_camera_hash{0};
std::atomic<unsigned long long> scene_first_projection_hash{0};
std::atomic<unsigned long long> scene_last_projection_hash{0};

// Phase 5: Projection Jitter State
std::atomic<bool> jitter_enabled{true};
std::atomic<bool> jitter_test_override{false};
std::atomic<unsigned> jittered_writes_count{0};
std::atomic<unsigned> jitter_rejection_count{0};
std::atomic<unsigned long long> jitter_frame_index{0};
std::mutex jitter_mutex;
edpe::ProjectionJitter active_projection_jitter{};
UINT active_render_width = 0;
UINT active_render_height = 0;

std::atomic<bool> s_jitter_invert_x{false};
std::atomic<bool> s_jitter_invert_y{false};
std::atomic<bool> s_jitter_swap_xy{false};
std::atomic<float> s_jitter_scale{1.0f};
std::atomic<unsigned> s_jitter_phase_count{32};
std::atomic<unsigned> s_active_jitter_phases{32};
std::atomic<bool> s_preserve_native_ui{true};

std::atomic<bool> s_jitter_debug_view{false};
std::atomic<float> s_jitter_debug_sensitivity{1.5f};
std::atomic<float> s_jitter_debug_threshold{0.02f};
std::atomic<int> s_jitter_debug_mode{0};


std::atomic<bool> s_zero_motion_vectors{false};
std::atomic<bool> s_motion_invert_x{false};
std::atomic<bool> s_motion_invert_y{false};
std::atomic<float> s_motion_scale{1.0f};
static bool s_config_loaded = false;

void queueConstantBufferSample(ID3D11DeviceContext* context, ID3D11Buffer* source,
    const D3D11_BUFFER_DESC& source_desc, unsigned bind_ordinal);
void captureEarlyMotionColor(ID3D11DeviceContext* context, bool from_hud);
void tryTemporalUpscale(ID3D11DeviceContext* context, unsigned long long rendered, ID3D11Texture2D* backbuffer);
void renderJitterDebugView(ID3D11DeviceContext* context, unsigned long long rendered, ID3D11Texture2D* backbuffer);
void rearmContextHooks();

bool sameRenderTargetResource(ID3D11RenderTargetView* view,
    ID3D11RenderTargetView* source) {
    if (!view || !source) return false;
    if (view == source) return true;
    Microsoft::WRL::ComPtr<ID3D11Resource> a, b;
    view->GetResource(&a);
    source->GetResource(&b);
    if (!a || !b) return false;
    if (a.Get() == b.Get()) return true;
    Microsoft::WRL::ComPtr<IUnknown> identity_a, identity_b;
    return SUCCEEDED(a.As(&identity_a)) && SUCCEEDED(b.As(&identity_b)) &&
        identity_a.Get() == identity_b.Get();
}

// Adapted from EDVR's exact glass-state gate. Only a one-shot diagnostic
// replay can pass this; unsupported state leaves the game's draw untouched.
const wchar_t* glassReplayRefusal(ID3D11DeviceContext* context, char kind,
    UINT instances) {
    if (!motion_clean_color_target || !motion_early_color_from_hud)
        return L"clean HDR target unavailable";
    if (kind != 'X' || instances != 1)
        return L"glass draw shape differs from indexed-instanced x1";
    for (const auto& sample : pipeline_samples)
        if (sample.active) return L"EDPE pipeline query is active";
    ID3D11RenderTargetView* bound[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, bound, &depth);
    const bool target_matches = sameRenderTargetResource(bound[0], motion_color_source.Get());
    bool multiple = false;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        multiple |= i != 0 && bound[i] != nullptr;
        if (bound[i]) bound[i]->Release();
    }
    if (!target_matches || multiple || !depth) return L"glass target or depth differs";
    ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
        0, D3D11_PS_CS_UAV_REGISTER_COUNT, uavs);
    bool has_uav = false;
    for (auto* uav : uavs) {
        has_uav |= uav != nullptr;
        if (uav) uav->Release();
    }
    if (has_uav) return L"glass draw uses an output UAV";
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    context->IAGetPrimitiveTopology(&topology);
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
        return L"glass topology differs from triangle list";
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> gs;
    Microsoft::WRL::ComPtr<ID3D11HullShader> hs;
    Microsoft::WRL::ComPtr<ID3D11DomainShader> ds;
    context->GSGetShader(&gs, nullptr, nullptr);
    context->HSGetShader(&hs, nullptr, nullptr);
    context->DSGetShader(&ds, nullptr, nullptr);
    if (gs || hs || ds) return L"glass uses another shader stage";
    ID3D11Buffer* stream_output[D3D11_SO_BUFFER_SLOT_COUNT]{};
    context->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, stream_output);
    bool has_stream_output = false;
    for (auto* buffer : stream_output) {
        has_stream_output |= buffer != nullptr;
        if (buffer) buffer->Release();
    }
    if (has_stream_output) return L"glass uses stream output";
    Microsoft::WRL::ComPtr<ID3D11Predicate> predicate;
    BOOL predicate_value = FALSE;
    context->GetPredication(&predicate, &predicate_value);
    if (predicate) return L"glass draw is predicated";
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend;
    float factors[4]{};
    UINT sample_mask = 0;
    context->OMGetBlendState(&blend, factors, &sample_mask);
    if (!blend || sample_mask != ~0u) return L"glass blend state or sample mask differs";
    D3D11_BLEND_DESC blend_desc{};
    blend->GetDesc(&blend_desc);
    const auto& rt = blend_desc.RenderTarget[0];
    if (blend_desc.AlphaToCoverageEnable || !rt.BlendEnable ||
        rt.RenderTargetWriteMask != D3D11_COLOR_WRITE_ENABLE_ALL ||
        rt.SrcBlend != D3D11_BLEND_ONE || rt.DestBlend != D3D11_BLEND_SRC1_COLOR ||
        rt.BlendOp != D3D11_BLEND_OP_ADD || rt.SrcBlendAlpha != D3D11_BLEND_ONE ||
        rt.DestBlendAlpha != D3D11_BLEND_SRC1_ALPHA ||
        rt.BlendOpAlpha != D3D11_BLEND_OP_ADD)
        return L"glass dual-source blend equation differs";
    Microsoft::WRL::ComPtr<ID3D11BlendState1> blend1;
    if (SUCCEEDED(blend.As(&blend1))) {
        D3D11_BLEND_DESC1 blend_desc1{};
        blend1->GetDesc1(&blend_desc1);
        if (blend_desc1.RenderTarget[0].LogicOpEnable)
            return L"glass render target uses a logic operation";
    }
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> stencil;
    UINT stencil_ref = 0;
    context->OMGetDepthStencilState(&stencil, &stencil_ref);
    if (!stencil) return L"glass depth/stencil state absent";
    D3D11_DEPTH_STENCIL_DESC depth_desc{};
    stencil->GetDesc(&depth_desc);
    const auto& front = depth_desc.FrontFace;
    const auto& back = depth_desc.BackFace;
    if (!depth_desc.DepthEnable || depth_desc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO ||
        depth_desc.DepthFunc != D3D11_COMPARISON_GREATER_EQUAL ||
        !depth_desc.StencilEnable || depth_desc.StencilReadMask != 0 ||
        depth_desc.StencilWriteMask != 4 || stencil_ref != 4 ||
        front.StencilFunc != D3D11_COMPARISON_ALWAYS ||
        front.StencilFailOp != D3D11_STENCIL_OP_KEEP ||
        front.StencilDepthFailOp != D3D11_STENCIL_OP_KEEP ||
        front.StencilPassOp != D3D11_STENCIL_OP_REPLACE ||
        back.StencilFunc != D3D11_COMPARISON_ALWAYS ||
        back.StencilFailOp != D3D11_STENCIL_OP_KEEP ||
        back.StencilDepthFailOp != D3D11_STENCIL_OP_KEEP ||
        (back.StencilPassOp != D3D11_STENCIL_OP_KEEP &&
         back.StencilPassOp != D3D11_STENCIL_OP_REPLACE)) {
        if (!glass_replay_reason_logged) {
            wchar_t message[256];
            swprintf_s(message,
                L"EDPE: glass depth state enabled=%u write=%u func=%u stencil=%u read=%u writeMask=%u ref=%u",
                depth_desc.DepthEnable, static_cast<unsigned>(depth_desc.DepthWriteMask),
                static_cast<unsigned>(depth_desc.DepthFunc), depth_desc.StencilEnable,
                depth_desc.StencilReadMask, depth_desc.StencilWriteMask, stencil_ref);
            EdpeLog(message);
            swprintf_s(message,
                L"EDPE: glass stencil front=%u/%u/%u/%u back=%u/%u/%u/%u (fail/depthFail/pass/func)",
                front.StencilFailOp, front.StencilDepthFailOp, front.StencilPassOp,
                front.StencilFunc, back.StencilFailOp, back.StencilDepthFailOp,
                back.StencilPassOp, back.StencilFunc);
            EdpeLog(message);
        }
        return L"glass depth/stencil state differs";
    }
    // With read mask 0, write mask/ref 4, REPLACE writes the same stencil bit twice.
    return nullptr;
}

void declineGlassReplay(const wchar_t* reason) {
    ++glass_replay_declined;
    if (glass_replay_reason_logged) return;
    wchar_t message[160];
    swprintf_s(message, L"EDPE: glass replay declined: %ls", reason);
    EdpeLog(message);
    glass_replay_reason_logged = true;
}

void probeWorldShaderFanout(ID3D11DeviceContext* context,
    ID3D11PixelShader* shader, std::uint64_t vs, std::uint64_t ps) {
    UINT bytes = 0;
    shader->GetPrivateData(kEdpePixelBytecodeGuid, &bytes, nullptr);
    const wchar_t* result = L"bytecode unavailable";
    std::wstring detail;
    if (bytes && bytes <= 65536) {
        try {
            std::vector<BYTE> source(bytes), patched;
            if (SUCCEEDED(shader->GetPrivateData(kEdpePixelBytecodeGuid,
                    &bytes, source.data()))) {
                std::string reason;
                if (edpe::colourFanout(source.data(), bytes, patched, reason)) {
                    Microsoft::WRL::ComPtr<ID3D11Device> device;
                    Microsoft::WRL::ComPtr<ID3D11PixelShader> candidate;
                    context->GetDevice(&device);
                    result = device && SUCCEEDED(device->CreatePixelShader(
                        patched.data(), patched.size(), nullptr, &candidate))
                        ? L"ready" : L"CreatePixelShader failed";
                } else {
                    result = L"DXBC fanout declined";
                    detail.assign(reason.begin(), reason.end());
                }
            } else result = L"bytecode read failed";
        } catch (...) { result = L"allocation failed"; }
    }
    wchar_t message[256];
    swprintf_s(message,
        L"EDPE: world fanout probe VS=%016llX PS=%016llX bytes=%u result=%ls %ls",
        vs, ps, bytes, result, detail.c_str());
    EdpeLog(message);
}

bool observeHudDraw(ID3D11DeviceContext* context, char kind, UINT instances) {
    if (!hud_draw_probe_active.load(std::memory_order_relaxed) ||
        !motion_color_source || !motion_color_bound) return false;
    struct DrawProbeTimer {
        LARGE_INTEGER start{};
        bool active = false;
        DrawProbeTimer() {
            static uint32_t counter = 0;
            if (((++counter & 15) == 0) && edpe::Profiler::instance().isEnabled()) {
                active = true;
                QueryPerformanceCounter(&start);
            }
        }
        ~DrawProbeTimer() {
            if (active) {
                LARGE_INTEGER end;
                QueryPerformanceCounter(&end);
                edpe::Profiler::instance().accumulate(edpe::ProfileZone::DrawProbe, (end.QuadPart - start.QuadPart) * 16);
            }
        }
    } drawTimer;
    ++hud_target_draws;
    ID3D11RenderTargetView* bound[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, bound, nullptr);
    bool target_matches = false;
    for (auto* view : bound) {
        target_matches |= sameRenderTargetResource(view, motion_color_source.Get());
        if (view) view->Release();
    }
    if (!target_matches) return false;
    if (hud_first_match_seen) ++hud_after_copy_draws;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
    context->VSGetShader(&vs, nullptr, nullptr);
    context->PSGetShader(&ps, nullptr, nullptr);
    if (!vs || !ps) {
        ++hud_shader_missing_hash;
        return false;
    }
    std::uint64_t vs_hash = 0, ps_hash = 0;
    UINT size = sizeof(vs_hash);
    if (FAILED(vs->GetPrivateData(kEdpeShaderHashGuid, &size, &vs_hash)) ||
        size != sizeof(vs_hash)) {
        ++hud_shader_missing_hash;
        return false;
    }
    size = sizeof(ps_hash);
    if (FAILED(ps->GetPrivateData(kEdpeShaderHashGuid, &size, &ps_hash)) ||
        size != sizeof(ps_hash)) {
        ++hud_shader_missing_hash;
        return false;
    }
    auto sample = std::find_if(hud_shader_samples.begin(),
        hud_shader_samples.begin() + hud_shader_sample_count,
        [=](const HudShaderSample& item) { return item.vs == vs_hash && item.ps == ps_hash; });
    if (sample != hud_shader_samples.begin() + hud_shader_sample_count) {
        ++sample->draws;
        if (hud_first_match_seen) ++sample->after_hud;
    }
    else if (hud_shader_sample_count < hud_shader_samples.size())
        hud_shader_samples[hud_shader_sample_count++] =
            {vs_hash, ps_hash, 1, hud_first_match_seen ? 1u : 0u, false};
    else ++hud_shader_sample_overflow;
    if (hud_first_match_seen) {
        const bool world_candidate =
            (vs_hash == 0x9AEC596A2B036EA6ull &&
             ps_hash == 0x3789CA2062E196FBull) ||
            (vs_hash == 0x025B4B9FF54622EDull &&
             ps_hash == 0xC5A5C7E8216CB9AFull) ||
            (vs_hash == 0x3D05E7CF11AC9BEEull &&
             ps_hash == 0x912477AEF6958379ull);
        if (world_candidate && sample != hud_shader_samples.begin() + hud_shader_sample_count &&
            !sample->fanout_checked) {
            sample->fanout_checked = true;
            probeWorldShaderFanout(context, ps.Get(), vs_hash, ps_hash);
        }
        if (vs_hash == 0xF512712C40D93C12ull &&
            ps_hash == 0x4A71EB0D34E9F2EFull) {
            ++glass_after_copy_draws;
            if (const auto* reason = glassReplayRefusal(context, kind, instances)) {
                declineGlassReplay(reason);
                return false;
            }
            return true;
        }
        return false;
    }
    // Exact UI shader pairs documented by EDVR; validate them in Desktop captures.
    const bool hud = (vs_hash == 0xB7790CBFC6554097ull &&
                      ps_hash == 0x8DEF46452FA459F5ull) ||
                     (vs_hash == 0x81216C77F90DEDD6ull &&
                      ps_hash == 0xA2965EC2931A39C8ull) ||
                     (vs_hash == 0xE508648660A352B2ull &&
                      ps_hash == 0x63ABD86359B57D01ull);
    if (!hud) return false;
    if (!hud_first_match_seen) {
        wchar_t message[160];
        swprintf_s(message, L"EDPE: HUD draw on HDR target VS=%016llX PS=%016llX frame=%llu",
            vs_hash, ps_hash, motion_color_frame);
        EdpeLog(message);
        hud_first_match_seen = true;
        captureEarlyMotionColor(context, true);
    }
    return false;
}

void replayGlassDraw(ID3D11DeviceContext* context, UINT index_count,
    UINT instance_count, UINT start_index, INT base_vertex, UINT start_instance) {
    const auto set_targets = original.load(std::memory_order_acquire);
    const auto draw = original_draw_indexed_instanced.load(std::memory_order_acquire);
    if (!set_targets || !draw || !motion_clean_color_target) {
        declineGlassReplay(L"draw hook or clean target unavailable");
        return;
    }
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> game_target;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(1, game_target.GetAddressOf(), depth.GetAddressOf());
    if (!sameRenderTargetResource(game_target.Get(), motion_color_source.Get()) || !depth) {
        declineGlassReplay(L"game target changed before duplicate draw");
        return;
    }
    ID3D11RenderTargetView* clean = motion_clean_color_target.Get();
    set_targets(context, 1, &clean, depth.Get());
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rebound;
    context->OMGetRenderTargets(1, rebound.GetAddressOf(), nullptr);
    if (rebound.Get() == clean) {
        draw(context, index_count, instance_count, start_index, base_vertex, start_instance);
        ++glass_replayed_draws;
    } else {
        declineGlassReplay(L"clean target binding failed");
    }
    ID3D11RenderTargetView* restore = game_target.Get();
    set_targets(context, 1, &restore, depth.Get());
}

void observeFirstSceneDraw(ID3D11DeviceContext* context) {
    if (!vs_scene_buffer_bound.load(std::memory_order_relaxed)) return;
    const bool first = !first_scene_draw_seen.exchange(true, std::memory_order_relaxed);
    if (first) {
        ID3D11Buffer* buffer = nullptr;
        context->VSGetConstantBuffers(1, 1, &buffer);
        first_scene_draw_buffer.store(buffer, std::memory_order_relaxed);
        if (buffer) buffer->Release();
    }
    if (!first && !scene_shader_probe_active.load(std::memory_order_acquire)) return;
    ID3D11VertexShader* shader = nullptr;
    context->VSGetShader(&shader, nullptr, nullptr);
    if (!shader) return;
    if (first) {
        UINT bytes = static_cast<UINT>(first_scene_shader_bytecode.size());
        if (SUCCEEDED(shader->GetPrivateData(kEdpeVertexBytecodeGuid, &bytes,
                first_scene_shader_bytecode.data()))) {
            first_scene_shader_bytes.store(bytes, std::memory_order_release);
            scene_shader_probe_active.store(true, std::memory_order_release);
        }
    }
    if (scene_shader_probe_active.load(std::memory_order_acquire)) {
        std::lock_guard lock(scene_shader_mutex);
        for (size_t i = 0; i < scene_shader_count; ++i) {
            if (scene_shaders[i].shader.Get() == shader) {
                ++scene_shaders[i].draws;
                shader->Release();
                return;
            }
        }
        if (scene_shader_count < scene_shaders.size()) {
            auto& sample = scene_shaders[scene_shader_count++];
            sample.shader.Attach(shader); // VSGetShader transferred one reference.
            sample.draws = 1;
            // Exact one-run diagnostic signature from the captured CB0 shader.
            UINT bytes = 0;
            shader->GetPrivateData(kEdpeVertexBytecodeGuid, &bytes, nullptr);
            if (bytes == 4884) {
                std::array<unsigned char, 4884> bytecode{};
                if (SUCCEEDED(shader->GetPrivateData(kEdpeVertexBytecodeGuid,
                        &bytes, bytecode.data()))) {
                    unsigned long long hash = 14695981039346656037ull;
                    for (unsigned char value : bytecode)
                        hash = (hash ^ value) * 1099511628211ull;
                    if (hash == 0x22D371BE9A186044ull) {
                        ID3D11Buffer* buffer = nullptr;
                        context->VSGetConstantBuffers(0, 1, &buffer);
                        if (buffer) {
                            D3D11_BUFFER_DESC desc{};
                            buffer->GetDesc(&desc);
                            wchar_t message[160];
                            swprintf_s(message,
                                L"EDPE: alternate VS CB0 buffer=%p bytes=%u usage=%u bind=0x%X",
                                buffer, desc.ByteWidth, static_cast<unsigned>(desc.Usage),
                                desc.BindFlags);
                            EdpeLog(message);
                            if (desc.ByteWidth >= 128 &&
                                (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER))
                                queueConstantBufferSample(context, buffer, desc,
                                    kAlternateProjectionSample);
                            buffer->Release();
                        }
                    }
                }
            }
            return;
        }
        ++scene_shader_overflow_draws;
    }
    shader->Release();
}

void dumpFirstSceneShader() {
    const UINT bytes = first_scene_shader_bytes.load(std::memory_order_acquire);
    if (!bytes) return;
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    wchar_t* name = wcsrchr(path, L'\\');
    if (!name) return;
    const size_t prefix = name + 1 - path;
    if (swprintf_s(name + 1, MAX_PATH - prefix, L"edpe-vs-bind%u.dxbc",
            vs_probe_bind_ordinal.load(std::memory_order_relaxed)) <= 0) return;
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const BOOL saved = WriteFile(file, first_scene_shader_bytecode.data(),
        bytes, &written, nullptr);
    CloseHandle(file);
    wchar_t message[160];
    swprintf_s(message, L"EDPE: first scene shader dump bind=%u bytes=%u saved=%u",
        vs_probe_bind_ordinal.load(std::memory_order_relaxed),
        bytes, saved && written == bytes);
    EdpeLog(message);
}

void dumpSceneShaders() {
    if (!scene_shader_probe_active.exchange(false, std::memory_order_acq_rel)) return;
    std::lock_guard lock(scene_shader_mutex);
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* name = length && length < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    const size_t prefix = name ? name + 1 - path : 0;
    std::array<unsigned char, 65536> bytecode{};
    for (size_t i = 0; i < scene_shader_count; ++i) {
        auto& sample = scene_shaders[i];
        UINT bytes = static_cast<UINT>(bytecode.size());
        bool saved = false;
        if (name && SUCCEEDED(sample.shader->GetPrivateData(kEdpeVertexBytecodeGuid,
                &bytes, bytecode.data())) &&
            swprintf_s(name + 1, MAX_PATH - prefix, L"edpe-vs-bind%u-%zu.dxbc",
                vs_probe_bind_ordinal.load(std::memory_order_relaxed), i) > 0) {
            const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ,
                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                saved = WriteFile(file, bytecode.data(), bytes, &written, nullptr) &&
                    written == bytes;
                CloseHandle(file);
            }
        }
        wchar_t message[160];
        swprintf_s(message, L"EDPE: scene shader bind=%u index=%zu draws=%u bytes=%u saved=%u",
            vs_probe_bind_ordinal.load(std::memory_order_relaxed), i,
            sample.draws, bytes, saved);
        EdpeLog(message);
        sample.shader.Reset();
        sample.draws = 0;
    }
    wchar_t message[160];
    swprintf_s(message, L"EDPE: scene shader census bind=%u unique=%zu overflowDraws=%u",
        vs_probe_bind_ordinal.load(std::memory_order_relaxed), scene_shader_count,
        scene_shader_overflow_draws);
    EdpeLog(message);
    scene_shader_count = 0;
    scene_shader_overflow_draws = 0;
}

void STDMETHODCALLTYPE observedVSSetConstantBuffers(ID3D11DeviceContext* context,
    UINT start, UINT count, ID3D11Buffer* const* buffers) {
    const auto forward = original_vs_set_buffers.load(std::memory_order_acquire);
    forward(context, start, count, buffers);
    if (context != observed_context.load(std::memory_order_acquire) ||
        !vs_buffer_probe_active.load(std::memory_order_acquire)) return;
    vs_buffer_calls.fetch_add(1, std::memory_order_relaxed);
    if (start > 1 || count <= 1 - start) return;
    ID3D11Buffer* buffer = buffers ? buffers[1 - start] : nullptr;
    D3D11_BUFFER_DESC desc{};
    if (buffer) buffer->GetDesc(&desc);
    const bool scene = desc.ByteWidth == 5376 && desc.Usage == D3D11_USAGE_DYNAMIC;
    vs_scene_buffer_bound.store(scene, std::memory_order_relaxed);
    if (scene) {
        const unsigned scene_bind = vs_scene_buffer_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        ID3D11Buffer* expected = nullptr;
        if (vs_first_scene_buffer.compare_exchange_strong(expected, buffer, std::memory_order_relaxed)) {
            scene_map_target.store(buffer, std::memory_order_release);
            if (vs_probe_bind_ordinal.load(std::memory_order_relaxed) == 3)
                queueConstantBufferSample(context, buffer, desc, 0); // Existing interval-3 diagnostic.
        }
        if (scene_bind == 51 && vs_probe_bind_ordinal.load(std::memory_order_relaxed) == 3)
            queueConstantBufferSample(context, buffer, desc, 51);
        auto* previous = vs_scene_buffer.exchange(buffer, std::memory_order_relaxed);
        if (previous && previous != buffer)
            vs_scene_buffer_switches.fetch_add(1, std::memory_order_relaxed);
    }
}

void STDMETHODCALLTYPE observedDrawIndexed(ID3D11DeviceContext* context, UINT count,
    UINT start, INT base) {
    if (context == observed_context.load(std::memory_order_acquire))
        observeHudDraw(context, 'I', 1);
    if (context == observed_context.load(std::memory_order_acquire) &&
        draw_probe_active.load(std::memory_order_acquire)) {
        draw_indexed_calls.fetch_add(1, std::memory_order_relaxed);
        if (vs_scene_buffer_bound.load(std::memory_order_relaxed))
            draw_indexed_scene_calls.fetch_add(1, std::memory_order_relaxed);
        observeFirstSceneDraw(context);
    }
    original_draw_indexed.load(std::memory_order_acquire)(context, count, start, base);
}

void STDMETHODCALLTYPE observedDraw(ID3D11DeviceContext* context, UINT count, UINT start) {
    if (context == observed_context.load(std::memory_order_acquire))
        observeHudDraw(context, 'D', 1);
    if (context == observed_context.load(std::memory_order_acquire) &&
        draw_probe_active.load(std::memory_order_acquire)) {
        draw_calls.fetch_add(1, std::memory_order_relaxed);
        if (vs_scene_buffer_bound.load(std::memory_order_relaxed))
            draw_scene_calls.fetch_add(1, std::memory_order_relaxed);
        observeFirstSceneDraw(context);
    }
    original_draw.load(std::memory_order_acquire)(context, count, start);
}

void STDMETHODCALLTYPE observedDrawIndexedInstanced(ID3D11DeviceContext* context,
    UINT index_count, UINT instance_count, UINT start_index, INT base_vertex,
    UINT start_instance) {
    const bool replay = context == observed_context.load(std::memory_order_acquire) &&
        observeHudDraw(context, 'X', instance_count);
    if (context == observed_context.load(std::memory_order_acquire) &&
        draw_probe_active.load(std::memory_order_acquire)) {
        draw_indexed_instanced_calls.fetch_add(1, std::memory_order_relaxed);
        if (vs_scene_buffer_bound.load(std::memory_order_relaxed))
            draw_indexed_instanced_scene_calls.fetch_add(1, std::memory_order_relaxed);
        observeFirstSceneDraw(context);
    }
    original_draw_indexed_instanced.load(std::memory_order_acquire)(context,
        index_count, instance_count, start_index, base_vertex, start_instance);
    if (replay) replayGlassDraw(context, index_count, instance_count,
        start_index, base_vertex, start_instance);
}

void STDMETHODCALLTYPE observedDrawInstanced(ID3D11DeviceContext* context,
    UINT vertex_count, UINT instance_count, UINT start_vertex, UINT start_instance) {
    if (context == observed_context.load(std::memory_order_acquire))
        observeHudDraw(context, 'N', instance_count);
    if (context == observed_context.load(std::memory_order_acquire) &&
        draw_probe_active.load(std::memory_order_acquire)) {
        draw_instanced_calls.fetch_add(1, std::memory_order_relaxed);
        if (vs_scene_buffer_bound.load(std::memory_order_relaxed))
            draw_instanced_scene_calls.fetch_add(1, std::memory_order_relaxed);
        observeFirstSceneDraw(context);
    }
    original_draw_instanced.load(std::memory_order_acquire)(context,
        vertex_count, instance_count, start_vertex, start_instance);
}

void beginHudDrawProbe() {
    if (!patched_table || hud_draw_hook_mask ||
        draw_probe_active.load(std::memory_order_acquire)) return;
    auto install = [&](size_t slot, auto& original_fn, auto observer, unsigned bit) {
        auto forward = reinterpret_cast<decltype(observer)>(patched_table[slot]);
        if (!forward || forward == observer) return;
        original_fn.store(forward, std::memory_order_release);
        if (patchSlot(patched_table, slot, reinterpret_cast<void*>(forward),
                reinterpret_cast<void*>(observer))) hud_draw_hook_mask |= bit;
    };
    install(kDrawIndexed, original_draw_indexed, &observedDrawIndexed, 1);
    install(kDraw, original_draw, &observedDraw, 2);
    install(kDrawIndexedInstanced, original_draw_indexed_instanced,
        &observedDrawIndexedInstanced, 4);
    install(kDrawInstanced, original_draw_instanced, &observedDrawInstanced, 8);
    hud_target_draws = 0;
    hud_draw_rearmed_mask = 0;
    hud_shader_sample_count = 0;
    hud_shader_sample_overflow = 0;
    hud_shader_missing_hash = 0;
    hud_first_match_seen = false;
    hud_after_copy_draws = glass_after_copy_draws = 0;
    glass_replayed_draws = glass_replay_declined = 0;
    glass_replay_reason_logged = false;
    hud_draw_probe_active.store(hud_draw_hook_mask != 0, std::memory_order_release);
    wchar_t message[128];
    swprintf_s(message, L"EDPE: HUD draw probe armed mask=0x%X frame=%llu",
        hud_draw_hook_mask, motion_color_frame);
    EdpeLog(message);
}

void rearmHudDrawProbe() {
    if (!hud_draw_probe_active.load(std::memory_order_acquire) || !patched_table) return;
    // EDVR's VTableHook::reclaim documents runtime vtable rewrites. During this
    // one-shot probe, reclaim only a slot that reverted to our known forward.
    auto rearm = [&](size_t slot, auto& original_fn, auto observer, unsigned bit) {
        if (!(hud_draw_hook_mask & bit)) return;
        auto* forward = reinterpret_cast<void*>(original_fn.load(std::memory_order_acquire));
        if (patched_table[slot] == forward &&
            patchSlot(patched_table, slot, forward, reinterpret_cast<void*>(observer)))
            hud_draw_rearmed_mask |= bit;
    };
    rearm(kDrawIndexed, original_draw_indexed, &observedDrawIndexed, 1);
    rearm(kDraw, original_draw, &observedDraw, 2);
    rearm(kDrawIndexedInstanced, original_draw_indexed_instanced,
        &observedDrawIndexedInstanced, 4);
    rearm(kDrawInstanced, original_draw_instanced, &observedDrawInstanced, 8);
}

void endHudDrawProbe() {
    if (!hud_draw_hook_mask) return;
    hud_draw_probe_active.store(false, std::memory_order_release);
    unsigned restored = 0;
    unsigned reverted = 0;
    auto restore = [&](size_t slot, auto& original_fn, auto observer, unsigned bit) {
        if (!(hud_draw_hook_mask & bit)) return;
        auto* forward = reinterpret_cast<void*>(original_fn.load(std::memory_order_acquire));
        if (patchSlot(patched_table, slot, reinterpret_cast<void*>(observer), forward))
            restored |= bit;
        else if (patched_table[slot] == forward) reverted |= bit;
    };
    restore(kDrawIndexed, original_draw_indexed, &observedDrawIndexed, 1);
    restore(kDraw, original_draw, &observedDraw, 2);
    restore(kDrawIndexedInstanced, original_draw_indexed_instanced,
        &observedDrawIndexedInstanced, 4);
    restore(kDrawInstanced, original_draw_instanced, &observedDrawInstanced, 8);
    wchar_t message[320];
    swprintf_s(message,
        L"EDPE: HUD draw probe frame=%llu mask=0x%X rearmed=0x%X restored=0x%X reverted=0x%X draws=%u hudCopy=%u afterHud=%u glassAfterHud=%u glassReplay=%u glassDeclined=%u missingHash=%u overflow=%u",
        motion_color_frame, hud_draw_hook_mask, hud_draw_rearmed_mask, restored, reverted, hud_target_draws,
        motion_early_color_from_hud, hud_after_copy_draws, glass_after_copy_draws,
        glass_replayed_draws, glass_replay_declined,
        hud_shader_missing_hash, hud_shader_sample_overflow);
    EdpeLog(message);
    for (size_t i = 0; i < hud_shader_sample_count; ++i) {
        const auto& sample = hud_shader_samples[i];
        swprintf_s(message, L"EDPE: HDR draw shader %zu VS=%016llX PS=%016llX draws=%u afterHud=%u",
            i, sample.vs, sample.ps, sample.draws, sample.after_hud);
        EdpeLog(message);
    }
    hud_draw_hook_mask = 0;
}

HRESULT STDMETHODCALLTYPE observedMap(ID3D11DeviceContext* context, ID3D11Resource* resource,
    UINT subresource, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) {
    const HRESULT result = original_map.load(std::memory_order_acquire)(
        context, resource, subresource, type, flags, mapped);
    if (s_in_edpe_present.load(std::memory_order_acquire)) return result;
    if (SUCCEEDED(result) && context == observed_context.load(std::memory_order_acquire) &&
        resource && map_hook_active.load(std::memory_order_acquire)) {
        ID3D11Resource* target = scene_map_target.load(std::memory_order_acquire);
        bool is_scene_cb = (resource == target);
        if (is_scene_cb || !target) {
            edpe::ScopedProfileTimer profTimer(edpe::ProfileZone::MapUnmap);
            if (!is_scene_cb) {
                D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
                resource->GetType(&dimension);
                if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
                    ID3D11Buffer* buffer = nullptr;
                    if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Buffer),
                            reinterpret_cast<void**>(&buffer)))) {
                        D3D11_BUFFER_DESC desc{};
                        buffer->GetDesc(&desc);
                        if (desc.ByteWidth == 5376 && desc.Usage == D3D11_USAGE_DYNAMIC &&
                            (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) {
                            scene_map_target.store(buffer, std::memory_order_release);
                            clearTrackedMappedBuffersExcept(buffer);
                            target = buffer;
                            is_scene_cb = true;
                            if (!scene_maps_before_bind.load(std::memory_order_relaxed)) {
                                scene_maps_before_bind.fetch_add(1, std::memory_order_relaxed);
                                EdpeLog(L"EDPE: scene constant buffer target discovered via Map()");
                            }
                        }
                        buffer->Release();
                    }
                }
            }
        }
        if (is_scene_cb) {
            trackMappedBuffer(resource, mapped ? mapped->pData : nullptr);
            scene_maps.fetch_add(1, std::memory_order_relaxed);
            scene_mapped_data.store(mapped ? mapped->pData : nullptr, std::memory_order_release);
            if (draw_indexed_calls.load(std::memory_order_relaxed) ||
                draw_calls.load(std::memory_order_relaxed) ||
                draw_indexed_instanced_calls.load(std::memory_order_relaxed) ||
                draw_instanced_calls.load(std::memory_order_relaxed))
                scene_maps_after_draw.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return result;
}

void STDMETHODCALLTYPE observedUnmap(ID3D11DeviceContext* context, ID3D11Resource* resource,
    UINT subresource) {
    if (!s_in_edpe_present.load(std::memory_order_acquire) &&
        context == observed_context.load(std::memory_order_acquire) &&
        resource &&
        unmap_hook_active.load(std::memory_order_acquire)) {
        ID3D11Resource* target = scene_map_target.load(std::memory_order_acquire);
        if (target && resource != target) {
            popMappedBuffer(resource);
            original_unmap.load(std::memory_order_acquire)(context, resource, subresource);
            return;
        }
        edpe::ScopedProfileTimer profTimer(edpe::ProfileZone::MapUnmap);
        void* mapped_ptr = popMappedBuffer(resource);
        if (!mapped_ptr && resource == scene_map_target.load(std::memory_order_acquire)) {
            mapped_ptr = scene_mapped_data.exchange(nullptr, std::memory_order_acq_rel);
        }
        if (mapped_ptr) {
            scene_unmaps.fetch_add(1, std::memory_order_relaxed);
            unsigned long long camera = 14695981039346656037ull;
            unsigned long long projection = camera;
            const auto* data = static_cast<const unsigned char*>(mapped_ptr);
            for (size_t i = 932 * 4; i < 944 * 4; ++i)
                camera = (camera ^ data[i]) * 1099511628211ull;
            for (size_t i = 1080 * 4; i < 1096 * 4; ++i)
                projection = (projection ^ data[i]) * 1099511628211ull;
            if (scene_hashed_writes.fetch_add(1, std::memory_order_relaxed) == 0) {
                scene_first_camera_hash.store(camera, std::memory_order_relaxed);
                scene_first_projection_hash.store(projection, std::memory_order_relaxed);
            } else {
                if (camera != scene_last_camera_hash.load(std::memory_order_relaxed))
                    scene_camera_changes.fetch_add(1, std::memory_order_relaxed);
                if (projection != scene_last_projection_hash.load(std::memory_order_relaxed))
                    scene_projection_changes.fetch_add(1, std::memory_order_relaxed);
            }
            scene_last_camera_hash.store(camera, std::memory_order_relaxed);
            scene_last_projection_hash.store(projection, std::memory_order_relaxed);

            // Track the unjittered main-view camera for the motion pass.
            // Filter out canopy planar reflections and auxiliary sub-cameras,
            // latching only authoritative primary scene cameras once per frame.
            edpe::CameraProjection parsed_camera{};
            const auto cur_frame = last_present_frame.load(std::memory_order_relaxed);
            const bool in_main_scene = s_inside_main_scene.load(std::memory_order_acquire);
            bool is_reflection = false;
            bool is_main_camera = false;
            bool aspect_ok = false;
            if (edpe::parseEliteCamera(static_cast<const float*>(mapped_ptr), 5376 / sizeof(float), &parsed_camera)) {
                const UINT outW = s_swapchain_width.load(std::memory_order_relaxed);
                const UINT outH = s_swapchain_height.load(std::memory_order_relaxed);
                aspect_ok = isSceneCameraAspect(parsed_camera.scaleX, parsed_camera.scaleY, outW, outH);
                is_reflection = isReflectionOrAuxiliaryCamera(parsed_camera, s_camera_now);
                if (aspect_ok) {
                    if (!is_reflection) {
                        is_main_camera = true;
                        s_auxiliary_rejection_streak = 0;
                        if (in_main_scene) {
                            if (s_in_scene_camera_latched_frame != cur_frame) {
                                updateMainCamera(parsed_camera, cur_frame);
                                s_in_scene_camera_latched_frame = cur_frame;
                            }
                        } else if (s_camera_frame != cur_frame) {
                            s_latest_unmapped_camera = parsed_camera;
                            s_latest_unmapped_camera_frame = cur_frame;
                            s_latest_unmapped_camera_valid = true;
                        }
                    } else if (cameraRotationDeterminant(parsed_camera) >= 0.5f && in_main_scene) {
                        s_fallback_unmapped_camera = parsed_camera;
                        s_fallback_unmapped_camera_frame = cur_frame;
                    }
                }
            }

            // Phase 5: Projection jitter injection into words 1080..1095
            // Inject jitter into all non-reflection scene/cockpit camera projections matching the display aspect ratio
            const bool is_planar_reflection = aspect_ok && (cameraRotationDeterminant(parsed_camera) < 0.5f);
            if ((is_main_camera || (aspect_ok && !is_planar_reflection)) && ContextCensusIsJitterActive()) {
                float* words = static_cast<float*>(mapped_ptr);
                bool valid_projection = true;
                for (size_t i = 1080; i < 1096; ++i) {
                    if (!std::isfinite(words[i])) {
                        valid_projection = false;
                        break;
                    }
                }
                if (valid_projection) {
                    const float depthB = words[1094];
                    if (depthB <= 0.0f || depthB > 1.0f ||
                        std::fabs(words[1092]) > 0.002f ||
                        std::fabs(words[1093]) > 0.002f ||
                        std::fabs(words[1095]) > 0.002f) {
                        valid_projection = false;
                    }
                }
                if (valid_projection) {
                    edpe::ProjectionJitter jitter{};
                    {
                        std::lock_guard lock(jitter_mutex);
                        jitter = active_projection_jitter;
                    }
                    float jittered_block[16]{};
                    if (edpe::jitterEliteProjectionBlock(&words[1080], jittered_block, jitter)) {
                        std::memcpy(&words[1080], jittered_block, sizeof(jittered_block));
                        s_injected_jitter = jitter;
                        s_last_rendered_jitter = jitter;
                        s_jitter_injected_frame = cur_frame;
                        const unsigned count = jittered_writes_count.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (count == 1) {
                            edpe::NdcOffset verified_offset{};
                            edpe::estimateEliteBlockNdcOffset(&words[1080], &verified_offset);
                            wchar_t msg[256];
                            swprintf_s(msg,
                                L"EDPE: projection jitter applied #%u frame=%llu pixel=(%.4f, %.4f) ndc=(%.6g, %.6g) verified_ndc=(%.6g, %.6g)",
                                count, jitter_frame_index.load(std::memory_order_relaxed),
                                jitter.pixelX, jitter.pixelY, jitter.ndcX, jitter.ndcY,
                                verified_offset.x, verified_offset.y);
                            EdpeLog(msg);
                        }
                    } else {
                        jitter_rejection_count.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    jitter_rejection_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    original_unmap.load(std::memory_order_acquire)(context, resource, subresource);
}

void endDrawProbe() {
    if (!draw_probe_active.exchange(false, std::memory_order_acq_rel)) return;
    auto* forward = reinterpret_cast<void*>(original_draw_indexed.load(std::memory_order_acquire));
    const bool restored = patched_table && patchSlot(patched_table, kDrawIndexed,
        reinterpret_cast<void*>(&observedDrawIndexed), forward);
    wchar_t message[256];
    swprintf_s(message,
        L"EDPE: depth-pass DrawIndexed calls=%u with5376VS1=%u slotRestored=%u bind=%u",
        draw_indexed_calls.load(std::memory_order_relaxed),
        draw_indexed_scene_calls.load(std::memory_order_relaxed), restored,
        vs_probe_bind_ordinal.load(std::memory_order_relaxed));
    EdpeLog(message);
    if (!restored && patched_table) {
        swprintf_s(message, L"EDPE: DrawIndexed slot changed current=%p forward=%p",
            patched_table[kDrawIndexed], forward);
        EdpeLog(message);
    }
    const bool draw_restored = !draw_hook_active.exchange(false) || patchSlot(
        patched_table, kDraw, reinterpret_cast<void*>(&observedDraw),
        reinterpret_cast<void*>(original_draw.load(std::memory_order_acquire)));
    const bool indexed_instanced_restored =
        !draw_indexed_instanced_hook_active.exchange(false) || patchSlot(
            patched_table, kDrawIndexedInstanced,
            reinterpret_cast<void*>(&observedDrawIndexedInstanced),
            reinterpret_cast<void*>(original_draw_indexed_instanced.load(std::memory_order_acquire)));
    const bool instanced_restored = !draw_instanced_hook_active.exchange(false) || patchSlot(
        patched_table, kDrawInstanced, reinterpret_cast<void*>(&observedDrawInstanced),
        reinterpret_cast<void*>(original_draw_instanced.load(std::memory_order_acquire)));
    swprintf_s(message,
        L"EDPE: scene draws Draw=%u/%u DrawIndexedInstanced=%u/%u DrawInstanced=%u/%u restored=%u%u%u bind=%u",
        draw_calls.load(std::memory_order_relaxed),
        draw_scene_calls.load(std::memory_order_relaxed),
        draw_indexed_instanced_calls.load(std::memory_order_relaxed),
        draw_indexed_instanced_scene_calls.load(std::memory_order_relaxed),
        draw_instanced_calls.load(std::memory_order_relaxed),
        draw_instanced_scene_calls.load(std::memory_order_relaxed),
        draw_restored, indexed_instanced_restored, instanced_restored,
        vs_probe_bind_ordinal.load(std::memory_order_relaxed));
    EdpeLog(message);
    swprintf_s(message, L"EDPE: first scene draw VS1=%p observed=%u bind=%u",
        first_scene_draw_buffer.load(std::memory_order_relaxed),
        first_scene_draw_seen.load(std::memory_order_relaxed),
        vs_probe_bind_ordinal.load(std::memory_order_relaxed));
    EdpeLog(message);
    dumpFirstSceneShader();
    dumpSceneShaders();
    auto* map_target = scene_map_target.exchange(nullptr, std::memory_order_acq_rel);
    scene_mapped_data.store(nullptr, std::memory_order_release);
    const bool map_restored = map_hook_active.load(std::memory_order_acquire);
    const bool unmap_restored = unmap_hook_active.load(std::memory_order_acquire);
    swprintf_s(message, L"EDPE: scene buffer maps=%u afterDraw=%u unmaps=%u restored=%u%u bind=%u beforeBind=%u target=%p firstVS=%p",
        scene_maps.load(std::memory_order_relaxed),
        scene_maps_after_draw.load(std::memory_order_relaxed),
        scene_unmaps.load(std::memory_order_relaxed), map_restored, unmap_restored,
        vs_probe_bind_ordinal.load(std::memory_order_relaxed),
        scene_maps_before_bind.load(std::memory_order_relaxed), map_target,
        vs_first_scene_buffer.load(std::memory_order_relaxed));
    EdpeLog(message);
    swprintf_s(message,
        L"EDPE: scene write hashes sampled=%u cameraChanges=%u projectionChanges=%u camera=%016llX/%016llX projection=%016llX/%016llX bind=%u",
        scene_hashed_writes.load(std::memory_order_relaxed),
        scene_camera_changes.load(std::memory_order_relaxed),
        scene_projection_changes.load(std::memory_order_relaxed),
        scene_first_camera_hash.load(std::memory_order_relaxed),
        scene_last_camera_hash.load(std::memory_order_relaxed),
        scene_first_projection_hash.load(std::memory_order_relaxed),
        scene_last_projection_hash.load(std::memory_order_relaxed),
        vs_probe_bind_ordinal.load(std::memory_order_relaxed));
    EdpeLog(message);
}

void beginDrawProbe() {
    if (!patched_table || !vs_buffer_probe_active.load(std::memory_order_acquire)) return;
    auto forward = reinterpret_cast<DrawIndexedFn>(patched_table[kDrawIndexed]);
    if (!forward || forward == &observedDrawIndexed) return;
    original_draw_indexed.store(forward, std::memory_order_release);
    draw_indexed_calls.store(0, std::memory_order_relaxed);
    draw_indexed_scene_calls.store(0, std::memory_order_relaxed);
    draw_calls.store(0, std::memory_order_relaxed);
    draw_scene_calls.store(0, std::memory_order_relaxed);
    draw_indexed_instanced_calls.store(0, std::memory_order_relaxed);
    draw_indexed_instanced_scene_calls.store(0, std::memory_order_relaxed);
    draw_instanced_calls.store(0, std::memory_order_relaxed);
    draw_instanced_scene_calls.store(0, std::memory_order_relaxed);
    first_scene_draw_seen.store(false, std::memory_order_relaxed);
    first_scene_draw_buffer.store(nullptr, std::memory_order_relaxed);
    first_scene_shader_bytes.store(0, std::memory_order_relaxed);
    scene_shader_probe_active.store(false, std::memory_order_release);
    {
        std::lock_guard lock(scene_shader_mutex);
        for (auto& sample : scene_shaders) sample.shader.Reset();
        scene_shader_count = 0;
        scene_shader_overflow_draws = 0;
    }
    scene_maps.store(0, std::memory_order_relaxed);
    scene_unmaps.store(0, std::memory_order_relaxed);
    scene_maps_after_draw.store(0, std::memory_order_relaxed);
    scene_maps_before_bind.store(0, std::memory_order_relaxed);
    scene_mapped_data.store(nullptr, std::memory_order_relaxed);
    scene_hashed_writes.store(0, std::memory_order_relaxed);
    scene_camera_changes.store(0, std::memory_order_relaxed);
    scene_projection_changes.store(0, std::memory_order_relaxed);
    scene_first_camera_hash.store(0, std::memory_order_relaxed);
    scene_last_camera_hash.store(0, std::memory_order_relaxed);
    scene_first_projection_hash.store(0, std::memory_order_relaxed);
    scene_last_projection_hash.store(0, std::memory_order_relaxed);
    if (patchSlot(patched_table, kDrawIndexed, reinterpret_cast<void*>(forward),
            reinterpret_cast<void*>(&observedDrawIndexed))) {
        draw_probe_active.store(true, std::memory_order_release);
        auto draw_forward = reinterpret_cast<DrawFn>(patched_table[kDraw]);
        auto indexed_instanced_forward = reinterpret_cast<DrawIndexedInstancedFn>(
            patched_table[kDrawIndexedInstanced]);
        auto instanced_forward = reinterpret_cast<DrawInstancedFn>(patched_table[kDrawInstanced]);
        original_draw.store(draw_forward, std::memory_order_release);
        original_draw_indexed_instanced.store(indexed_instanced_forward,
            std::memory_order_release);
        original_draw_instanced.store(instanced_forward, std::memory_order_release);
        if (draw_forward && patchSlot(patched_table, kDraw,
                reinterpret_cast<void*>(draw_forward), reinterpret_cast<void*>(&observedDraw)))
            draw_hook_active.store(true, std::memory_order_release);
        if (indexed_instanced_forward && patchSlot(patched_table, kDrawIndexedInstanced,
                reinterpret_cast<void*>(indexed_instanced_forward),
                reinterpret_cast<void*>(&observedDrawIndexedInstanced)))
            draw_indexed_instanced_hook_active.store(true, std::memory_order_release);
        if (instanced_forward && patchSlot(patched_table, kDrawInstanced,
                reinterpret_cast<void*>(instanced_forward),
                reinterpret_cast<void*>(&observedDrawInstanced)))
            draw_instanced_hook_active.store(true, std::memory_order_release);
        if (!map_hook_active.load(std::memory_order_acquire)) {
            auto map_forward = reinterpret_cast<MapFn>(patched_table[kMap]);
            if (map_forward) original_map.store(map_forward, std::memory_order_release);
            if (map_forward && patchSlot(patched_table, kMap,
                    reinterpret_cast<void*>(map_forward), reinterpret_cast<void*>(&observedMap)))
                map_hook_active.store(true, std::memory_order_release);
        }
        if (!unmap_hook_active.load(std::memory_order_acquire)) {
            auto unmap_forward = reinterpret_cast<UnmapFn>(patched_table[kUnmap]);
            if (unmap_forward) original_unmap.store(unmap_forward, std::memory_order_release);
            if (unmap_forward && patchSlot(patched_table, kUnmap,
                    reinterpret_cast<void*>(unmap_forward), reinterpret_cast<void*>(&observedUnmap)))
                unmap_hook_active.store(true, std::memory_order_release);
        }
    } else
        EdpeLog(L"EDPE: depth-pass DrawIndexed probe unavailable (slot changed)");
}

void endVSBufferProbe() {
    endDrawProbe();
    if (!vs_buffer_probe_active.exchange(false, std::memory_order_acq_rel)) return;
    const bool restored = patched_table && patchSlot(patched_table,
        kVSSetConstantBuffers, reinterpret_cast<void*>(&observedVSSetConstantBuffers),
        reinterpret_cast<void*>(original_vs_set_buffers.load(std::memory_order_acquire)));
    wchar_t message[256];
    swprintf_s(message,
        L"EDPE: depth-pass VS bindings calls=%u sceneBufferCalls=%u first=%p last=%p switches=%u slotRestored=%u bind=%u",
        vs_buffer_calls.load(std::memory_order_relaxed),
        vs_scene_buffer_calls.load(std::memory_order_relaxed),
        vs_first_scene_buffer.load(std::memory_order_relaxed),
        vs_scene_buffer.load(std::memory_order_relaxed),
        vs_scene_buffer_switches.load(std::memory_order_relaxed), restored,
        vs_probe_bind_ordinal.load(std::memory_order_relaxed));
    EdpeLog(message);
}

void beginVSBufferProbe(ID3D11DeviceContext* context, unsigned bind_ordinal) {
    if (!patched_table) return;
    vs_probe_bind_ordinal.store(bind_ordinal, std::memory_order_relaxed);
    auto forward = reinterpret_cast<VSSetConstantBuffersFn>(
        patched_table[kVSSetConstantBuffers]);
    if (!forward || forward == &observedVSSetConstantBuffers) {
        EdpeLog(L"EDPE: depth-pass VS bindings unavailable (invalid forward)");
        return;
    }
    original_vs_set_buffers.store(forward, std::memory_order_release);
    vs_buffer_calls.store(0, std::memory_order_relaxed);
    vs_scene_buffer_calls.store(0, std::memory_order_relaxed);
    vs_scene_buffer_switches.store(0, std::memory_order_relaxed);
    vs_first_scene_buffer.store(nullptr, std::memory_order_relaxed);
    vs_scene_buffer.store(nullptr, std::memory_order_relaxed);
    ID3D11Buffer* initial = nullptr;
    context->VSGetConstantBuffers(1, 1, &initial);
    D3D11_BUFFER_DESC initial_desc{};
    if (initial) {
        initial->GetDesc(&initial_desc);
        initial->Release();
    }
    vs_scene_buffer_bound.store(initial_desc.ByteWidth == 5376 &&
        initial_desc.Usage == D3D11_USAGE_DYNAMIC, std::memory_order_relaxed);
    scene_map_target.store(vs_scene_buffer_bound.load(std::memory_order_relaxed)
        ? initial : nullptr, std::memory_order_release);
    vs_buffer_probe_active.store(true, std::memory_order_release);
    if (!patchSlot(patched_table, kVSSetConstantBuffers,
            reinterpret_cast<void*>(forward),
            reinterpret_cast<void*>(&observedVSSetConstantBuffers))) {
        vs_buffer_probe_active.store(false, std::memory_order_release);
        EdpeLog(L"EDPE: depth-pass VS bindings unavailable (slot changed)");
    } else beginDrawProbe();
}

void logDepthState(ID3D11DeviceContext* context, const PipelineSample& sample,
    const wchar_t* edge) {
    ID3D11DepthStencilState* state = nullptr;
    UINT stencil_ref = 0;
    context->OMGetDepthStencilState(&state, &stencil_ref);
    wchar_t message[192];
    if (state) {
        D3D11_DEPTH_STENCIL_DESC desc{};
        state->GetDesc(&desc);
        swprintf_s(message,
            L"EDPE: scene depth state afterPresent=%llu bind=%u edge=%s enable=%u write=%u func=%u",
            sample.after_present, sample.bind, edge, desc.DepthEnable,
            static_cast<unsigned>(desc.DepthWriteMask), static_cast<unsigned>(desc.DepthFunc));
        state->Release();
    } else {
        swprintf_s(message, L"EDPE: scene depth state afterPresent=%llu bind=%u edge=%s default",
            sample.after_present, sample.bind, edge);
    }
    EdpeLog(message);
}

void endPipelineSample(ID3D11DeviceContext* context) {
    std::lock_guard lock(seen_mutex);
    for (auto& sample : pipeline_samples) {
        if (!sample.active) continue;
        logDepthState(context, sample, L"end");
        context->End(sample.query);
        sample.active = false;
    }
}

void beginPipelineSample(ID3D11DeviceContext* context, unsigned bind) {
    if (bind < 1 || (bind > 3 && bind != 6)) return;
    std::lock_guard lock(seen_mutex);
    auto& sample = pipeline_samples[bind == 6 ? 3 : bind - 1];
    if (sample.query) return;
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    D3D11_QUERY_DESC desc{D3D11_QUERY_PIPELINE_STATISTICS, 0};
    const HRESULT result = device ? device->CreateQuery(&desc, &sample.query) : E_FAIL;
    if (device) device->Release();
    if (FAILED(result) || !sample.query) {
        if (sample.query) sample.query->Release();
        sample.query = nullptr;
        EdpeLog(L"EDPE: scene pipeline statistics unavailable (query creation failed)");
        return;
    }
    sample.bind = bind;
    sample.after_present = last_present_frame.load(std::memory_order_relaxed);
    sample.wait = 0;
    sample.active = true;
    context->Begin(sample.query);
    logDepthState(context, sample, L"start");
}

void pollPipelineSamples(ID3D11DeviceContext* context) {
    std::lock_guard lock(seen_mutex);
    for (auto& sample : pipeline_samples) {
        if (!sample.query || sample.active) continue;
        D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
        const HRESULT result = context->GetData(sample.query, &stats, sizeof(stats),
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (result == S_FALSE && ++sample.wait < 120) continue;
        if (result == S_OK) {
            wchar_t message[208];
            swprintf_s(message,
                L"EDPE: scene pipeline afterPresent=%llu bind=%u iaPrimitives=%llu vsInvocations=%llu psInvocations=%llu",
                sample.after_present, sample.bind, stats.IAPrimitives,
                stats.VSInvocations, stats.PSInvocations);
            EdpeLog(message);
        } else {
            EdpeLog(L"EDPE: scene pipeline statistics unavailable (query failed or timed out)");
        }
        sample.query->Release();
        sample = {};
    }
}

void recordBind(int index) {
    if (index == sequence_last) return;
    sequence_last = index;
    ++sequence_transitions;
    if (sequence_stored < bind_sequence.size()) bind_sequence[sequence_stored++] = index;
}

void queueConstantBufferSample(ID3D11DeviceContext* context, ID3D11Buffer* source,
    const D3D11_BUFFER_DESC& source_desc, unsigned bind_ordinal) {
    const unsigned slot = bind_ordinal == kPairFirstSample || bind_ordinal == kPairSecondSample
        ? 6 + bind_ordinal - kPairFirstSample
        : bind_ordinal == kAlternateProjectionSample ? 8
        : bind_ordinal == 51 ? 5 : bind_ordinal == 0 ? 4
        : bind_ordinal == 6 ? 3 : bind_ordinal - 2;
    auto& sample = constant_buffer_samples[slot];
    bool expected = false;
    if (!sample.claimed.compare_exchange_strong(expected, true)) return;
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    D3D11_BUFFER_DESC desc = source_desc;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Buffer* staging = nullptr;
    const HRESULT result = device ? device->CreateBuffer(&desc, nullptr, &staging) : E_FAIL;
    if (FAILED(result) || !staging) {
        if (device) device->Release();
        sample.claimed.store(false, std::memory_order_release);
        EdpeLog(L"EDPE: scene CB sample unavailable (staging creation failed)");
        return;
    }
    const bool pair = bind_ordinal == kPairFirstSample || bind_ordinal == kPairSecondSample;
    Microsoft::WRL::ComPtr<ID3D11Buffer> gpu_copy;
    if (pair) {
        desc = source_desc;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        if (SUCCEEDED(device->CreateBuffer(&desc, nullptr, &gpu_copy))) {
            context->CopyResource(gpu_copy.Get(), source);
            gpu_camera_pair[bind_ordinal - kPairFirstSample] = gpu_copy;
            wchar_t message[112];
            swprintf_s(message, L"EDPE: camera GPU copy queued bind=%u bytes=%u",
                bind_ordinal, desc.ByteWidth);
            EdpeLog(message);
        } else {
            EdpeLog(L"EDPE: camera GPU copy unavailable; staging source directly");
        }
    }
    context->CopyResource(staging, gpu_copy ? gpu_copy.Get() : source);
    if (device) device->Release();
    sample.buffer = staging;
    sample.bind = bind_ordinal;
    sample.after_present = last_present_frame.load(std::memory_order_relaxed);
    sample.wait = 0;
    sample.ready.store(true, std::memory_order_release);
}

void pollConstantBufferSamples(ID3D11DeviceContext* context) {
    for (auto& sample : constant_buffer_samples) {
        if (!sample.ready.load(std::memory_order_acquire)) continue;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = context->Map(sample.buffer, 0, D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (SUCCEEDED(result) && mapped.pData) {
            const auto* values = static_cast<const float*>(mapped.pData);
            if (sample.bind == 0 || sample.bind == 51 || sample.bind == 3) {
                unsigned long long hash = 14695981039346656037ull;
                unsigned long long camera_hash = 14695981039346656037ull;
                const auto* bytes = static_cast<const unsigned char*>(mapped.pData);
                for (size_t i = 0; i < 5376; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
                for (size_t i = 932 * sizeof(float); i < 944 * sizeof(float); ++i)
                    camera_hash = (camera_hash ^ bytes[i]) * 1099511628211ull;
                for (size_t i = 1080 * sizeof(float); i < 1096 * sizeof(float); ++i)
                    camera_hash = (camera_hash ^ bytes[i]) * 1099511628211ull;
                wchar_t message[128];
                swprintf_s(message, L"EDPE: scene CB hash bind=%u fnv64=%016llX camera64=%016llX",
                    sample.bind, hash, camera_hash);
                EdpeLog(message);
            }
            if (sample.bind == kAlternateProjectionSample) {
                D3D11_BUFFER_DESC desc{};
                sample.buffer->GetDesc(&desc);
                wchar_t message[192];
                swprintf_s(message, L"EDPE: alternate VS CB0 sample afterPresent=%llu bytes=%u",
                    sample.after_present, desc.ByteWidth);
                EdpeLog(message);
                for (unsigned row = 4; row < 8; ++row) {
                    const unsigned offset = row * 4;
                    swprintf_s(message, L"EDPE: alternate VS CB0 row%u=(%.9g,%.9g,%.9g,%.9g)",
                        row, values[offset], values[offset + 1],
                        values[offset + 2], values[offset + 3]);
                    EdpeLog(message);
                }
            } else if (sample.bind == 2) {
                wchar_t words[192];
                int used = swprintf_s(words, L"EDPE: depth-pass CB bind=2 slot=2 hex=");
                const auto* raw = static_cast<const unsigned*>(mapped.pData);
                for (size_t i = 0; i < 12; ++i)
                    used += swprintf_s(words + used, 192 - used, L"%08X", raw[i]);
                EdpeLog(words);
            } else {
                wchar_t message[320];
                swprintf_s(message,
                    L"EDPE: scene CB sample bind=%u projectionZ=(%.9g,%.9g) projection2D=%.9g rows0=(%.6g,%.6g,%.6g,%.6g)",
                    sample.bind, values[794], values[795], values[1094],
                    values[932], values[933], values[934], values[935]);
                EdpeLog(message);
                swprintf_s(message, L"EDPE: scene CB rows1 bind=%u (%.6g,%.6g,%.6g,%.6g) rows2=(%.6g,%.6g,%.6g,%.6g)",
                    sample.bind, values[936], values[937], values[938], values[939],
                    values[940], values[941], values[942], values[943]);
                EdpeLog(message);
                swprintf_s(message,
                    L"EDPE: scene CB 2D xy bind=%u x=(%.9g,%.9g,%.9g,%.9g) y=(%.9g,%.9g,%.9g,%.9g)",
                    sample.bind, values[1080], values[1081], values[1082], values[1083],
                    values[1084], values[1085], values[1086], values[1087]);
                EdpeLog(message);
                swprintf_s(message,
                    L"EDPE: scene CB 2D zw bind=%u z=(%.9g,%.9g,%.9g,%.9g) w=(%.9g,%.9g,%.9g,%.9g)",
                    sample.bind, values[1088], values[1089], values[1090], values[1091],
                    values[1092], values[1093], values[1094], values[1095]);
                EdpeLog(message);
                edpe::CameraProjection camera{};
                const bool parsed = edpe::parseEliteCamera(values, 5376 / sizeof(float), &camera);
                if (motion_pair_active && parsed &&
                    (sample.bind == kPairFirstSample || sample.bind == kPairSecondSample)) {
                    const unsigned slot = sample.bind - kPairFirstSample;
                    motion_cameras[slot] = camera;
                    motion_camera_frames[slot] = sample.after_present;
                }
                edpe::NdcOffset native{};
                const bool have_native = edpe::estimateEliteBlockNdcOffset(values + 1080, &native);
                swprintf_s(message,
                    L"EDPE: scene camera candidate afterPresent=%llu bind=%u valid=%u scale=(%.9g,%.9g) depthB=%.9g nativeNdc=(%.3g,%.3g) nativeValid=%u",
                    sample.after_present, sample.bind, parsed, camera.scaleX, camera.scaleY, camera.depthB,
                    native.x, native.y, have_native);
                EdpeLog(message);
                if (sample.bind == 6) for (size_t offset = 0; offset < 5376 / sizeof(float); offset += 32) {
                    wchar_t words[320];
                    int used = swprintf_s(words, L"EDPE: scene CB hex %04zu ", offset);
                    for (size_t i = 0; i < 32; ++i)
                        used += swprintf_s(words + used, 320 - used, L"%08X",
                            std::bit_cast<unsigned>(values[offset + i]));
                    EdpeLog(words);
                }
            }
            context->Unmap(sample.buffer, 0);
        } else if (result == DXGI_ERROR_WAS_STILL_DRAWING && ++sample.wait < 120) {
            continue;
        } else {
            EdpeLog(L"EDPE: scene CB sample unavailable (nonblocking map failed or timed out)");
        }
        sample.buffer->Release();
        sample.buffer = nullptr;
        sample.ready.store(false, std::memory_order_release);
        sample.claimed.store(false, std::memory_order_release);
    }
}

void logCameraDelta(unsigned long long frame, bool parsed, const edpe::CameraProjection& camera) {
    const auto r = camera_delta_tracker.feed(frame, parsed, camera);
    wchar_t message[512]; // swprintf_s aborts on overflow; worst case is about 260 chars.
    if (!camera_delta_announced) {
        camera_delta_announced = true;
        swprintf_s(message, L"EDPE: camera delta log first sample frame=%llu valid=%u window=%u",
            frame, parsed, edpe::CameraDeltaTracker::kWindow);
        EdpeLog(message);
    }
    SYSTEMTIME now{};
    if (r.logEvent || r.summary) GetLocalTime(&now);
    if (r.logEvent) {
        const float* m = camera.worldFromView;
        swprintf_s(message,
            L"EDPE: camera delta event time=%02u:%02u:%02u.%03u frame=%llu flags=0x%X valid=%u gap=%llu move=%.6g turnDeg=%.6g projection=%.3g depthB=%.3g pos=(%.7g,%.7g,%.7g) scale=(%.9g,%.9g)",
            now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
            frame, r.flags, parsed, r.gap, r.delta.translation, r.delta.rotationDeg,
            r.delta.projection, r.delta.depthB, m[3], m[7], m[11], camera.scaleX, camera.scaleY);
        EdpeLog(message);
    }
    if (!r.summary) return;
    const auto& w = camera_delta_tracker.window();
    const double deltas = w.deltas ? w.deltas : 1;
    swprintf_s(message,
        L"EDPE: camera delta summary time=%02u:%02u:%02u frames=%llu-%llu samples=%u invalid=%u gaps=%u dropped=%u maxMove=%.6g meanMove=%.6g maxTurnDeg=%.6g meanTurnDeg=%.6g events=%u suppressed=%u",
        now.wHour, now.wMinute, now.wSecond,
        w.firstFrame, w.lastFrame, w.samples, w.invalid, w.gaps, camera_delta_dropped,
        w.maxTranslation, w.sumTranslation / deltas, w.maxRotationDeg, w.sumRotationDeg / deltas,
        w.events, w.suppressed);
    EdpeLog(message);
    camera_delta_tracker.nextWindow();
    camera_delta_dropped = 0;
}

// Called at the first scene-MRT bind of a frame: retain the VS slot-1 buffer.
void takeCameraDeltaSource(ID3D11DeviceContext* context, unsigned long long frame) {
    if (edpe::UpscalerManager::instance().isReconstructionActive()) return;
    if (camera_delta_disabled || camera_delta_source || camera_delta_taken_frame == frame) return;
    camera_delta_taken_frame = frame;
    ID3D11Buffer* buffer = nullptr;
    context->VSGetConstantBuffers(1, 1, &buffer);
    if (!buffer) return;
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    if (desc.ByteWidth != 5376 || desc.Usage != D3D11_USAGE_DYNAMIC) {
        buffer->Release();
        return;
    }
    if (!scene_map_target.load(std::memory_order_acquire)) {
        scene_map_target.store(buffer, std::memory_order_release);
    }
    camera_delta_source.Attach(buffer);
    camera_delta_source_frame = frame;
}

// Called when the scene-MRT interval ends: queue a 5376-byte GPU copy.
void copyCameraDeltaSource(ID3D11DeviceContext* context) {
    edpe::ScopedProfileTimer profTimer(edpe::ProfileZone::CameraDeltaCopy);
    if (!camera_delta_source) return;
    const auto source = std::move(camera_delta_source);
    if (camera_delta_count == kCameraDeltaRing) {
        ++camera_delta_dropped;
        return;
    }
    auto& slot = camera_delta_ring[(camera_delta_head + camera_delta_count) % kCameraDeltaRing];
    if (!slot.staging) { // Created once per slot, never per frame.
        D3D11_BUFFER_DESC desc{};
        source->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        desc.StructureByteStride = 0;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (!device || FAILED(device->CreateBuffer(&desc, nullptr, &slot.staging))) {
            camera_delta_disabled = true;
            EdpeLog(L"EDPE: camera delta log disabled (staging creation failed)");
            return;
        }
    }
    context->CopyResource(slot.staging.Get(), source.Get());
    slot.frame = camera_delta_source_frame;
    ++camera_delta_count;
}

// Called at Present: read finished copies in order without waiting.
void pollCameraDeltaRing(ID3D11DeviceContext* context) {
    while (camera_delta_count) {
        auto& slot = camera_delta_ring[camera_delta_head];
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = context->Map(slot.staging.Get(), 0, D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (result == DXGI_ERROR_WAS_STILL_DRAWING) return;
        if (SUCCEEDED(result) && mapped.pData) {
            edpe::CameraProjection camera{};
            const bool parsed = edpe::parseEliteCamera(static_cast<const float*>(mapped.pData),
                5376 / sizeof(float), &camera);
            context->Unmap(slot.staging.Get(), 0);
            logCameraDelta(slot.frame, parsed, camera);
        } else {
            ++camera_delta_dropped;
        }
        camera_delta_head = (camera_delta_head + 1) % kCameraDeltaRing;
        --camera_delta_count;
    }
}

void resetCameraDelta() {
    camera_delta_source.Reset();
    for (auto& slot : camera_delta_ring) slot = {};
    camera_delta_head = camera_delta_count = 0;
    camera_delta_taken_frame = ~0ull;
    camera_delta_dropped = 0;
    camera_delta_disabled = false;
    camera_delta_announced = false;
    camera_delta_tracker.reset();
}

void logBoundConstantBuffers(ID3D11DeviceContext* context, size_t depth_index,
    unsigned bind_ordinal) {
    constexpr UINT slots = 8;
    ID3D11Buffer* vs[slots]{};
    ID3D11Buffer* ps[slots]{};
    context->VSGetConstantBuffers(0, slots, vs);
    context->PSGetConstantBuffers(0, slots, ps);
    for (UINT stage = 0; stage < 2; ++stage) {
        auto* buffers = stage ? ps : vs;
        for (UINT slot = 0; slot < slots; ++slot) {
            if (!buffers[slot]) continue;
            D3D11_BUFFER_DESC desc{};
            buffers[slot]->GetDesc(&desc);
            wchar_t message[160];
            swprintf_s(message,
                L"EDPE: DSV #%zu bind=%u CB stage=%s slot=%u buffer=%p bytes=%u usage=%u cpu=0x%X",
                depth_index, bind_ordinal, stage ? L"PS" : L"VS", slot, buffers[slot],
                desc.ByteWidth, static_cast<unsigned>(desc.Usage), desc.CPUAccessFlags);
            EdpeLog(message);
            if (stage == 0 && desc.Usage == D3D11_USAGE_DYNAMIC &&
                ((bind_ordinal == 2 && slot == 2 && desc.ByteWidth == 48) ||
                 ((bind_ordinal == 3 || bind_ordinal == 4 || bind_ordinal == 6) &&
                  slot == 1 && desc.ByteWidth == 5376)))
                queueConstantBufferSample(context, buffers[slot], desc, bind_ordinal);
            buffers[slot]->Release();
        }
    }
}

void queueCameraPairSample(ID3D11DeviceContext* context, unsigned sample_label) {
    ID3D11Buffer* buffer = nullptr;
    context->VSGetConstantBuffers(1, 1, &buffer);
    if (!buffer) {
        EdpeLog(L"EDPE: adjacent camera sample unavailable (VS slot 1 empty)");
        return;
    }
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    if (desc.ByteWidth == 5376 && desc.Usage == D3D11_USAGE_DYNAMIC)
        queueConstantBufferSample(context, buffer, desc, sample_label);
    else
        EdpeLog(L"EDPE: adjacent camera sample unavailable (unexpected buffer)");
    buffer->Release();
}

void queueMotionDepth(ID3D11DeviceContext* context, ID3D11Texture2D* source,
    const D3D11_TEXTURE2D_DESC& source_desc, unsigned long long after_present) {
    if (!motion_pair_active || after_present != camera_pair_armed_after + 1) return;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc = source_desc;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = desc.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
    view_desc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &copy)) ||
        FAILED(device->CreateShaderResourceView(copy.Get(), &view_desc, &view))) {
        EdpeLog(L"EDPE: motion candidate unavailable (GPU depth copy creation failed)");
        motion_pair_active = false;
        return;
    }
    context->CopyResource(copy.Get(), source);
    motion_depth = copy;
    motion_depth_view = view;
    motion_depth_frame = after_present;
    EdpeLog(L"EDPE: motion candidate depth retained on GPU");
}

void queueMotionColor(ID3D11DeviceContext* context, unsigned long long frame) {
    if (!motion_pair_active || !motion_color_source ||
        motion_color_frame + 1 != frame || motion_depth_frame != motion_color_frame)
        return;
    wchar_t transition[160];
    swprintf_s(transition,
        L"EDPE: motion HDR RTV3 frame=%llu firstExit=%u rebound=%u lastObservedBound=%u",
        motion_color_frame, motion_color_first_exit_logged, motion_color_rebound,
        motion_color_bound);
    EdpeLog(transition);
    auto source_view = std::move(motion_color_source);
    ID3D11RenderTargetView* bound[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, bound, nullptr);
    bool still_bound = false;
    for (auto* view : bound) {
        still_bound |= sameRenderTargetResource(view, source_view.Get());
        if (view) view->Release();
    }
    if (still_bound) {
        EdpeLog(L"EDPE: motion color candidate unavailable (RTV still bound)");
        return;
    }
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
    source_view->GetDesc(&rtv_desc);
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    source_view->GetResource(&resource);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
    if (!resource || FAILED(resource.As(&source))) {
        EdpeLog(L"EDPE: motion color candidate unavailable (not a Texture2D)");
        return;
    }
    D3D11_TEXTURE2D_DESC desc{}, depth_desc{};
    source->GetDesc(&desc);
    motion_depth->GetDesc(&depth_desc);
    if (desc.Format != DXGI_FORMAT_R11G11B10_FLOAT ||
        rtv_desc.Format != desc.Format || desc.Width != depth_desc.Width ||
        desc.Height != depth_desc.Height || desc.MipLevels != 1 ||
        desc.ArraySize != 1 || desc.SampleDesc.Count != 1) {
        EdpeLog(L"EDPE: motion color candidate unavailable (layout mismatch)");
        return;
    }
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = desc.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &copy)) ||
        FAILED(device->CreateShaderResourceView(copy.Get(), nullptr, &motion_color_view))) {
        EdpeLog(L"EDPE: motion color candidate unavailable (GPU copy creation failed)");
        return;
    }
    context->CopyResource(copy.Get(), source.Get());
    wchar_t message[160];
    swprintf_s(message, L"EDPE: motion color retained on GPU afterPresent=%llu size=%ux%u format=%u",
        motion_color_frame, desc.Width, desc.Height, static_cast<unsigned>(desc.Format));
    EdpeLog(message);
}

void captureEarlyMotionColor(ID3D11DeviceContext* context, bool from_hud) {
    if ((motion_early_color_view && !from_hud) || !motion_color_source) return;
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
    motion_color_source->GetDesc(&rtv_desc);
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    motion_color_source->GetResource(&resource);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
    if (!resource || FAILED(resource.As(&source))) {
        EdpeLog(L"EDPE: early HDR copy unavailable (RTV has no Texture2D)");
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R11G11B10_FLOAT ||
        rtv_desc.Format != desc.Format || !desc.Width || !desc.Height ||
        desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
        static_cast<unsigned long long>(desc.Width) * desc.Height > 3840ull * 2160) {
        EdpeLog(L"EDPE: early HDR copy unavailable (unsupported texture layout)");
        return;
    }
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = desc.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    if (!device || FAILED(device->CreateTexture2D(&desc, nullptr, &copy)) ||
        FAILED(device->CreateShaderResourceView(copy.Get(), nullptr, &view)) ||
        FAILED(device->CreateRenderTargetView(copy.Get(), nullptr, &target))) {
        EdpeLog(L"EDPE: early HDR copy unavailable (resource creation failed)");
        return;
    }
    context->CopyResource(copy.Get(), source.Get());
    motion_early_color_view = std::move(view);
    motion_clean_color_target = std::move(target);
    motion_early_color_width = desc.Width;
    motion_early_color_height = desc.Height;
    motion_early_color_from_hud = from_hud;
    if (from_hud) EdpeLog(L"EDPE: clean HDR RTV ready for diagnostic world replay");
    EdpeLog(from_hud ? L"EDPE: HDR color copied before first matched HUD draw" :
                       L"EDPE: early HDR color copied at first RTV3 exit");
}

void queueCameraPairDepth(ID3D11DeviceContext* context, unsigned long long frame) {
    for (auto& sample : camera_pair_depth) {
        ID3D11DepthStencilView* view = nullptr;
        {
            std::lock_guard lock(seen_mutex);
            if (sample.view && sample.after_present + 1 == frame) {
                view = sample.view;
                sample.view = nullptr;
            }
        }
        if (!view) continue;
        ID3D11DepthStencilView* bound = nullptr;
        context->OMGetRenderTargets(0, nullptr, &bound);
        if (bound) {
            bound->Release();
            view->Release();
            EdpeLog(L"EDPE: adjacent depth sample skipped (DSV still bound at Present)");
            continue;
        }
        ID3D11Resource* resource = nullptr;
        view->GetResource(&resource);
        ID3D11Texture2D* source = nullptr;
        if (resource) {
            resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source));
            resource->Release();
        }
        view->Release();
        if (!source) {
            EdpeLog(L"EDPE: adjacent depth sample unavailable (not a Texture2D)");
            continue;
        }
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS || desc.MipLevels != 1 ||
            desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || !desc.Width || !desc.Height ||
            static_cast<unsigned long long>(desc.Width) * desc.Height > 3840ull * 2160) {
            source->Release();
            EdpeLog(L"EDPE: adjacent depth sample unavailable (unsupported layout)");
            continue;
        }
        ID3D11Device* device = nullptr;
        context->GetDevice(&device);
        queueMotionDepth(context, source, desc, sample.after_present);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        const HRESULT result = device ? device->CreateTexture2D(&desc, nullptr, &staging) : E_FAIL;
        if (device) device->Release();
        if (SUCCEEDED(result) && staging) {
            context->CopyResource(staging, source); // One-shot full depth copy; no normal-frame readback.
            std::lock_guard lock(seen_mutex);
            sample.staging = staging;
            sample.width = desc.Width;
            sample.height = desc.Height;
            sample.wait = 0;
        } else {
            EdpeLog(L"EDPE: adjacent depth sample unavailable (staging creation failed)");
        }
        source->Release();
    }
    queueMotionColor(context, frame);
}

void pollCameraPairDepth(ID3D11DeviceContext* context) {
    std::lock_guard lock(seen_mutex);
    for (auto& sample : camera_pair_depth) {
        if (!sample.staging) continue;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = context->Map(sample.staging, 0, D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (result == DXGI_ERROR_WAS_STILL_DRAWING && ++sample.wait < 120) continue;
        if (SUCCEEDED(result) && mapped.pData) {
            float depth = 0;
            const auto* pixel = static_cast<const unsigned char*>(mapped.pData) +
                static_cast<size_t>(sample.height / 2) * mapped.RowPitch +
                static_cast<size_t>(sample.width / 2) * 8;
            std::memcpy(&depth, pixel, sizeof(depth));
            context->Unmap(sample.staging, 0);
            wchar_t message[160];
            swprintf_s(message,
                L"EDPE: adjacent depth afterPresent=%llu centerValid=%u center=%.9g size=%ux%u",
                sample.after_present, std::isfinite(depth) && depth >= 0 && depth <= 1,
                depth, sample.width, sample.height);
            EdpeLog(message);
        } else {
            EdpeLog(L"EDPE: adjacent depth sample unavailable (nonblocking map failed)");
        }
        sample.staging->Release();
        sample.staging = nullptr;
    }
}

void pollMotionGrid(ID3D11DeviceContext* context) {
    if (!motion_grid_readback) return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT result = context->Map(motion_grid_readback.Get(), 0,
        D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (result == DXGI_ERROR_WAS_STILL_DRAWING && ++motion_grid_wait < 120) return;
    if (SUCCEEDED(result) && mapped.pData) {
        float min_x = std::numeric_limits<float>::infinity();
        float max_x = -min_x, min_y = min_x, max_y = -min_x;
        float center_x = 0, center_y = 0;
        unsigned finite = 0, over_one = 0, over_ten = 0;
        for (unsigned row = 0; row < 5; ++row) {
            const auto* halves = reinterpret_cast<const uint16_t*>(
                static_cast<const unsigned char*>(mapped.pData) + row * mapped.RowPitch);
            for (unsigned column = 0; column < 5; ++column) {
                const float x = DirectX::PackedVector::XMConvertHalfToFloat(halves[column * 2]);
                const float y = DirectX::PackedVector::XMConvertHalfToFloat(halves[column * 2 + 1]);
                if (row == 2 && column == 2) { center_x = x; center_y = y; }
                if (!std::isfinite(x) || !std::isfinite(y)) continue;
                ++finite;
                min_x = (std::min)(min_x, x); max_x = (std::max)(max_x, x);
                min_y = (std::min)(min_y, y); max_y = (std::max)(max_y, y);
                const float magnitude = (std::max)(std::fabs(x), std::fabs(y));
                over_one += magnitude > 1.0f;
                over_ten += magnitude > 10.0f;
            }
        }
        context->Unmap(motion_grid_readback.Get(), 0);
        wchar_t message[192];
        swprintf_s(message,
            L"EDPE: motion candidate currentAfterPresent=%llu center=(%.6g,%.6g) pixels finite=%u",
            motion_depth_frame, center_x, center_y,
            std::isfinite(center_x) && std::isfinite(center_y));
        EdpeLog(message);
        swprintf_s(message,
            L"EDPE: motion grid 5x5 finite=%u over1=%u over10=%u x=[%.6g,%.6g] y=[%.6g,%.6g] pixels",
            finite, over_one, over_ten, min_x, max_x, min_y, max_y);
        EdpeLog(message);
    } else {
        EdpeLog(L"EDPE: motion candidate grid unavailable (nonblocking readback failed)");
    }
    motion_grid_readback.Reset();
}

void tryMotionPair(ID3D11DeviceContext* context, unsigned long long frame) {
    if (!motion_pair_active) return;
    if (frame > camera_pair_armed_after + 120) {
        EdpeLog(L"EDPE: motion candidate unavailable (camera/depth pair timed out)");
        motion_pair_active = false;
        motion_depth.Reset();
        motion_depth_view.Reset();
        motion_color_source.Reset();
        motion_color_view.Reset();
        motion_early_color_view.Reset();
        motion_clean_color_target.Reset();
        return;
    }
    if (motion_camera_frames[0] != camera_pair_armed_after ||
        motion_camera_frames[1] != camera_pair_armed_after + 1 ||
        motion_depth_frame != camera_pair_armed_after + 1 ||
        !motion_depth_view) return;

    D3D11_TEXTURE2D_DESC desc{};
    motion_depth->GetDesc(&desc);
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!motion_pass) {
        auto candidate = std::make_unique<edpe::MotionPass>();
        if (!candidate->initialize(device.Get(), context)) {
            EdpeLog(L"EDPE: motion candidate unavailable (motion pass initialization failed)");
            motion_pair_active = false;
            motion_depth.Reset();
            motion_depth_view.Reset();
            motion_early_color_view.Reset();
            motion_clean_color_target.Reset();
            return;
        }
        motion_pass = std::move(candidate);
    }
    const bool gpu_cameras = gpu_camera_pair[0] && gpu_camera_pair[1];
    const bool gpu_rendered = gpu_cameras && motion_pass->renderGpuCameras(
        motion_depth_view.Get(), gpu_camera_pair[1].Get(), gpu_camera_pair[0].Get(),
        desc.Width, desc.Height);
    const bool rendered = gpu_rendered || motion_pass->render(motion_depth_view.Get(),
        motion_cameras[1], motion_cameras[0], desc.Width, desc.Height);
    EdpeLog(gpu_rendered ? L"EDPE: motion candidate used GPU camera buffers" :
        L"EDPE: motion candidate used CPU camera fallback");
    gpu_camera_pair = {};
    if (!rendered) {
        EdpeLog(L"EDPE: motion candidate unavailable (GPU motion draw rejected inputs)");
        motion_pair_active = false;
        motion_depth.Reset();
        motion_depth_view.Reset();
        motion_color_source.Reset();
        motion_color_view.Reset();
        motion_early_color_view.Reset();
        motion_clean_color_target.Reset();
        return;
    }
    EdpeLog(motion_color_view ? L"EDPE: motion candidate has same-frame HDR color" :
        L"EDPE: motion candidate HDR color unavailable");
    motion_color_snapshot = std::move(motion_color_view);
    motion_early_color_snapshot = std::move(motion_early_color_view);
    motion_depth_snapshot = std::move(motion_depth_view);
    motion_depth_snapshot_width = desc.Width;
    motion_depth_snapshot_height = desc.Height;
    motion_depth_snapshot_index = static_cast<unsigned>(camera_pair_target >= 0 ? camera_pair_target : 0);
    motion_clean_color_target.Reset();
    motion_color_snapshot_width = desc.Width;
    motion_color_snapshot_height = desc.Height;
    motion_snapshot = motion_pass->output();
    motion_snapshot_width = desc.Width;
    motion_snapshot_height = desc.Height;
    Microsoft::WRL::ComPtr<ID3D11Resource> output;
    motion_pass->output()->GetResource(&output);
    D3D11_TEXTURE2D_DESC read_desc{};
    read_desc.Width = read_desc.Height = 5;
    read_desc.MipLevels = read_desc.ArraySize = 1;
    read_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    read_desc.SampleDesc.Count = 1;
    read_desc.Usage = D3D11_USAGE_STAGING;
    read_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&read_desc, nullptr, &motion_grid_readback))) {
        EdpeLog(L"EDPE: motion candidate unavailable (grid readback creation failed)");
    } else {
        for (UINT row = 0; row < 5; ++row) for (UINT column = 0; column < 5; ++column) {
            const UINT x = (column * (desc.Width - 1) + 2) / 4;
            const UINT y = (row * (desc.Height - 1) + 2) / 4;
            const D3D11_BOX pixel{x, y, 0, x + 1, y + 1, 1};
            context->CopySubresourceRegion(motion_grid_readback.Get(), 0, column, row, 0,
                output.Get(), 0, &pixel);
        }
        motion_grid_wait = 0;
        EdpeLog(L"EDPE: motion candidate GPU pass completed; grid readback queued");
    }
    motion_pair_active = false;
    motion_depth.Reset();
    motion_depth_view.Reset();
    motion_hook_watch = 8;
}

static bool s_upscale_evaluated_this_frame = false;

bool isLdrSceneColorFormat(DXGI_FORMAT fmt) {
    return fmt == DXGI_FORMAT_R8G8B8A8_UNORM ||
           fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           fmt == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
           fmt == DXGI_FORMAT_B8G8R8A8_UNORM ||
           fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
           fmt == DXGI_FORMAT_R10G10B10A2_UNORM ||
           fmt == DXGI_FORMAT_R10G10B10A2_TYPELESS;
}

DXGI_FORMAT concreteLdrColorFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
        return fmt;
    }
}

static std::atomic<bool> s_comparison_capture_requested{false};
static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_backbuffer_copy;
static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_backbuffer_copy_srv;
static Microsoft::WRL::ComPtr<ID3D11RenderTargetView> s_backbuffer_copy_rtv;
static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_dlaa_input;
static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_dlaa_input_srv;
static edpe::TemporalFrameInputs s_last_temporal_inputs{};
static unsigned long long s_last_temporal_inputs_frame = ~0ull;

// - When render resolution == output resolution (DLAA), evaluates on the finished
//   LDR backbuffer (tone mapped, HUD included).
// - When render resolution < output resolution (DLSS Super Resolution via Cobra's
//   SSAAMultiplier / Supersampling < 1.0), evaluates on the tone-mapped low-res LDR
//   scene target (scene_ldr_source, renderWidth x renderHeight) and reconstructs
//   into the native-resolution backbuffer (outputWidth x outputHeight).
// `rendered` is the last_present_frame value seen while this frame rendered.
void tryTemporalUpscale(ID3D11DeviceContext* context, unsigned long long rendered,
    ID3D11Texture2D* backbuffer) {
    edpe::ScopedProfileTimer profTimer(edpe::ProfileZone::TemporalUpscale);
    const auto frame = rendered;
    auto& upscaler = edpe::UpscalerManager::instance();
    const bool upscale_active = upscaler.isReconstructionActive();
    const bool debug_view_active = s_jitter_debug_view.load(std::memory_order_relaxed);
    if ((!upscale_active && !debug_view_active) || !context || !backbuffer) return;

    const wchar_t* skip = nullptr;
    if (!motion_color_source || !scene_depth_source) skip = L"scene color/depth not observed";
    else if (motion_color_frame != rendered) skip = L"no scene MRT pass this frame";
    else if (s_camera_frame != rendered) {
        if (s_latest_unmapped_camera_valid && s_latest_unmapped_camera_frame == rendered) {
            updateMainCamera(s_latest_unmapped_camera, rendered);
        } else if (s_fallback_unmapped_camera_frame == rendered && ++s_auxiliary_rejection_streak >= 3) {
            // Genuine camera cut persisted across >= 3 frames (e.g. external camera switch):
            updateMainCamera(s_fallback_unmapped_camera, rendered);
            s_auxiliary_rejection_streak = 0;
        } else if (s_camera_now.scaleX > 0.0f) {
            // Camera was not unmapped or only a 1-frame auxiliary probe ran this frame.
            // Retain previous camera smoothly without resetting temporal history!
            s_camera_previous = s_camera_now;
            s_camera_previous_frame = s_camera_frame;
            s_camera_frame = rendered;
        } else {
            skip = L"no main camera latched this frame";
        }
    }
    if (skip) {
        if (++s_consecutive_skipped_frames > 2) {
            s_camera_reset = true;
        }
        return;
    }

    D3D11_TEXTURE2D_DESC backbufferDesc{};
    backbuffer->GetDesc(&backbufferDesc);
    if (!backbufferDesc.Width || !backbufferDesc.Height || backbufferDesc.SampleDesc.Count != 1) return;

    D3D11_TEXTURE2D_DESC depthDesc{};
    scene_depth_source->GetDesc(&depthDesc);
    if (!depthDesc.Width || !depthDesc.Height ||
        depthDesc.Width > backbufferDesc.Width || depthDesc.Height > backbufferDesc.Height) {
        return;
    }

    const bool isSubNative = (depthDesc.Width < backbufferDesc.Width ||
                              depthDesc.Height < backbufferDesc.Height);
    const auto desiredMode = (isSubNative && upscaler.getBackend() != edpe::UpscalerBackend::Taa)
        ? edpe::selectModeByRatio(depthDesc.Width, backbufferDesc.Width)
        : edpe::UpscaleMode::NativeAA;
    upscaler.setMode(desiredMode);
    const bool hdrInput = edpe::Config::instance().hdrInput;
    ID3D11Texture2D* colorSource = nullptr;
    if (hdrInput) {
        if (scene_hdr_color_source && scene_hdr_color_frame == rendered) {
            colorSource = scene_hdr_color_source.Get();
        } else if (scene_ldr_source && scene_ldr_frame == rendered) {
            colorSource = scene_ldr_source.Get();
        } else if (isSubNative && scene_ldr_source) {
            colorSource = scene_ldr_source.Get();
        } else if (!isSubNative) {
            colorSource = scene_hdr_color_source ? scene_hdr_color_source.Get() : backbuffer;
        } else {
            if (++s_consecutive_skipped_frames > 2) {
                s_camera_reset = true;
            }
            return;
        }
    } else {
        if (scene_ldr_source && scene_ldr_frame == rendered) {
            colorSource = scene_ldr_source.Get();
        } else if (isSubNative && scene_ldr_source) {
            colorSource = scene_ldr_source.Get();
        } else if (!isSubNative) {
            colorSource = backbuffer;
        } else {
            if (++s_consecutive_skipped_frames > 2) {
                s_camera_reset = true;
            }
            static unsigned long long s_last_ldr_log = ~0ull;
            if (s_last_ldr_log == ~0ull || frame >= s_last_ldr_log + 300) {
                s_last_ldr_log = frame;
                wchar_t msg[192];
                swprintf_s(msg, L"EDPE: reconstruction skipped frame=%llu (low-res LDR scene target %ux%u not observed)",
                    frame, depthDesc.Width, depthDesc.Height);
                EdpeLog(msg);
            }
            return;
        }
    }
    s_consecutive_skipped_frames = 0;

    D3D11_TEXTURE2D_DESC srcColorDesc{};
    colorSource->GetDesc(&srcColorDesc);
    if (srcColorDesc.Width != depthDesc.Width || srcColorDesc.Height != depthDesc.Height) {
        if (!isSubNative && colorSource != backbuffer) {
            colorSource = backbuffer;
            colorSource->GetDesc(&srcColorDesc);
            if (srcColorDesc.Width != depthDesc.Width || srcColorDesc.Height != depthDesc.Height) {
                return;
            }
        } else {
            static unsigned long long s_last_dim_log = ~0ull;
            if (s_last_dim_log == ~0ull || frame >= s_last_dim_log + 300) {
                s_last_dim_log = frame;
                wchar_t msg[192];
                swprintf_s(msg, L"EDPE: reconstruction skipped frame=%llu (color %ux%u != depth %ux%u)",
                    frame, srcColorDesc.Width, srcColorDesc.Height, depthDesc.Width, depthDesc.Height);
                EdpeLog(msg);
            }
            return;
        }
    }

    static bool s_first_exec_logged = false;
    if (!s_first_exec_logged) {
        s_first_exec_logged = true;
        wchar_t msg[192];
        swprintf_s(msg, L"EDPE: tryTemporalUpscale executing frame=%llu render=%ux%u output=%ux%u",
            frame, depthDesc.Width, depthDesc.Height, backbufferDesc.Width, backbufferDesc.Height);
        EdpeLog(msg);
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device) return;

    const DXGI_FORMAT inputFormat = concreteLdrColorFormat(srcColorDesc.Format);

    // Persistent copy of the input color texture with a concrete typed SRV format:
    // NGX needs a shader-readable input that is not also the output, and swap-chain
    // or typeless post-process buffers may fail NGX's internal default SRV creation.
    if (s_dlaa_input) {
        D3D11_TEXTURE2D_DESC cur{};
        s_dlaa_input->GetDesc(&cur);
        if (cur.Width != depthDesc.Width || cur.Height != depthDesc.Height ||
            cur.Format != inputFormat) {
            s_dlaa_input.Reset();
            s_dlaa_input_srv.Reset();
        }
    }
    if (!s_dlaa_input) {
        D3D11_TEXTURE2D_DESC inDesc{};
        inDesc.Width = depthDesc.Width;
        inDesc.Height = depthDesc.Height;
        inDesc.MipLevels = inDesc.ArraySize = 1;
        inDesc.Format = inputFormat;
        inDesc.SampleDesc.Count = 1;
        inDesc.Usage = D3D11_USAGE_DEFAULT;
        inDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&inDesc, nullptr, &s_dlaa_input))) return;
        device->CreateShaderResourceView(s_dlaa_input.Get(), nullptr, &s_dlaa_input_srv);
        s_camera_reset = true;
        wchar_t msg[160];
        swprintf_s(msg, L"EDPE: temporal color input created %ux%u format=%u (%ls)",
            depthDesc.Width, depthDesc.Height, static_cast<unsigned>(inputFormat),
            colorSource == scene_hdr_color_source.Get() ? L"HDR scene color (no UI)" :
            (isSubNative ? L"low-res LDR scene target" :
            (colorSource != backbuffer ? L"offscreen LDR scene target (no UI)" : L"native backbuffer")));
        EdpeLog(msg);
    }
    context->CopyResource(s_dlaa_input.Get(), colorSource);

    if (motion_depth) {
        D3D11_TEXTURE2D_DESC curDesc{};
        motion_depth->GetDesc(&curDesc);
        if (curDesc.Width != depthDesc.Width || curDesc.Height != depthDesc.Height) {
            motion_depth.Reset();
            motion_depth_view.Reset();
        }
    }

    if (!motion_depth) {
        D3D11_TEXTURE2D_DESC copyDesc = depthDesc;
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copyDesc.CPUAccessFlags = copyDesc.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, &motion_depth))) return;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(motion_depth.Get(), &srvDesc, &motion_depth_view))) return;
    }

    ID3D11RenderTargetView* savedRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* savedDsv = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtvs, &savedDsv);
    if (savedDsv) {
        if (const auto rawSet = original.load(std::memory_order_acquire)) {
            rawSet(context, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtvs, nullptr);
        }
    }
    struct RestoreTargetsGuard {
        ID3D11DeviceContext* ctx;
        OMSetRenderTargetsFn setFn;
        ID3D11RenderTargetView** rtvs;
        ID3D11DepthStencilView* dsv;
        ~RestoreTargetsGuard() {
            if (dsv && setFn) {
                setFn(ctx, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
            }
            for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
                if (rtvs[i]) rtvs[i]->Release();
            }
            if (dsv) dsv->Release();
        }
    } restoreTargets{context, original.load(std::memory_order_acquire), savedRtvs, savedDsv};

    context->CopyResource(motion_depth.Get(), scene_depth_source.Get());

    if (!motion_pass) {
        auto candidate = std::make_unique<edpe::MotionPass>();
        if (!candidate->initialize(device.Get(), context)) return;
        motion_pass = std::move(candidate);
    }

    if (s_zero_motion_vectors.load(std::memory_order_relaxed)) {
        motion_pass->clear(context);
    } else {
        // Render motion vectors at render resolution using current and previous camera projections
        motion_pass->render(motion_depth_view.Get(),
            s_camera_now, s_camera_previous, depthDesc.Width, depthDesc.Height);
    }

    if (!motion_pass->output()) return;

    Microsoft::WRL::ComPtr<ID3D11Resource> motionRes;
    motion_pass->output()->GetResource(&motionRes);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> motionTexture;
    if (!motionRes || FAILED(motionRes.As(&motionTexture))) return;

    float jx = 0.0f, jy = 0.0f;
    if (s_jitter_injected_frame == rendered) {
        jx = s_injected_jitter.pixelX;
        jy = s_injected_jitter.pixelY;
        s_last_rendered_jitter = s_injected_jitter;
    } else if (s_last_rendered_jitter.pixelX != 0.0f || s_last_rendered_jitter.pixelY != 0.0f) {
        // Main camera was not unmapped this frame (stationary cockpit / no buffer update).
        // The constant buffer on GPU still holds the previous frame's jitter!
        jx = s_last_rendered_jitter.pixelX;
        jy = s_last_rendered_jitter.pixelY;
    } else {
        // No jitter was injected into the scene projection for this frame!
        // Never pass phantom jitter to DLSS / FSR when the GPU rendered unjittered.
        jx = 0.0f;
        jy = 0.0f;
    }

    if (s_jitter_swap_xy.load(std::memory_order_relaxed)) {
        std::swap(jx, jy);
    }
    if (s_jitter_invert_x.load(std::memory_order_relaxed)) {
        jx = -jx;
    }
    if (s_jitter_invert_y.load(std::memory_order_relaxed)) {
        jy = -jy;
    }

    const float mScale = s_motion_scale.load(std::memory_order_relaxed);
    const float mScaleX = (s_motion_invert_x.load(std::memory_order_relaxed) ? -1.0f : 1.0f) * mScale;
    const float mScaleY = (s_motion_invert_y.load(std::memory_order_relaxed) ? -1.0f : 1.0f) * mScale;

    // Physical camera scene-cut detection based on position and rotation discontinuities.
    // A true scene cut requires an abrupt discontinuous camera change:
    // 1. First frame after initialization (no previous frame).
    // 2. Abrupt orientation discontinuity (rotTrace < 2.0f, corresponding to > 45 degree turn).
    // 3. Astronomical position teleport (moveDist > 5000.0f, e.g. supercruise drop / hyperspace).
    // 4. Large position jump (> 50.0f) ONLY IF accompanied by an orientation deviation (rotTrace < 2.85f).
    // Steady linear flight (rotTrace >= 2.85f) must NEVER trigger a scene cut regardless of velocity!
    const float* c = s_camera_now.worldFromView;
    const float* p = s_camera_previous.worldFromView;
    const float dx = c[3] - p[3];
    const float dy = c[7] - p[7];
    const float dz = c[11] - p[11];
    const float moveDist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float rotTrace = (c[0]*p[0] + c[4]*p[4] + c[8]*p[8]) +
                           (c[1]*p[1] + c[5]*p[5] + c[9]*p[9]) +
                           (c[2]*p[2] + c[6]*p[6] + c[10]*p[10]);
    const bool orientation_cut = (rotTrace < 2.0f);
    const bool teleport_cut = (moveDist > 5000.0f);
    const bool translation_cut = (moveDist > 50.0f && rotTrace < 2.85f);
    const bool scene_cut = (s_camera_previous_frame == ~0ull) || orientation_cut || teleport_cut || translation_cut;

    static LARGE_INTEGER s_last_upscale_qpc{};
    static LARGE_INTEGER s_upscale_freq{};
    if (s_upscale_freq.QuadPart == 0) QueryPerformanceFrequency(&s_upscale_freq);
    LARGE_INTEGER now_qpc;
    QueryPerformanceCounter(&now_qpc);
    float dt_ms = 16.667f;
    if (s_last_upscale_qpc.QuadPart > 0 && s_upscale_freq.QuadPart > 0) {
        const double measured_ms = static_cast<double>(now_qpc.QuadPart - s_last_upscale_qpc.QuadPart) * 1000.0 /
                                   static_cast<double>(s_upscale_freq.QuadPart);
        if (measured_ms >= 0.5 && measured_ms <= 250.0) {
            dt_ms = static_cast<float>(measured_ms);
        }
    }
    s_last_upscale_qpc = now_qpc;

    edpe::TemporalFrameInputs inputs{};
    inputs.color = s_dlaa_input.Get();
    inputs.colorSrv = s_dlaa_input_srv.Get();
    inputs.depth = motion_depth.Get();
    inputs.depthSrv = motion_depth_view.Get();
    inputs.motionVectors = motionTexture.Get();
    inputs.motionSrv = motion_pass->output();
    inputs.renderWidth = depthDesc.Width;
    inputs.renderHeight = depthDesc.Height;
    inputs.outputWidth = backbufferDesc.Width;
    inputs.outputHeight = backbufferDesc.Height;
    inputs.jitterX = jx;
    inputs.jitterY = jy;
    inputs.motionScaleX = mScaleX;
    inputs.motionScaleY = mScaleY;
    inputs.frameTimeMs = dt_ms;
    inputs.reset = s_camera_reset || scene_cut;
    inputs.cameraNow = s_camera_now;
    inputs.cameraPrevious = s_camera_previous;
    inputs.sharpness = edpe::Config::instance().sharpness;
    inputs.depthInverted = true;
    inputs.motionIsLowRes = true;
    const bool isHdrColor = hdrInput &&
                            (srcColorDesc.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
                             srcColorDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                             srcColorDesc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS);
    inputs.colorIsHdr = isHdrColor;

    inputs.reactiveMask = nullptr;
    inputs.reactiveMaskSrv = nullptr;

    s_last_temporal_inputs = inputs;
    s_last_temporal_inputs_frame = rendered;

    if (!upscale_active) {
        s_camera_reset = false;
    } else {
        static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_dlss_output_texture;
        static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_dlss_output_srv;
        static Microsoft::WRL::ComPtr<ID3D11RenderTargetView> s_backbuffer_rtv;
        static ID3D11Texture2D* s_cached_backbuffer_ptr = nullptr;

        const DXGI_FORMAT bbFormat = concreteLdrColorFormat(backbufferDesc.Format);
        const DXGI_FORMAT outFormat = isHdrColor ? srcColorDesc.Format : bbFormat;

        if (s_dlss_output_texture) {
            D3D11_TEXTURE2D_DESC cur{};
            s_dlss_output_texture->GetDesc(&cur);
            if (cur.Width != backbufferDesc.Width || cur.Height != backbufferDesc.Height ||
                cur.Format != outFormat) {
                s_dlss_output_texture.Reset();
                s_dlss_output_srv.Reset();
            }
        }
        if (!s_dlss_output_texture) {
            D3D11_TEXTURE2D_DESC td = backbufferDesc;
            td.Format = outFormat;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.CPUAccessFlags = td.MiscFlags = 0;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &s_dlss_output_texture)) ||
                FAILED(device->CreateShaderResourceView(s_dlss_output_texture.Get(), nullptr, &s_dlss_output_srv))) {
                s_dlss_output_texture.Reset();
                s_dlss_output_srv.Reset();
            }
        }

        if (s_backbuffer_copy) {
            D3D11_TEXTURE2D_DESC cur{};
            s_backbuffer_copy->GetDesc(&cur);
            if (cur.Width != backbufferDesc.Width || cur.Height != backbufferDesc.Height ||
                cur.Format != bbFormat) {
                s_backbuffer_copy.Reset();
                s_backbuffer_copy_srv.Reset();
                s_backbuffer_copy_rtv.Reset();
            }
        }
        if (!s_backbuffer_copy) {
            D3D11_TEXTURE2D_DESC td = backbufferDesc;
            td.Format = bbFormat;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.CPUAccessFlags = td.MiscFlags = 0;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &s_backbuffer_copy)) ||
                FAILED(device->CreateShaderResourceView(s_backbuffer_copy.Get(), nullptr, &s_backbuffer_copy_srv)) ||
                FAILED(device->CreateRenderTargetView(s_backbuffer_copy.Get(), nullptr, &s_backbuffer_copy_rtv))) {
                s_backbuffer_copy.Reset();
                s_backbuffer_copy_srv.Reset();
                s_backbuffer_copy_rtv.Reset();
            }
        }

        if (s_cached_backbuffer_ptr != backbuffer || !s_backbuffer_rtv) {
            s_cached_backbuffer_ptr = backbuffer;
            s_backbuffer_rtv.Reset();
            device->CreateRenderTargetView(backbuffer, nullptr, &s_backbuffer_rtv);
        }

        ID3D11Texture2D* evalDestination = s_dlss_output_texture ? s_dlss_output_texture.Get() : backbuffer;

        // Copy backbuffer (containing native UI drawn by Cobra) to s_backbuffer_copy BEFORE any modification
        if (s_backbuffer_copy) {
            context->CopyResource(s_backbuffer_copy.Get(), backbuffer);
        }

        bool compOk = false;
        if (upscaler.evaluate(context, inputs, nullptr, evalDestination)) {
            s_camera_reset = false;

            const bool canCompositeUi = (s_preserve_native_ui.load(std::memory_order_relaxed) &&
                                         evalDestination == s_dlss_output_texture.Get() &&
                                         s_dlss_output_srv && s_backbuffer_copy_srv &&
                                         s_dlaa_input_srv && s_backbuffer_rtv &&
                                         colorSource != backbuffer);
            if (canCompositeUi) {
                if (!ui_composite_pass) {
                    auto comp = std::make_unique<edpe::UiCompositePass>();
                    if (comp->initialize(device.Get(), context)) {
                        ui_composite_pass = std::move(comp);
                    }
                }
                if (ui_composite_pass) {
                    compOk = ui_composite_pass->composite(context,
                        s_dlss_output_srv.Get(),
                        s_backbuffer_copy_srv.Get(),
                        s_dlaa_input_srv.Get(),
                        s_backbuffer_rtv.Get(),
                        backbufferDesc.Width, backbufferDesc.Height,
                        depthDesc.Width, depthDesc.Height,
                        isHdrColor);
                }
                if (compOk) {
                    static unsigned long long s_comp_log = ~0ull;
                    if (s_comp_log != frame && (s_comp_log == ~0ull || frame % 300 == 0)) {
                        s_comp_log = frame;
                        wchar_t cmsg[192];
                        swprintf_s(cmsg, L"EDPE: UI composited frame=%llu render=%ux%u output=%ux%u (%ls, %ls)",
                            frame, depthDesc.Width, depthDesc.Height, backbufferDesc.Width, backbufferDesc.Height,
                            isSubNative ? L"Super Resolution" : L"Native Resolution",
                            isHdrColor ? L"HDR" : L"LDR");
                        EdpeLog(cmsg);
                    }
                } else if (!isHdrColor) {
                    context->CopyResource(backbuffer, s_dlss_output_texture.Get());
                }
            } else if (evalDestination == s_dlss_output_texture.Get()) {
                if (!isHdrColor) {
                    context->CopyResource(backbuffer, s_dlss_output_texture.Get());
                } else {
                    if (!ui_composite_pass) {
                        auto comp = std::make_unique<edpe::UiCompositePass>();
                        if (comp->initialize(device.Get(), context)) {
                            ui_composite_pass = std::move(comp);
                        }
                    }
                    if (ui_composite_pass) {
                        compOk = ui_composite_pass->composite(context,
                            s_dlss_output_srv.Get(),
                            s_backbuffer_copy_srv.Get(),
                            s_dlaa_input_srv.Get(),
                            s_backbuffer_rtv.Get(),
                            backbufferDesc.Width, backbufferDesc.Height,
                            depthDesc.Width, depthDesc.Height,
                            true);
                    }
                }
            }
        }

        {
            const bool evalToIntermediate = (evalDestination == s_dlss_output_texture.Get());
            static bool s_path_state_valid = false;
            static ID3D11Texture2D* s_prev_color_source = nullptr;
            static DXGI_FORMAT s_prev_color_format = DXGI_FORMAT_UNKNOWN;
            static bool s_prev_is_hdr = false;
            static bool s_prev_eval_intermediate = false;
            static bool s_prev_comp_ok = false;
            static unsigned s_path_log_count = 0;
            if (!s_path_state_valid ||
                s_prev_color_source != colorSource ||
                s_prev_color_format != srcColorDesc.Format ||
                s_prev_is_hdr != isHdrColor ||
                s_prev_eval_intermediate != evalToIntermediate ||
                s_prev_comp_ok != compOk) {
                s_path_state_valid = true;
                s_prev_color_source = colorSource;
                s_prev_color_format = srcColorDesc.Format;
                s_prev_is_hdr = isHdrColor;
                s_prev_eval_intermediate = evalToIntermediate;
                s_prev_comp_ok = compOk;
                if (s_path_log_count < 200) {
                    ++s_path_log_count;
                    wchar_t pmsg[192];
                    swprintf_s(pmsg,
                        L"EDPE: temporal path change frame=%llu src=%p fmt=%u hdr=%d eval=%d comp=%d",
                        frame, static_cast<const void*>(colorSource),
                        static_cast<unsigned>(srcColorDesc.Format),
                        isHdrColor ? 1 : 0,
                        evalToIntermediate ? 1 : 0,
                        compOk ? 1 : 0);
                    EdpeLog(pmsg);
                }
            }
        }
    }

    // Push frame jitter diagnostics and log entry to asynchronous JitterLogger
    {
        static unsigned s_last_writes = 0;
        const unsigned cur_writes = jittered_writes_count.load(std::memory_order_relaxed);
        const unsigned writes_this_frame = cur_writes - s_last_writes;
        s_last_writes = cur_writes;

        const unsigned phases = s_active_jitter_phases.load(std::memory_order_relaxed);

        edpe::JitterFrameLog jlog{};
        jlog.frame = rendered;
        jlog.deltaMs = dt_ms;
        jlog.phase = static_cast<unsigned>(rendered % (phases ? phases : 32));
        jlog.totalPhases = phases ? phases : 32;
        jlog.pixelX = s_injected_jitter.pixelX;
        jlog.pixelY = s_injected_jitter.pixelY;
        jlog.ndcX = s_injected_jitter.ndcX;
        jlog.ndcY = s_injected_jitter.ndcY;
        jlog.injectedWrites = writes_this_frame;
        jlog.rejections = jitter_rejection_count.load(std::memory_order_relaxed);
        jlog.evalJx = inputs.jitterX;
        jlog.evalJy = inputs.jitterY;
        jlog.syncOk = (s_jitter_injected_frame == rendered);
        jlog.cameraValid = (s_camera_now.scaleX > 0.0f);
        jlog.heldJitter = (s_jitter_injected_frame != rendered && (s_last_rendered_jitter.pixelX != 0.0f || s_last_rendered_jitter.pixelY != 0.0f));
        jlog.sceneCut = scene_cut || s_camera_reset;
        jlog.camMoveDist = moveDist;
        jlog.camRotDeg = std::acos(std::clamp((rotTrace - 1.0f) * 0.5f, -1.0f, 1.0f)) * 57.29578f;
        edpe::JitterLogger::instance().pushFrame(jlog);
    }

    rearmContextHooks();
}

void renderJitterDebugView(ID3D11DeviceContext* context, unsigned long long rendered,
    ID3D11Texture2D* backbuffer) {
    if (!context || !backbuffer) return;

    D3D11_TEXTURE2D_DESC bbDesc{};
    backbuffer->GetDesc(&bbDesc);
    if (!bbDesc.Width || !bbDesc.Height || bbDesc.SampleDesc.Count != 1) return;

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device) return;

    static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_debug_input_copy;
    static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_debug_input_srv;
    static Microsoft::WRL::ComPtr<ID3D11RenderTargetView> s_debug_backbuffer_rtv;
    static ID3D11Texture2D* s_debug_cached_backbuffer = nullptr;

    if (s_debug_cached_backbuffer != backbuffer || !s_debug_backbuffer_rtv) {
        s_debug_cached_backbuffer = backbuffer;
        s_debug_backbuffer_rtv.Reset();
        if (FAILED(device->CreateRenderTargetView(backbuffer, nullptr, &s_debug_backbuffer_rtv))) {
            return;
        }
    }

    static std::unique_ptr<edpe::JitterDebugPass> s_jitter_debug_pass;
    if (!s_jitter_debug_pass) {
        auto pass = std::make_unique<edpe::JitterDebugPass>();
        if (!pass->initialize(device.Get(), context)) {
            return;
        }
        s_jitter_debug_pass = std::move(pass);
    }

    ID3D11ShaderResourceView* inputColorSrv = nullptr;
    ID3D11ShaderResourceView* motionSrv = nullptr;
    ID3D11ShaderResourceView* depthSrv = nullptr;
    UINT renderW = bbDesc.Width;
    UINT renderH = bbDesc.Height;
    float jx = 0.0f, jy = 0.0f;
    bool reset = true;
    bool isHdr = false;

    if (s_last_temporal_inputs_frame == rendered && s_last_temporal_inputs.colorSrv != nullptr) {
        inputColorSrv = s_last_temporal_inputs.colorSrv;
        motionSrv = s_last_temporal_inputs.motionSrv;
        depthSrv = s_last_temporal_inputs.depthSrv;
        renderW = s_last_temporal_inputs.renderWidth ? s_last_temporal_inputs.renderWidth : bbDesc.Width;
        renderH = s_last_temporal_inputs.renderHeight ? s_last_temporal_inputs.renderHeight : bbDesc.Height;
        jx = s_last_temporal_inputs.jitterX;
        jy = s_last_temporal_inputs.jitterY;
        reset = s_last_temporal_inputs.reset;
        isHdr = s_last_temporal_inputs.colorIsHdr;
    } else {
        const DXGI_FORMAT typedFormat = concreteLdrColorFormat(bbDesc.Format);
        if (s_debug_input_copy) {
            D3D11_TEXTURE2D_DESC cur{};
            s_debug_input_copy->GetDesc(&cur);
            if (cur.Width != bbDesc.Width || cur.Height != bbDesc.Height || cur.Format != typedFormat) {
                s_debug_input_copy.Reset();
                s_debug_input_srv.Reset();
            }
        }
        if (!s_debug_input_copy) {
            D3D11_TEXTURE2D_DESC td = bbDesc;
            td.Format = typedFormat;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = td.MiscFlags = 0;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &s_debug_input_copy)) ||
                FAILED(device->CreateShaderResourceView(s_debug_input_copy.Get(), nullptr, &s_debug_input_srv))) {
                s_debug_input_copy.Reset();
                s_debug_input_srv.Reset();
                return;
            }
        }
        context->CopyResource(s_debug_input_copy.Get(), backbuffer);
        inputColorSrv = s_debug_input_srv.Get();
    }

    const float sensitivity = s_jitter_debug_sensitivity.load(std::memory_order_relaxed);
    const float threshold = s_jitter_debug_threshold.load(std::memory_order_relaxed);
    const int mode = s_jitter_debug_mode.load(std::memory_order_relaxed);

    s_jitter_debug_pass->render(context,
        inputColorSrv,
        s_debug_backbuffer_rtv.Get(),
        bbDesc.Width, bbDesc.Height,
        jx, jy,
        sensitivity, threshold, mode,
        motionSrv,
        depthSrv,
        renderW, renderH,
        reset,
        isHdr);

    auto& recorder = edpe::JitterViewRecorder::instance();
    if (recorder.isActive()) {
        const auto& passInfo = s_jitter_debug_pass->getLastFrameInfo();
        const bool motionNull = (motionSrv == nullptr);
        const bool depthNull = (depthSrv == nullptr);
        recorder.onFrame(context, backbuffer, rendered, bbDesc.Width, bbDesc.Height,
            renderW, renderH, jx, jy, isHdr, reset,
            passInfo, motionNull, depthNull, sensitivity, threshold, mode, inputColorSrv);
    }
}

void logSnapshotColorTarget(size_t depth_index, unsigned bind_ordinal, UINT count,
    ID3D11RenderTargetView* const* targets) {
    wchar_t message[192];
    swprintf_s(message, L"EDPE: DSV #%zu bind=%u color target count=%u",
        depth_index, bind_ordinal, count);
    EdpeLog(message);
    for (UINT slot = 0; slot < count && slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++slot) {
        ID3D11RenderTargetView* color = targets ? targets[slot] : nullptr;
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        D3D11_TEXTURE2D_DESC texture_desc{};
        if (color) {
            color->GetDesc(&view);
            ID3D11Resource* resource = nullptr;
            color->GetResource(&resource);
            if (resource) {
                ID3D11Texture2D* texture = nullptr;
                if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                        reinterpret_cast<void**>(&texture)))) {
                    texture->GetDesc(&texture_desc);
                    texture->Release();
                }
                resource->Release();
            }
        }
        swprintf_s(message,
            L"EDPE: DSV #%zu bind=%u rtv%u=%p format=%u size=%ux%u bind=0x%X",
            depth_index, bind_ordinal, slot, color, static_cast<unsigned>(view.Format),
            texture_desc.Width, texture_desc.Height, texture_desc.BindFlags);
        EdpeLog(message);
    }
}

bool isMainSceneMrtDepth(const D3D11_TEXTURE2D_DESC& dd, UINT outW, UINT outH, bool sameFrame) {
    if (dd.Format != DXGI_FORMAT_R32G8X24_TYPELESS || dd.SampleDesc.Count != 1) return false;
    if (!dd.Width || !dd.Height) return false;
    if (outW > 0 && outH > 0) {
        if (dd.Width > outW || dd.Height > outH) return false;
        if (outW > outH && dd.Width == dd.Height) return false;
        const float dsvAspect = static_cast<float>(dd.Width) / static_cast<float>(dd.Height);
        const float outAspect = static_cast<float>(outW) / static_cast<float>(outH);
        if (std::fabs(dsvAspect - outAspect) > 0.05f) return false;
        if (outW >= 640 && (dd.Width < outW / 3 || dd.Height < outH / 3)) return false;
        // Within the same frame, never downgrade an already established main scene depth buffer to a lower-resolution reflection/probe buffer
        const UINT currentW = s_observed_scene_width.load(std::memory_order_relaxed);
        if (sameFrame && scene_depth_source && currentW > 0 && dd.Width < currentW) return false;
    }
    return true;
}

void STDMETHODCALLTYPE observedOMSetRenderTargets(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* dsv) {
    const auto forward = original.load(std::memory_order_acquire);
    if (s_in_edpe_present.load(std::memory_order_acquire)) {
        forward(context, count, targets, dsv);
        return;
    }
    if (context == observed_context.load(std::memory_order_acquire)) {
        endVSBufferProbe();
        endPipelineSample(context);
    }
    forward(context, count, targets, dsv);
    if (context != observed_context.load(std::memory_order_acquire)) return;
    copyCameraDeltaSource(context);
    rearmHudDrawProbe();
    edpe::ScopedProfileTimer profTimer(edpe::ProfileZone::OMSetRenderTargets);
    const bool reconstruction_active =
        edpe::UpscalerManager::instance().isReconstructionActive() ||
        s_jitter_debug_view.load(std::memory_order_relaxed);
    const auto current_frame = last_present_frame.load(std::memory_order_relaxed);
    if ((motion_pair_active || reconstruction_active) && motion_color_source &&
        motion_color_frame == current_frame) {
        bool bound = false;
        UINT non_null_count = 0;
        for (UINT slot = 0; targets && slot < count; ++slot) {
            if (targets[slot]) {
                ++non_null_count;
                bound |= sameRenderTargetResource(targets[slot], motion_color_source.Get());
            }
        }
        // Cobra transitions the HDR color target from MRT RTV3 (Binds 1-3) to RTV0 (Binds 4-7).
        // In Cobra Odyssey, OMSetRenderTargets is called with NumViews=8 and null in unused slots.
        const bool is_hdr_slot0 = count >= 1 && targets && targets[0] &&
            sameRenderTargetResource(targets[0], motion_color_source.Get()) &&
            (non_null_count < 4 || (count > 3 && targets[3] == nullptr));

        if (motion_color_bound && !bound) {
            // Main scene G-buffer pass has completed and unbound!
            s_inside_main_scene.store(false, std::memory_order_release);
            if (motion_pair_active && !motion_color_first_exit_logged) {
                motion_color_first_exit_logged = true;
                wchar_t message[160];
                swprintf_s(message,
                    L"EDPE: motion HDR RTV3 first exit frame=%llu nextRTVs=%u nextDSV=%p",
                    motion_color_frame, count, dsv);
                EdpeLog(message);
                captureEarlyMotionColor(context, false);
            }
        } else if (is_hdr_slot0) {
            // Transitioned from 4-MRT G-buffer (Binds 1-3) to RTV0 deferred lighting/HUD (Binds 4-7)
            s_inside_main_scene.store(false, std::memory_order_release);
            s_hdr_slot0_seen_frame = current_frame;
        }
        if (!motion_color_bound && bound) motion_color_rebound = true;
        motion_color_bound = bound;

        if (bound) {
            scene_ldr_source.Reset();
            scene_ldr_frame = ~0ull;
        } else if (count >= 1 && targets && targets[0] && scene_ldr_frame != current_frame &&
                   s_hdr_slot0_seen_frame == current_frame) {
            const UINT sceneW = s_observed_scene_width.load(std::memory_order_relaxed);
            const UINT sceneH = s_observed_scene_height.load(std::memory_order_relaxed);
            if (sceneW > 0 && sceneH > 0) {
                ID3D11Resource* res0 = nullptr;
                targets[0]->GetResource(&res0);
                if (res0) {
                    const bool isGbuffer =
                        res0 == s_gbuffer_resources[0] ||
                        res0 == s_gbuffer_resources[1] ||
                        res0 == s_gbuffer_resources[2] ||
                        res0 == s_gbuffer_resources[3];
                    const bool isBackbuffer =
                        res0 == s_swapchain_backbuffer.load(std::memory_order_relaxed);
                    if (!isGbuffer && !isBackbuffer) {
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex0;
                        if (SUCCEEDED(res0->QueryInterface(__uuidof(ID3D11Texture2D),
                                reinterpret_cast<void**>(tex0.GetAddressOf())))) {
                            D3D11_TEXTURE2D_DESC td{};
                            tex0->GetDesc(&td);
                            if (td.Width == sceneW && td.Height == sceneH &&
                                td.SampleDesc.Count == 1 && isLdrSceneColorFormat(td.Format)) {
                                scene_ldr_source = tex0;
                                scene_ldr_frame = current_frame;
                            }
                        }
                    }
                    res0->Release();
                }
            }
        }
    }
    if (!dsv) {
        if (sequence_active.load(std::memory_order_acquire)) {
            std::lock_guard lock(seen_mutex);
            recordBind(-1);
        }
        return;
    }
    dsv_binds.fetch_add(1, std::memory_order_relaxed);

    bool scene_mrt = false;
    if (count >= 4 && targets && targets[0] && targets[1] && targets[2] && targets[3]) {
        D3D11_RENDER_TARGET_VIEW_DESC views[4]{};
        for (UINT slot = 0; slot < 4; ++slot) targets[slot]->GetDesc(&views[slot]);
        scene_mrt = views[0].Format == DXGI_FORMAT_R10G10B10A2_UNORM &&
            views[1].Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
            views[2].Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
            views[3].Format == DXGI_FORMAT_R11G11B10_FLOAT;
    }

    bool is_main_scene = false;
    bool hud_probe_begin = false;
    if (scene_mrt) {
        // Once the main 3D scene has already transitioned from 4-MRT (Binds 1-3) to RTV0 lighting (Binds 4-7)
        // in this frame, ignore any late 1-second auxiliary 4-MRT pass so it cannot clobber depth/color/LDR targets.
        const bool main_gbuffer_already_finished =
            (s_hdr_slot0_seen_frame == current_frame && scene_depth_source &&
             motion_color_source && motion_color_frame == current_frame);
        if (!main_gbuffer_already_finished) {
            ID3D11Resource* dsv_res = nullptr;
            dsv->GetResource(&dsv_res);
            if (dsv_res) {
                Microsoft::WRL::ComPtr<ID3D11Texture2D> depthTex;
                if (SUCCEEDED(dsv_res->QueryInterface(__uuidof(ID3D11Texture2D),
                        reinterpret_cast<void**>(depthTex.GetAddressOf())))) {
                    D3D11_TEXTURE2D_DESC dd{};
                    depthTex->GetDesc(&dd);
                    const UINT outW = s_swapchain_width.load(std::memory_order_relaxed);
                    const UINT outH = s_swapchain_height.load(std::memory_order_relaxed);
                    const bool same_frame = (s_observed_scene_frame == current_frame);
                    if (isMainSceneMrtDepth(dd, outW, outH, same_frame)) {
                        is_main_scene = true;
                        scene_depth_source = depthTex;
                        s_observed_scene_width.store(dd.Width, std::memory_order_relaxed);
                        s_observed_scene_height.store(dd.Height, std::memory_order_relaxed);
                        s_observed_scene_frame = current_frame;
                    }
                }
                dsv_res->Release();
            }
        }

        if (is_main_scene) {
            s_inside_main_scene.store(true, std::memory_order_release);
            for (UINT slot = 0; slot < 4; ++slot) {
                ID3D11Resource* gbRes = nullptr;
                targets[slot]->GetResource(&gbRes);
                s_gbuffer_resources[slot] = gbRes;
                if (gbRes) gbRes->Release();
            }
            scene_ldr_source.Reset();
            scene_ldr_frame = ~0ull;

            if (reconstruction_active) {
                motion_color_source = targets[3];
                motion_color_frame = current_frame;
                motion_color_bound = true;
                ID3D11Resource* hdrRes = nullptr;
                targets[3]->GetResource(&hdrRes);
                if (hdrRes) {
                    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
                    if (SUCCEEDED(hdrRes->QueryInterface(__uuidof(ID3D11Texture2D),
                            reinterpret_cast<void**>(tex.GetAddressOf())))) {
                        scene_hdr_color_source = tex;
                        scene_hdr_color_frame = current_frame;
                    }
                    hdrRes->Release();
                }
                if (!s_hud_probe_armed_once.load(std::memory_order_relaxed) && !hud_draw_hook_mask) {
                    s_hud_probe_armed_once.store(true, std::memory_order_relaxed);
                    beginHudDrawProbe();
                }
            }

            if (s_camera_frame != current_frame && s_latest_unmapped_camera_valid &&
                s_latest_unmapped_camera_frame == current_frame) {
                updateMainCamera(s_latest_unmapped_camera, current_frame);
            }
        } else {
            s_inside_main_scene.store(false, std::memory_order_release);
        }
    }


    size_t index = 0;
    bool first_bind = false;
    bool first_color = false;
    unsigned snapshot_probe_bind = 0;
    unsigned camera_pair_sample = 0;
    bool camera_delta_take = false;
    {
        std::lock_guard lock(seen_mutex);
        while (index < seen_count && seen[index].view != dsv) ++index;
        if (index == seen.size()) {
            if (sequence_active.load(std::memory_order_acquire)) recordBind(-3);
            return;
        }
        if (index == seen_count) {
            seen[seen_count++].view = dsv;
            first_bind = true;
        }
        ++seen[index].interval_binds;
        if (seen[index].texture_format == DXGI_FORMAT_R32G8X24_TYPELESS &&
            seen[index].samples == 1) {
            const auto frame = current_frame;
            if (seen[index].mrt_frame != frame) {
                seen[index].mrt_hdr_view = nullptr;
                seen[index].mrt_frame = frame;
            }
            ID3D11RenderTargetView* hdrRtv = nullptr;
            if (is_main_scene) {
                seen[index].mrt_hdr_view = targets[3];
                hdrRtv = targets[3];
            } else if (seen[index].mrt_hdr_view && count && targets &&
                targets[0] == seen[index].mrt_hdr_view) {
                seen[index].scene_match_frame = frame;
                hdrRtv = targets[0];
            } else if (count && targets && targets[0]) {
                D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
                targets[0]->GetDesc(&rtvd);
                if (rtvd.Format == DXGI_FORMAT_R11G11B10_FLOAT) {
                    hdrRtv = targets[0];
                }
            }
            if (hdrRtv) {
                ID3D11Resource* res = nullptr;
                hdrRtv->GetResource(&res);
                if (res) {
                    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
                    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),
                            reinterpret_cast<void**>(tex.GetAddressOf())))) {
                        scene_hdr_color_source = tex;
                        scene_hdr_color_frame = frame;
                    }
                    res->Release();
                }
            }
            camera_delta_take = is_main_scene;
        }
        if (sequence_active.load(std::memory_order_acquire)) recordBind(static_cast<int>(index));
        if (snapshot_active == static_cast<int>(index) && !snapshot_view) {
            dsv->AddRef();
            snapshot_view = dsv;
            snapshot_index = snapshot_active;
            snapshot_active = -1;
            snapshot_waiting.store(false, std::memory_order_release);
        }
        if (snapshot_view == dsv && snapshot_index == static_cast<int>(index)) {
            auto* color = count && targets ? targets[0] : nullptr;
            if (color != snapshot_color_view) {
                if (color) color->AddRef();
                if (snapshot_color_view) snapshot_color_view->Release();
                snapshot_color_view = color;
            }
            const auto after = last_present_frame.load(std::memory_order_relaxed);
            if (!snapshot_bind_count) snapshot_first_bind_after = after;
            snapshot_last_bind_after = after;
            ++snapshot_bind_count;
            snapshot_probe_bind = snapshot_bind_count;
        }
        if (camera_pair_target == static_cast<int>(index)) {
            const auto frame = last_present_frame.load(std::memory_order_relaxed);
            if (frame == camera_pair_armed_after || frame == camera_pair_armed_after + 1) {
                if (camera_pair_bind_frame != frame) {
                    camera_pair_bind_frame = frame;
                    camera_pair_bind_count = 0;
                }
                if (++camera_pair_bind_count == 3 && is_main_scene &&
                    seen[index].mrt_frame == frame && seen[index].mrt_hdr_view)
                    camera_pair_sample = frame == camera_pair_armed_after
                        ? kPairFirstSample : kPairSecondSample;
                if (camera_pair_sample) {
                    auto& depth = camera_pair_depth[camera_pair_sample - kPairFirstSample];
                    if (!depth.view && !depth.staging) {
                        dsv->AddRef();
                        depth.view = dsv;
                        depth.after_present = frame;
                    }
                    if (motion_pair_active && camera_pair_sample == kPairSecondSample &&
                        targets[3] && !motion_color_source) {
                        motion_color_source = targets[3];
                        motion_color_frame = frame;
                        motion_color_bound = true;
                        hud_probe_begin = true;
                    }
                }
            }
        }
        if (count && targets && targets[0] && !seen[index].color_logged) {
            seen[index].color_logged = true;
            first_color = true;
        }
    }

    if (hud_probe_begin) beginHudDrawProbe();
    if (snapshot_probe_bind) {
        beginPipelineSample(context, snapshot_probe_bind);
        logSnapshotColorTarget(index, snapshot_probe_bind, count, targets);
        logBoundConstantBuffers(context, index, snapshot_probe_bind);
        if (snapshot_probe_bind >= 1 && snapshot_probe_bind <= 3)
            beginVSBufferProbe(context, snapshot_probe_bind);
    }
    if (camera_pair_sample) queueCameraPairSample(context, camera_pair_sample);
    if (camera_delta_take)
        takeCameraDeltaSource(context, last_present_frame.load(std::memory_order_relaxed));

    if (!first_bind && !first_color) return;

    D3D11_DEPTH_STENCIL_VIEW_DESC view{};
    dsv->GetDesc(&view);
    D3D11_TEXTURE2D_DESC texture_desc{};
    ID3D11Resource* resource = nullptr;
    dsv->GetResource(&resource);
    if (resource) {
        ID3D11Texture2D* texture = nullptr;
        if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&texture)))) {
            texture->GetDesc(&texture_desc);
            texture->Release();
        }
        resource->Release();
    }
    if (first_bind) {
        std::lock_guard lock(seen_mutex);
        seen[index].texture_format = texture_desc.Format;
        seen[index].samples = texture_desc.SampleDesc.Count;
    }
    D3D11_RENDER_TARGET_VIEW_DESC color_view{};
    D3D11_TEXTURE2D_DESC color_texture{};
    ID3D11RenderTargetView* color = count && targets ? targets[0] : nullptr;
    if (color) {
        color->GetDesc(&color_view);
        ID3D11Resource* color_resource = nullptr;
        color->GetResource(&color_resource);
        if (color_resource) {
            ID3D11Texture2D* texture = nullptr;
            if (SUCCEEDED(color_resource->QueryInterface(__uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&texture)))) {
                texture->GetDesc(&color_texture);
                texture->Release();
            }
            color_resource->Release();
        }
    }
    wchar_t message[360];
    swprintf_s(message,
        L"EDPE: DSV bind #%zu phase=%s view=%p %ux%u textureFormat=%u viewFormat=%u bind=0x%X samples=%u rtvCount=%u rtv0=%p color=%ux%u colorFormat=%u colorBind=0x%X",
        index, first_bind ? L"first" : L"first-color", dsv,
        texture_desc.Width, texture_desc.Height,
        static_cast<unsigned>(texture_desc.Format), static_cast<unsigned>(view.Format),
        texture_desc.BindFlags, texture_desc.SampleDesc.Count, count, color,
        color_texture.Width, color_texture.Height,
        static_cast<unsigned>(color_view.Format), color_texture.BindFlags);
    EdpeLog(message);
}

bool patchSlot(void** table, size_t slot, void* expected, void* replacement) {
    DWORD old_protection = 0;
    if (!VirtualProtect(table + slot, sizeof(void*), PAGE_READWRITE,
            &old_protection)) return false;
    void* replaced = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(table + slot), replacement, expected);
    DWORD ignored = 0;
    VirtualProtect(table + slot, sizeof(void*), old_protection, &ignored);
    return replaced == expected;
}

void rearmContextHooks() {
    if (!patched_table) return;
    const auto forward_om = original.load(std::memory_order_acquire);
    if (forward_om && patched_table[kOMSetRenderTargets] != reinterpret_cast<void*>(&observedOMSetRenderTargets)) {
        const bool restored = patchSlot(patched_table, kOMSetRenderTargets,
            patched_table[kOMSetRenderTargets],
            reinterpret_cast<void*>(&observedOMSetRenderTargets));
        static bool s_swap_logged = false;
        if (restored && !s_swap_logged) {
            s_swap_logged = true;
            EdpeLog(L"EDPE: DSV observer restored after context-state swap");
        }
    }
    const auto forward_map = original_map.load(std::memory_order_acquire);
    if (forward_map && map_hook_active.load(std::memory_order_acquire) &&
        patched_table[kMap] != reinterpret_cast<void*>(&observedMap)) {
        patchSlot(patched_table, kMap,
            patched_table[kMap],
            reinterpret_cast<void*>(&observedMap));
    }
    const auto forward_unmap = original_unmap.load(std::memory_order_acquire);
    if (forward_unmap && unmap_hook_active.load(std::memory_order_acquire) &&
        patched_table[kUnmap] != reinterpret_cast<void*>(&observedUnmap)) {
        patchSlot(patched_table, kUnmap,
            patched_table[kUnmap],
            reinterpret_cast<void*>(&observedUnmap));
    }
}
} // namespace

void ContextCensusOnPresent(IDXGISwapChain* swap_chain, unsigned long long frame, UINT flags) {
    if (!s_config_loaded) {
        s_config_loaded = true;
        auto& cfg = edpe::Config::instance();
        cfg.load();
        s_jitter_invert_x.store(cfg.jitterInvertX, std::memory_order_relaxed);
        s_jitter_invert_y.store(cfg.jitterInvertY, std::memory_order_relaxed);
        s_jitter_swap_xy.store(cfg.jitterSwapXY, std::memory_order_relaxed);
        s_jitter_scale.store(cfg.jitterScale, std::memory_order_relaxed);
        s_jitter_phase_count.store(cfg.jitterPhaseCount, std::memory_order_relaxed);
        s_preserve_native_ui.store(cfg.preserveNativeUi, std::memory_order_relaxed);
        s_jitter_debug_view.store(cfg.jitterDebugView, std::memory_order_relaxed);
        s_jitter_debug_sensitivity.store(cfg.jitterDebugSensitivity, std::memory_order_relaxed);
        s_jitter_debug_threshold.store(cfg.jitterDebugThreshold, std::memory_order_relaxed);
        s_jitter_debug_mode.store(cfg.jitterDebugMode, std::memory_order_relaxed);
        s_zero_motion_vectors.store(cfg.zeroMotionVectors, std::memory_order_relaxed);
        s_motion_invert_x.store(cfg.motionInvertX, std::memory_order_relaxed);
        s_motion_invert_y.store(cfg.motionInvertY, std::memory_order_relaxed);
        s_motion_scale.store(cfg.motionScale, std::memory_order_relaxed);
        edpe::NgxContext::set_preset(static_cast<uint32_t>(cfg.dlssPreset));

        if (cfg.upscalerBackend >= 1 && cfg.upscalerBackend <= 3) {
            auto& upscaler = edpe::UpscalerManager::instance();
            const auto targetBackend = (cfg.upscalerBackend == 1) ? edpe::UpscalerBackend::NvidiaDlss :
                                       (cfg.upscalerBackend == 2) ? edpe::UpscalerBackend::AmdFsr :
                                                                    edpe::UpscalerBackend::Taa;
            if (upscaler.isBackendAvailable(targetBackend)) {
                upscaler.setBackend(targetBackend);
                const int m = (cfg.upscaleMode >= 1 && cfg.upscaleMode <= 5) ? cfg.upscaleMode : 1;
                upscaler.setMode(static_cast<edpe::UpscaleMode>(m));
            }
        }
    }
    s_inside_main_scene.store(false, std::memory_order_release);
    struct PresentScopeGuard {
        PresentScopeGuard() { s_in_edpe_present.store(true, std::memory_order_release); }
        ~PresentScopeGuard() { s_in_edpe_present.store(false, std::memory_order_release); }
    } present_scope_guard;
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain) {
        auto* context = observed_context.load(std::memory_order_acquire);
        endHudDrawProbe();
        endVSBufferProbe();
        endPipelineSample(context);
        pollPipelineSamples(context);
    }
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain)
        pollConstantBufferSamples(observed_context.load(std::memory_order_acquire));
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain) {
        auto* context = observed_context.load(std::memory_order_acquire);
        copyCameraDeltaSource(context);
        pollCameraDeltaRing(context);
    }
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain) {
        auto* context = observed_context.load(std::memory_order_acquire);
        queueCameraPairDepth(context, frame);
        pollCameraPairDepth(context);
        pollMotionGrid(context);
        tryMotionPair(context, frame);
    }
    const auto rendered_frame = last_present_frame.load(std::memory_order_relaxed);
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain)
        last_present_frame.store(frame, std::memory_order_relaxed);
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain) {
        int finished_pair = -1;
        {
            std::lock_guard lock(seen_mutex);
            if (camera_pair_target >= 0 && frame >= camera_pair_armed_after + 2) {
                finished_pair = camera_pair_target;
                camera_pair_target = -1;
                for (auto& sample : camera_pair_depth) {
                    if (sample.view) sample.view->Release();
                    sample.view = nullptr;
                }
            }
        }
        if (finished_pair >= 0) {
            wchar_t message[128];
            swprintf_s(message, L"EDPE: adjacent camera probe finished DSV #%d at Present %llu",
                finished_pair, frame);
            EdpeLog(message);
        }
    }
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain &&
        snapshot_waiting.load(std::memory_order_acquire)) {
        bool expired = false;
        {
            std::lock_guard lock(seen_mutex);
            if (snapshot_active >= 0 && snapshot_wait_frames && --snapshot_wait_frames == 0) {
                snapshot_active = -1;
                snapshot_waiting.store(false, std::memory_order_release);
                expired = true;
            }
        }
        if (expired) EdpeLog(L"EDPE: depth snapshot request expired without target bind");
    }
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain &&
        sequence_active.exchange(false, std::memory_order_acq_rel)) {
        std::array<int, 64> sequence{};
        size_t stored = 0;
        unsigned transitions = 0;
        {
            std::lock_guard lock(seen_mutex);
            sequence = bind_sequence;
            stored = sequence_stored;
            transitions = sequence_transitions;
        }
        wchar_t message[128];
        swprintf_s(message, L"EDPE: DSV bind sequence frame=%llu transitions=%u stored=%zu",
            frame, transitions, stored);
        EdpeLog(message);
        for (size_t i = 0; i < stored; ++i) {
            swprintf_s(message, L"EDPE: DSV bind sequence %zu target=%d", i, sequence[i]);
            EdpeLog(message);
        }
    }
    if (!attempted) {
        attempted = true;
        ID3D11Device* device = nullptr;
        if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device),
                reinterpret_cast<void**>(&device)))) return;
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        device->Release();
        if (!context) return;
        void** table = *reinterpret_cast<void***>(context);
        auto forward = reinterpret_cast<OMSetRenderTargetsFn>(table[kOMSetRenderTargets]);
        if (!forward) {
            context->Release();
            return;
        }
        original.store(forward, std::memory_order_release);
        observed_context.store(context, std::memory_order_release);
        if (patchSlot(table, kOMSetRenderTargets, reinterpret_cast<void*>(forward),
                reinterpret_cast<void*>(&observedOMSetRenderTargets))) {
            patched_table = table;
            observed_swap_chain = swap_chain;
            DXGI_SWAP_CHAIN_DESC sc_init{};
            if (SUCCEEDED(swap_chain->GetDesc(&sc_init))) {
                s_swapchain_width.store(sc_init.BufferDesc.Width, std::memory_order_relaxed);
                s_swapchain_height.store(sc_init.BufferDesc.Height, std::memory_order_relaxed);
            }
            EdpeLog(L"EDPE: OMSetRenderTargets DSV census armed");

            auto map_forward = reinterpret_cast<MapFn>(table[kMap]);
            auto unmap_forward = reinterpret_cast<UnmapFn>(table[kUnmap]);
            if (map_forward) original_map.store(map_forward, std::memory_order_release);
            if (unmap_forward) original_unmap.store(unmap_forward, std::memory_order_release);

            bool map_ok = map_forward && patchSlot(table, kMap,
                reinterpret_cast<void*>(map_forward), reinterpret_cast<void*>(&observedMap));
            bool unmap_ok = unmap_forward && patchSlot(table, kUnmap,
                reinterpret_cast<void*>(unmap_forward), reinterpret_cast<void*>(&observedUnmap));
            map_hook_active.store(map_ok, std::memory_order_release);
            unmap_hook_active.store(unmap_ok, std::memory_order_release);
            wchar_t hook_msg[128];
            swprintf_s(hook_msg, L"EDPE: Map/Unmap hooks armed map=%u unmap=%u", map_ok, unmap_ok);
            EdpeLog(hook_msg);
        } else {
            observed_context.store(nullptr, std::memory_order_release);
            context->Release();
            EdpeLog(L"EDPE: OMSetRenderTargets DSV census unavailable");
        }
    }
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain) {
        rearmContextHooks();
        auto* context = observed_context.load(std::memory_order_acquire);
        if (context) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer;
            if (SUCCEEDED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(backbuffer.GetAddressOf())))) {
                s_swapchain_backbuffer.store(backbuffer.Get(), std::memory_order_relaxed);
                const bool debug_view_active = s_jitter_debug_view.load(std::memory_order_relaxed);
                if (edpe::UpscalerManager::instance().isReconstructionActive() || debug_view_active) {
                    tryTemporalUpscale(context, rendered_frame, backbuffer.Get());
                }
                if (debug_view_active) {
                    renderJitterDebugView(context, rendered_frame, backbuffer.Get());
                }
            }
        }
        s_upscale_evaluated_this_frame = false;
        motion_color_first_exit_logged = false;
        s_latest_unmapped_camera_valid = false;
        s_jitter_injected_frame = ~0ull;
        clearTrackedMappedBuffers();
        static unsigned s_watchdog_last_jitter_writes = 0;
        static unsigned s_watchdog_no_jitter_frames = 0;
        const unsigned cur_writes = jittered_writes_count.load(std::memory_order_relaxed);
        if (cur_writes == s_watchdog_last_jitter_writes) {
            if (scene_map_target.load(std::memory_order_relaxed) != nullptr) {
                if (++s_watchdog_no_jitter_frames >= 120) {
                    scene_map_target.store(nullptr, std::memory_order_release);
                    s_watchdog_no_jitter_frames = 0;
                }
            }
        } else {
            s_watchdog_last_jitter_writes = cur_writes;
            s_watchdog_no_jitter_frames = 0;
        }
    }
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain) {
        UINT outW = 0, outH = 0;
        DXGI_SWAP_CHAIN_DESC sc_desc{};
        if (SUCCEEDED(swap_chain->GetDesc(&sc_desc))) {
            outW = sc_desc.BufferDesc.Width;
            outH = sc_desc.BufferDesc.Height;
            s_swapchain_width.store(outW, std::memory_order_relaxed);
            s_swapchain_height.store(outH, std::memory_order_relaxed);
        }
        const UINT sceneW = s_observed_scene_width.load(std::memory_order_relaxed);
        const UINT sceneH = s_observed_scene_height.load(std::memory_order_relaxed);
        active_render_width = (sceneW > 0 && (!outW || sceneW <= outW)) ? sceneW : outW;
        active_render_height = (sceneH > 0 && (!outH || sceneH <= outH)) ? sceneH : outH;
        if (active_render_width && active_render_height) {
            std::lock_guard lock(jitter_mutex);
            const unsigned base_phases = s_jitter_phase_count.load(std::memory_order_relaxed);
            unsigned phases = base_phases;
            if (outW > active_render_width && active_render_width > 0) {
                const float upscaleRatio = static_cast<float>(outW) / static_cast<float>(active_render_width);
                phases = static_cast<unsigned>(std::clamp(
                    std::lround(static_cast<float>(base_phases) * upscaleRatio * upscaleRatio), 8l, 256l));
            }
            s_active_jitter_phases.store(phases ? phases : 32, std::memory_order_relaxed);
            auto jitter = edpe::projectionJitter(frame, active_render_width, active_render_height, phases);
            const float scale = s_jitter_scale.load(std::memory_order_relaxed);
            jitter.pixelX *= scale;
            jitter.pixelY *= scale;
            jitter.ndcX *= scale;
            jitter.ndcY *= scale;
            active_projection_jitter = jitter;
            jitter_frame_index.store(frame, std::memory_order_relaxed);
        }

        if (!edpe::UpscalerManager::instance().isReconstructionActive() &&
            !s_jitter_debug_view.load(std::memory_order_relaxed)) {
            static LARGE_INTEGER s_last_present_qpc{};
            static LARGE_INTEGER s_present_freq{};
            if (s_present_freq.QuadPart == 0) QueryPerformanceFrequency(&s_present_freq);
            LARGE_INTEGER now_qpc;
            QueryPerformanceCounter(&now_qpc);
            float dt_ms = 16.67f;
            if (s_last_present_qpc.QuadPart > 0 && s_present_freq.QuadPart > 0) {
                dt_ms = static_cast<float>((now_qpc.QuadPart - s_last_present_qpc.QuadPart) * 1000.0 / s_present_freq.QuadPart);
            }
            s_last_present_qpc = now_qpc;

            static unsigned s_last_writes = 0;
            const unsigned cur_writes = jittered_writes_count.load(std::memory_order_relaxed);
            const unsigned writes_this_frame = cur_writes - s_last_writes;
            s_last_writes = cur_writes;

            const unsigned base_phases = s_jitter_phase_count.load(std::memory_order_relaxed);
            edpe::JitterFrameLog jlog{};
            jlog.frame = frame;
            jlog.deltaMs = dt_ms;
            jlog.phase = static_cast<unsigned>(frame % (base_phases ? base_phases : 8));
            jlog.totalPhases = base_phases ? base_phases : 8;
            jlog.pixelX = s_injected_jitter.pixelX;
            jlog.pixelY = s_injected_jitter.pixelY;
            jlog.ndcX = s_injected_jitter.ndcX;
            jlog.ndcY = s_injected_jitter.ndcY;
            jlog.injectedWrites = writes_this_frame;
            jlog.rejections = jitter_rejection_count.load(std::memory_order_relaxed);
            jlog.evalJx = 0.0f;
            jlog.evalJy = 0.0f;
            jlog.syncOk = (s_jitter_injected_frame == frame);
            jlog.cameraValid = (s_camera_now.scaleX > 0.0f);
            jlog.heldJitter = false;
            jlog.sceneCut = false;
            edpe::JitterLogger::instance().pushFrame(jlog);
        }
    }
    if (frame % 1024 == 0 && observed_swap_chain == swap_chain &&
        !edpe::UpscalerManager::instance().isReconstructionActive()) {
        size_t distinct = 0;
        std::array<unsigned long long, 32> counts{};
        {
            std::lock_guard lock(seen_mutex);
            distinct = seen_count;
            for (size_t i = 0; i < distinct; ++i) {
                counts[i] = seen[i].interval_binds;
                seen[i].interval_binds = 0;
            }
        }
        wchar_t message[144];
        swprintf_s(message, L"EDPE: DSV census frame=%llu binds=%llu unique=%zu slotActive=%u",
            frame, dsv_binds.load(std::memory_order_relaxed), distinct,
            patched_table && patched_table[kOMSetRenderTargets] ==
                reinterpret_cast<void*>(&observedOMSetRenderTargets));
        EdpeLog(message);
        int busiest[4]{-1, -1, -1, -1};
        unsigned long long binds[4]{};
        for (size_t rank = 0; rank < 4; ++rank) {
            for (size_t i = 0; i < distinct; ++i) {
                if (counts[i] > binds[rank]) {
                    binds[rank] = counts[i];
                    busiest[rank] = static_cast<int>(i);
                }
            }
            if (busiest[rank] >= 0) counts[busiest[rank]] = 0;
        }
        swprintf_s(message, L"EDPE: DSV interval frame=%llu top=%d:%llu,%d:%llu,%d:%llu,%d:%llu",
            frame, busiest[0], binds[0], busiest[1], binds[1],
            busiest[2], binds[2], busiest[3], binds[3]);
        EdpeLog(message);
    }
    edpe::Profiler::instance().onFrameEnd();
}

void ContextCensusAfterOverlay(IDXGISwapChain* swap_chain, UINT flags) {
    if ((flags & DXGI_PRESENT_TEST) || !patched_table || observed_swap_chain != swap_chain) return;
    const bool arm_sequence = sequence_requested.exchange(false, std::memory_order_acq_rel);
    int armed_pair = -1;
    {
        std::lock_guard lock(seen_mutex);
        if (arm_sequence) {
            sequence_stored = 0;
            sequence_transitions = 0;
            sequence_last = -2;
            sequence_active.store(true, std::memory_order_release);
        }
        if (snapshot_queued >= 0) {
            snapshot_active = snapshot_queued;
            snapshot_queued = -1;
            snapshot_wait_frames = 120;
            snapshot_waiting.store(true, std::memory_order_release);
            snapshot_armed_after = last_present_frame.load(std::memory_order_relaxed);
            snapshot_first_bind_after = snapshot_last_bind_after = 0;
            snapshot_bind_count = 0;
        }
        if (camera_pair_queued >= 0) {
            camera_pair_target = camera_pair_queued;
            camera_pair_queued = -1;
            camera_pair_armed_after = last_present_frame.load(std::memory_order_relaxed);
            camera_pair_bind_frame = ~0ull;
            camera_pair_bind_count = 0;
            motion_pair_active = motion_pair_queued;
            motion_pair_queued = false;
            motion_camera_frames = {~0ull, ~0ull};
            gpu_camera_pair = {};
            motion_color_source.Reset();
            motion_color_view.Reset();
            motion_color_snapshot.Reset();
            motion_early_color_view.Reset();
            motion_early_color_snapshot.Reset();
            motion_depth_snapshot.Reset();
            motion_clean_color_target.Reset();
            motion_early_color_from_hud = false;
            motion_color_frame = ~0ull;
            motion_color_bound = motion_color_rebound =
                motion_color_first_exit_logged = false;
            motion_depth_frame = ~0ull;
            motion_depth.Reset();
            motion_depth_view.Reset();
            scene_depth_source.Reset();
            armed_pair = camera_pair_target;
        }
    }
    if (arm_sequence) EdpeLog(L"EDPE: one-frame DSV bind sequence armed after overlay");
    if (armed_pair >= 0) {
        wchar_t message[128];
        swprintf_s(message, L"EDPE: adjacent camera probe armed DSV #%d after Present %llu",
            armed_pair, camera_pair_armed_after);
        EdpeLog(message);
    }
}

void ContextCensusAfterPresent(IDXGISwapChain* swap_chain, UINT flags) {
    s_in_edpe_present.store(false, std::memory_order_release);
    if ((flags & DXGI_PRESENT_TEST) || observed_swap_chain != swap_chain || !patched_table) return;
    rearmContextHooks();
}

bool ContextCensusRequestBindSequence() {
    if (!ContextCensusBindSequenceAvailable()) return false;
    bool expected = false;
    return sequence_requested.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
}

bool ContextCensusBindSequenceAvailable() {
    return patched_table && !sequence_active.load(std::memory_order_acquire) &&
        !sequence_requested.load(std::memory_order_acquire);
}

namespace {
bool depthSnapshotAvailableLocked(unsigned index) {
    if (!patched_table || index >= seen_count || snapshot_queued >= 0 ||
        snapshot_active >= 0 || snapshot_view || camera_pair_queued >= 0 ||
        camera_pair_target >= 0 || camera_pair_depth[0].staging ||
        camera_pair_depth[1].staging || motion_pair_active ||
        motion_grid_readback) return false;
    const auto& candidate = seen[index];
    return candidate.samples == 1 &&
        (candidate.texture_format == DXGI_FORMAT_R32G8X24_TYPELESS ||
         candidate.texture_format == DXGI_FORMAT_R24G8_TYPELESS);
}
}

bool ContextCensusRequestDepthSnapshot(unsigned index) {
    std::lock_guard lock(seen_mutex);
    if (!depthSnapshotAvailableLocked(index)) return false;
    snapshot_queued = static_cast<int>(index);
    return true;
}

bool ContextCensusDepthSnapshotAvailable(unsigned index) {
    std::lock_guard lock(seen_mutex);
    return depthSnapshotAvailableLocked(index);
}

bool ContextCensusRequestCameraPair(unsigned index) {
    std::lock_guard lock(seen_mutex);
    if (!depthSnapshotAvailableLocked(index)) return false;
    camera_pair_queued = static_cast<int>(index);
    return true;
}

bool ContextCensusRequestMotionPair(unsigned index) {
    std::lock_guard lock(seen_mutex);
    if (!depthSnapshotAvailableLocked(index) ||
        seen[index].texture_format != DXGI_FORMAT_R32G8X24_TYPELESS) return false;
    camera_pair_queued = static_cast<int>(index);
    motion_pair_queued = true;
    return true;
}

ID3D11ShaderResourceView* ContextCensusTakeMotionSnapshot(UINT* width, UINT* height) {
    if (!motion_snapshot) return nullptr;
    if (width) *width = motion_snapshot_width;
    if (height) *height = motion_snapshot_height;
    return motion_snapshot.Detach();
}

ID3D11ShaderResourceView* ContextCensusTakeMotionColorSnapshot(UINT* width, UINT* height) {
    if (!motion_color_snapshot) return nullptr;
    if (width) *width = motion_color_snapshot_width;
    if (height) *height = motion_color_snapshot_height;
    return motion_color_snapshot.Detach();
}

ID3D11ShaderResourceView* ContextCensusTakeMotionEarlyColorSnapshot(UINT* width, UINT* height) {
    if (!motion_early_color_snapshot) return nullptr;
    if (width) *width = motion_early_color_width;
    if (height) *height = motion_early_color_height;
    return motion_early_color_snapshot.Detach();
}

ID3D11ShaderResourceView* ContextCensusTakeMotionDepthSnapshot(UINT* width, UINT* height, unsigned* index) {
    if (!motion_depth_snapshot) return nullptr;
    if (width) *width = motion_depth_snapshot_width;
    if (height) *height = motion_depth_snapshot_height;
    if (index) *index = motion_depth_snapshot_index;
    return motion_depth_snapshot.Detach();
}

int ContextCensusSceneDepthCandidate() {
    std::lock_guard lock(seen_mutex);
    const auto frame = last_present_frame.load(std::memory_order_relaxed);
    int candidate = -1;
    for (size_t index = 0; index < seen_count; ++index) {
        const auto match = seen[index].scene_match_frame;
        // Present can run between scene signature binds; keep a recent match stable.
        if (match == ~0ull || match > frame || frame - match > 32 ||
            !depthSnapshotAvailableLocked(static_cast<unsigned>(index))) continue;
        if (candidate >= 0) return -1; // Ambiguous: require manual selection.
        candidate = static_cast<int>(index);
    }
    return candidate;
}

ID3D11DepthStencilView* ContextCensusTakeDepthSnapshot(unsigned* index,
    ID3D11RenderTargetView** color_view) {
    if (color_view) *color_view = nullptr;
    ID3D11DepthStencilView* view = nullptr;
    ID3D11RenderTargetView* color = nullptr;
    unsigned long long armed = 0, first = 0, last = 0;
    unsigned binds = 0, selected = 0;
    {
        std::lock_guard lock(seen_mutex);
        view = snapshot_view;
        if (view) {
            selected = static_cast<unsigned>(snapshot_index);
            armed = snapshot_armed_after;
            first = snapshot_first_bind_after;
            last = snapshot_last_bind_after;
            binds = snapshot_bind_count;
        }
        snapshot_view = nullptr;
        snapshot_index = -1;
        color = snapshot_color_view;
        snapshot_color_view = nullptr;
    }
    if (view) {
        if (index) *index = selected;
        if (color_view) *color_view = color;
        else if (color) color->Release();
        wchar_t message[192];
        swprintf_s(message,
            L"EDPE: depth snapshot timing #%u armedAfter=%llu firstBindAfter=%llu lastBindAfter=%llu binds=%u handedAt=%llu",
            selected, armed, first, last, binds,
            last_present_frame.load(std::memory_order_relaxed));
        EdpeLog(message);
    } else if (color) {
        color->Release();
    }
    return view;
}

extern "C" BOOL WINAPI EdpeRequestDepthSnapshot(UINT index) {
    return ContextCensusRequestDepthSnapshot(index);
}

extern "C" BOOL WINAPI EdpeRequestCameraPair(UINT index) {
    return ContextCensusRequestCameraPair(index);
}

extern "C" BOOL WINAPI EdpeRequestMotionPair(UINT index) {
    return ContextCensusRequestMotionPair(index);
}

extern "C" int WINAPI EdpeSceneDepthCandidate() {
    return ContextCensusSceneDepthCandidate();
}

extern "C" BOOL WINAPI EdpeRequestBindSequence() {
    return ContextCensusRequestBindSequence();
}

void ContextCensusOnSwapChainRelease(IUnknown* object) {
    if (object != observed_swap_chain) return;
    {
        std::lock_guard lock(seen_mutex);
        snapshot_queued = -1;
        snapshot_active = -1;
        snapshot_wait_frames = 0;
        snapshot_waiting.store(false, std::memory_order_release);
        if (snapshot_view) snapshot_view->Release();
        snapshot_view = nullptr;
        if (snapshot_color_view) snapshot_color_view->Release();
        snapshot_color_view = nullptr;
        snapshot_index = -1;
        snapshot_armed_after = snapshot_first_bind_after = snapshot_last_bind_after = 0;
        snapshot_bind_count = 0;
        camera_pair_queued = -1;
        camera_pair_target = -1;
        motion_pair_queued = motion_pair_active = false;
        motion_hook_watch = 0;
        motion_depth.Reset();
        motion_depth_view.Reset();
        scene_depth_source.Reset();
        scene_ldr_source.Reset();
        scene_ldr_frame = ~0ull;
        scene_hdr_color_source.Reset();
        scene_hdr_color_frame = ~0ull;
        s_dlaa_input.Reset();
        s_dlaa_input_srv.Reset();
        s_last_temporal_inputs = {};
        s_last_temporal_inputs_frame = ~0ull;
        s_in_scene_camera_latched_frame = ~0ull;
        s_hdr_slot0_seen_frame = ~0ull;
        s_consecutive_skipped_frames = 0;
        s_auxiliary_rejection_streak = 0;
        s_fallback_unmapped_camera_frame = ~0ull;
        s_gbuffer_resources = {};
        s_observed_scene_width.store(0, std::memory_order_relaxed);
        s_observed_scene_height.store(0, std::memory_order_relaxed);
        s_observed_scene_frame = ~0ull;
        s_swapchain_backbuffer.store(nullptr, std::memory_order_relaxed);
        motion_color_source.Reset();
        motion_color_view.Reset();
        motion_color_snapshot.Reset();
        motion_early_color_view.Reset();
        motion_early_color_snapshot.Reset();
        motion_depth_snapshot.Reset();
        motion_clean_color_target.Reset();
        motion_color_frame = ~0ull;
        motion_color_bound = motion_color_rebound =
            motion_color_first_exit_logged = false;
        motion_grid_readback.Reset();
        motion_snapshot.Reset();
        motion_pass.reset();
        motion_camera_frames = {~0ull, ~0ull};
        gpu_camera_pair = {};
        motion_depth_frame = ~0ull;
        camera_pair_bind_frame = ~0ull;
        camera_pair_bind_count = 0;
        for (auto& sample : camera_pair_depth) {
            if (sample.view) sample.view->Release();
            if (sample.staging) sample.staging->Release();
            sample = {};
        }
    }
    sequence_active.store(false, std::memory_order_release);
    sequence_requested.store(false, std::memory_order_release);
    for (auto& sample : constant_buffer_samples) {
        if (sample.buffer) sample.buffer->Release();
        sample.buffer = nullptr;
        sample.ready.store(false, std::memory_order_release);
        sample.claimed.store(false, std::memory_order_release);
    }
    {
        std::lock_guard lock(seen_mutex);
        for (auto& sample : pipeline_samples) {
            if (sample.active) observed_context.load(std::memory_order_acquire)->End(sample.query);
            if (sample.query) sample.query->Release();
            sample = {};
        }
    }
    endVSBufferProbe();
    resetCameraDelta();
    if (patched_table) {
        if (map_hook_active.exchange(false, std::memory_order_acq_rel)) {
            patchSlot(patched_table, kMap,
                reinterpret_cast<void*>(&observedMap),
                reinterpret_cast<void*>(original_map.load(std::memory_order_acquire)));
        }
        if (unmap_hook_active.exchange(false, std::memory_order_acq_rel)) {
            patchSlot(patched_table, kUnmap,
                reinterpret_cast<void*>(&observedUnmap),
                reinterpret_cast<void*>(original_unmap.load(std::memory_order_acquire)));
        }
        patchSlot(patched_table, kOMSetRenderTargets,
            reinterpret_cast<void*>(&observedOMSetRenderTargets),
            reinterpret_cast<void*>(original.load(std::memory_order_acquire)));
    }
    scene_map_target.store(nullptr, std::memory_order_release);
    scene_mapped_data.store(nullptr, std::memory_order_release);
    s_hud_probe_armed_once.store(false, std::memory_order_release);
    clearTrackedMappedBuffers();
    observed_context.exchange(nullptr, std::memory_order_acq_rel)->Release();
    patched_table = nullptr;
    observed_swap_chain = nullptr;
    last_present_frame.store(0, std::memory_order_relaxed);
}

void ContextCensusSetJitterEnabled(bool enabled) {
    jitter_enabled.store(enabled, std::memory_order_relaxed);
}

bool ContextCensusGetJitterEnabled() {
    return jitter_enabled.load(std::memory_order_relaxed);
}

void ContextCensusSetJitterTestOverride(bool override) {
    jitter_test_override.store(override, std::memory_order_relaxed);
}

bool ContextCensusGetJitterTestOverride() {
    return jitter_test_override.load(std::memory_order_relaxed);
}

bool ContextCensusIsJitterActive() {
    if (!jitter_enabled.load(std::memory_order_relaxed)) return false;
    if (jitter_test_override.load(std::memory_order_relaxed)) return true;
    if (s_jitter_debug_view.load(std::memory_order_relaxed)) return true;
    return edpe::UpscalerManager::instance().isReconstructionActive();
}

void ContextCensusGetActiveJitter(float* pixelX, float* pixelY, float* ndcX, float* ndcY,
    unsigned long long* frameIndex) {
    std::lock_guard lock(jitter_mutex);
    if (pixelX) *pixelX = active_projection_jitter.pixelX;
    if (pixelY) *pixelY = active_projection_jitter.pixelY;
    if (ndcX) *ndcX = active_projection_jitter.ndcX;
    if (ndcY) *ndcY = active_projection_jitter.ndcY;
    if (frameIndex) *frameIndex = jitter_frame_index.load(std::memory_order_relaxed);
}

unsigned ContextCensusGetJitteredWritesCount() {
    return jittered_writes_count.load(std::memory_order_relaxed);
}

unsigned ContextCensusGetJitterRejectionsCount() {
    return jitter_rejection_count.load(std::memory_order_relaxed);
}

extern "C" void WINAPI EdpeSetJitterEnabled(BOOL enabled) {
    ContextCensusSetJitterEnabled(enabled != FALSE);
}

extern "C" BOOL WINAPI EdpeGetJitterEnabled() {
    return ContextCensusGetJitterEnabled() ? TRUE : FALSE;
}

extern "C" void WINAPI EdpeSetJitterTestOverride(BOOL override) {
    ContextCensusSetJitterTestOverride(override != FALSE);
}

extern "C" BOOL WINAPI EdpeGetJitterTestOverride() {
    return ContextCensusGetJitterTestOverride() ? TRUE : FALSE;
}

extern "C" BOOL WINAPI EdpeIsJitterActive() {
    return ContextCensusIsJitterActive() ? TRUE : FALSE;
}

extern "C" void WINAPI EdpeGetActiveJitter(float* pixelX, float* pixelY, float* ndcX, float* ndcY,
    unsigned long long* frameIndex) {
    ContextCensusGetActiveJitter(pixelX, pixelY, ndcX, ndcY, frameIndex);
}

void ContextCensusSetJitterInvertX(bool invert) {
    s_jitter_invert_x.store(invert, std::memory_order_relaxed);
    edpe::Config::instance().jitterInvertX = invert;
}
bool ContextCensusGetJitterInvertX() {
    return s_jitter_invert_x.load(std::memory_order_relaxed);
}
void ContextCensusSetJitterInvertY(bool invert) {
    s_jitter_invert_y.store(invert, std::memory_order_relaxed);
    edpe::Config::instance().jitterInvertY = invert;
}
bool ContextCensusGetJitterInvertY() {
    return s_jitter_invert_y.load(std::memory_order_relaxed);
}
void ContextCensusSetJitterSwapXY(bool swap) {
    s_jitter_swap_xy.store(swap, std::memory_order_relaxed);
    edpe::Config::instance().jitterSwapXY = swap;
}
bool ContextCensusGetJitterSwapXY() {
    return s_jitter_swap_xy.load(std::memory_order_relaxed);
}
void ContextCensusSetJitterScale(float scale) {
    s_jitter_scale.store(scale, std::memory_order_relaxed);
    edpe::Config::instance().jitterScale = scale;
}
float ContextCensusGetJitterScale() {
    return s_jitter_scale.load(std::memory_order_relaxed);
}
void ContextCensusSetJitterPhaseCount(unsigned count) {
    s_jitter_phase_count.store(count, std::memory_order_relaxed);
    edpe::Config::instance().jitterPhaseCount = count;
}
unsigned ContextCensusGetJitterPhaseCount() {
    return s_jitter_phase_count.load(std::memory_order_relaxed);
}

void ContextCensusSetPreserveNativeUi(bool preserve) {
    s_preserve_native_ui.store(preserve, std::memory_order_relaxed);
    edpe::Config::instance().preserveNativeUi = preserve;
}
bool ContextCensusGetPreserveNativeUi() {
    return s_preserve_native_ui.load(std::memory_order_relaxed);
}

void ContextCensusSetZeroMotion(bool zero) {
    s_zero_motion_vectors.store(zero, std::memory_order_relaxed);
    edpe::Config::instance().zeroMotionVectors = zero;
}
bool ContextCensusGetZeroMotion() {
    return s_zero_motion_vectors.load(std::memory_order_relaxed);
}
void ContextCensusSetMotionInvertX(bool invert) {
    s_motion_invert_x.store(invert, std::memory_order_relaxed);
    edpe::Config::instance().motionInvertX = invert;
}
bool ContextCensusGetMotionInvertX() {
    return s_motion_invert_x.load(std::memory_order_relaxed);
}
void ContextCensusSetMotionInvertY(bool invert) {
    s_motion_invert_y.store(invert, std::memory_order_relaxed);
    edpe::Config::instance().motionInvertY = invert;
}
bool ContextCensusGetMotionInvertY() {
    return s_motion_invert_y.load(std::memory_order_relaxed);
}
void ContextCensusSetMotionScale(float scale) {
    s_motion_scale.store(scale, std::memory_order_relaxed);
    edpe::Config::instance().motionScale = scale;
}
float ContextCensusGetMotionScale() {
    return s_motion_scale.load(std::memory_order_relaxed);
}

void ContextCensusGetSceneRenderSize(UINT* outWidth, UINT* outHeight) {
    if (outWidth) *outWidth = s_observed_scene_width.load(std::memory_order_relaxed);
    if (outHeight) *outHeight = s_observed_scene_height.load(std::memory_order_relaxed);
}

void ContextCensusRequestComparisonCapture() {
    s_comparison_capture_requested.store(true, std::memory_order_release);
}

bool ContextCensusIsComparisonCapturePending() {
    return s_comparison_capture_requested.load(std::memory_order_acquire);
}

bool ContextCensusConsumeComparisonCaptureRequest() {
    return s_comparison_capture_requested.exchange(false, std::memory_order_acq_rel);
}

ID3D11Texture2D* ContextCensusGetBackbufferCopyTexture() {
    return s_backbuffer_copy.Get();
}

ID3D11RenderTargetView* ContextCensusGetBackbufferCopyRtv() {
    return s_backbuffer_copy_rtv.Get();
}

void ContextCensusSetJitterDebugView(bool enable) {
    s_jitter_debug_view.store(enable, std::memory_order_relaxed);
    edpe::Config::instance().jitterDebugView = enable;
}

bool ContextCensusGetJitterDebugView() {
    return s_jitter_debug_view.load(std::memory_order_relaxed);
}

void ContextCensusToggleJitterDebugView() {
    const bool current = s_jitter_debug_view.load(std::memory_order_relaxed);
    ContextCensusSetJitterDebugView(!current);
}

void ContextCensusSetJitterDebugSensitivity(float sensitivity) {
    s_jitter_debug_sensitivity.store(sensitivity, std::memory_order_relaxed);
    edpe::Config::instance().jitterDebugSensitivity = sensitivity;
}

float ContextCensusGetJitterDebugSensitivity() {
    return s_jitter_debug_sensitivity.load(std::memory_order_relaxed);
}

void ContextCensusSetJitterDebugThreshold(float threshold) {
    s_jitter_debug_threshold.store(threshold, std::memory_order_relaxed);
    edpe::Config::instance().jitterDebugThreshold = threshold;
}

float ContextCensusGetJitterDebugThreshold() {
    return s_jitter_debug_threshold.load(std::memory_order_relaxed);
}

void ContextCensusSetJitterDebugMode(int mode) {
    s_jitter_debug_mode.store(mode, std::memory_order_relaxed);
    edpe::Config::instance().jitterDebugMode = mode;
}

int ContextCensusGetJitterDebugMode() {
    return s_jitter_debug_mode.load(std::memory_order_relaxed);
}

