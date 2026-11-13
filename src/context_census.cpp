#include <chrono>
#include "context_census.h"
#include "crash_logger.h"
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
#include "hud_debug_pass.h"
#include "jitter_view_recorder.h"
#include "profiler.h"
#include "d3d11_proxy.h"
#include "vtable_hook.h"

#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
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
constexpr size_t kOMSetRenderTargetsAndUnorderedAccessViews = 34;
constexpr size_t kExecuteCommandList = 58;
constexpr size_t kFinishCommandList = 114;
constexpr unsigned kPairFirstSample = 100;
constexpr unsigned kPairSecondSample = 101;
constexpr unsigned kAlternateProjectionSample = 102;
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using OMSetRenderTargetsAndUnorderedAccessViewsFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
    UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using ExecuteCommandListFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using FinishCommandListFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
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
void STDMETHODCALLTYPE observedOMSetRenderTargetsAndUnorderedAccessViews(
    ID3D11DeviceContext* context, UINT numRTVs, ID3D11RenderTargetView* const* targets,
    ID3D11DepthStencilView* dsv, UINT uavStartSlot, UINT numUAVs,
    ID3D11UnorderedAccessView* const* uavs, const UINT* initialCounts);
void STDMETHODCALLTYPE observedExecuteCommandList(
    ID3D11DeviceContext* context, ID3D11CommandList* pCommandList, BOOL restoreContextState);
HRESULT STDMETHODCALLTYPE observedFinishCommandList(
    ID3D11DeviceContext* context, BOOL restoreDeferredContextState, ID3D11CommandList** ppCommandList);

static std::atomic<unsigned long long> s_rendered_scene_frame_id{0};

std::atomic<OMSetRenderTargetsFn> original{nullptr};
std::atomic<OMSetRenderTargetsFn> original_deferred_om{nullptr};
OMSetRenderTargetsFn resolveOriginalOM(ID3D11DeviceContext* context);
MapFn resolveOriginalMap(ID3D11DeviceContext* context);
UnmapFn resolveOriginalUnmap(ID3D11DeviceContext* context);
VSSetConstantBuffersFn resolveOriginalVSSetConstantBuffers(ID3D11DeviceContext* context);
DrawIndexedFn resolveOriginalDrawIndexed(ID3D11DeviceContext* context);
DrawFn resolveOriginalDraw(ID3D11DeviceContext* context);
DrawIndexedInstancedFn resolveOriginalDrawIndexedInstanced(ID3D11DeviceContext* context);
DrawInstancedFn resolveOriginalDrawInstanced(ID3D11DeviceContext* context);
std::atomic<OMSetRenderTargetsAndUnorderedAccessViewsFn> original_om_uav{nullptr};
std::atomic<OMSetRenderTargetsAndUnorderedAccessViewsFn> original_deferred_om_uav{nullptr};
std::atomic<ExecuteCommandListFn> original_execute_command_list{nullptr};
std::atomic<ExecuteCommandListFn> original_deferred_execute_command_list{nullptr};
std::atomic<FinishCommandListFn> original_finish_command_list{nullptr};
std::atomic<FinishCommandListFn> original_deferred_finish_command_list{nullptr};

std::atomic<unsigned long long> s_count_om_set_render_targets{0};
std::atomic<unsigned long long> s_count_om_set_rt_uav{0};
std::atomic<unsigned long long> s_count_finish_command_list{0};
std::atomic<unsigned long long> s_count_execute_command_list{0};

static std::atomic<bool> s_log_om_imm{false}, s_log_om_def{false};
static std::atomic<bool> s_log_om_uav_imm{false}, s_log_om_uav_def{false};
static std::atomic<bool> s_log_finish_imm{false}, s_log_finish_def{false};
static std::atomic<bool> s_log_exec_imm{false}, s_log_exec_def{false};

static void logContextHookOnce(ID3D11DeviceContext* context, size_t slot, const wchar_t* hookName,
    std::atomic<bool>& immFlag, std::atomic<bool>& defFlag) {
    if (!context) return;
    const auto type = context->GetType();
    std::atomic<bool>& flag = (type == D3D11_DEVICE_CONTEXT_IMMEDIATE) ? immFlag : defFlag;
    if (!flag.exchange(true, std::memory_order_acq_rel)) {
        const char* typeStr = (type == D3D11_DEVICE_CONTEXT_IMMEDIATE) ? "immediate" : "deferred";
        void** table = *reinterpret_cast<void***>(context);
        void* slotAddr = table ? &table[slot] : nullptr;
        wchar_t msg[256];
        swprintf_s(msg, L"EDPE: hook %ls this=%p type=%hs slotAddr=%p",
            hookName, context, typeStr, slotAddr);
        EdpeLog(msg);
    }
}
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
struct KnownDeviceInfo {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* immContext = nullptr;
};
static std::vector<KnownDeviceInfo> s_known_devices;
static std::mutex s_device_registry_mutex;
static std::atomic<ID3D11Device*> s_present_device{nullptr};
static std::atomic<ID3D11Device*> s_scene_device{nullptr};
static std::atomic<uint32_t> s_present_device_logs{0};

struct HookedTableEntry {
    void** table = nullptr;
    void** original_table = nullptr;
    ID3D11DeviceContext* context = nullptr;
    bool is_immediate = false;
    OMSetRenderTargetsFn orig_om = nullptr;
    OMSetRenderTargetsAndUnorderedAccessViewsFn orig_om_uav = nullptr;
    ExecuteCommandListFn orig_exec = nullptr;
    FinishCommandListFn orig_finish = nullptr;
    MapFn orig_map = nullptr;
    UnmapFn orig_unmap = nullptr;
    VSSetConstantBuffersFn orig_vs_cb = nullptr;
    DrawIndexedFn orig_draw_indexed = nullptr;
    DrawFn orig_draw = nullptr;
    DrawIndexedInstancedFn orig_draw_indexed_instanced = nullptr;
    DrawInstancedFn orig_draw_instanced = nullptr;
};
static constexpr size_t kMaxHookedTables = 16;
static std::atomic<size_t> s_hooked_table_count{0};
static HookedTableEntry s_hooked_tables[kMaxHookedTables]{};
static std::array<std::unique_ptr<edpe::VTableHook>, kMaxHookedTables> s_context_vtable_hooks{};
static std::mutex s_hooked_tables_mutex;

static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_engine_velocity_texture;
static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_engine_velocity_srv;
static unsigned long long s_engine_velocity_frame = ~0ull;
static unsigned long long s_map_shader_seen_frame = ~0ull;
static float s_last_jittered_forward_block[16]{};
static float s_last_unjittered_forward_block[16]{};
static float s_last_unjittered_inv_rays[12]{};
static bool s_has_reference_cb0_blocks = false;
static Microsoft::WRL::ComPtr<ID3D11Buffer> s_deferred_cb2_staging;
static Microsoft::WRL::ComPtr<ID3D11Buffer> s_deferred_cb2_override;
static constexpr size_t kDofRingSize = 8;
static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_dof_ring_tex[kDofRingSize]{};
static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_dof_ring_srv[kDofRingSize]{};
static UINT s_dof_ring_w = 0, s_dof_ring_h = 0;
static size_t s_dof_ring_idx = 0;

struct SeenDepthView {
    ID3D11DepthStencilView* view = nullptr; // Identity only; never dereferenced later.
    DXGI_FORMAT texture_format = DXGI_FORMAT_UNKNOWN;
    UINT samples = 0;
    bool color_logged = false;
    unsigned long long interval_binds = 0;
    ID3D11RenderTargetView* mrt_hdr_view = nullptr; // Weak; compared within one frame only.
    unsigned long long mrt_frame = 0;
    unsigned long long scene_match_frame = ~0ull;
    UINT width = 0;
    UINT height = 0;
    ID3D11Device* owning_device = nullptr;
};

struct ClonedContextVtable {
    ID3D11DeviceContext* context = nullptr;
    void** original_table = nullptr;
    OMSetRenderTargetsFn orig_om = nullptr;
    OMSetRenderTargetsAndUnorderedAccessViewsFn orig_om_uav = nullptr;
    MapFn orig_map = nullptr;
    UnmapFn orig_unmap = nullptr;
    void* methods[134]{};
};
static constexpr size_t kMaxClonedContexts = 16;
static std::array<std::unique_ptr<ClonedContextVtable>, kMaxClonedContexts> s_cloned_contexts{};
static std::atomic<size_t> s_cloned_context_count{0};
static std::mutex s_cloned_contexts_mutex;

struct PerContextData {
    ID3D11DeviceContext* context = nullptr;
    ID3D11Device* owningDevice = nullptr;
    std::atomic<unsigned long long> omCalls{0};
    std::atomic<unsigned long long> gameOmCalls{0};
    std::atomic<unsigned long long> edpeOmCalls{0};
    std::atomic<unsigned long long> omUavCalls{0};
    std::atomic<unsigned long long> gameOmUavCalls{0};
    std::atomic<unsigned long long> edpeOmUavCalls{0};
    std::atomic<unsigned long long> nonNullDsvCalls{0};
    std::atomic<unsigned long long> gameNonNullDsvCalls{0};
    std::atomic<unsigned long long> edpeNonNullDsvCalls{0};
    std::atomic<UINT> maxDsvWidth{0};
    std::atomic<UINT> maxDsvHeight{0};
    std::array<SeenDepthView, 32> seen{};
    size_t seen_count = 0;
    std::atomic<unsigned long long> dsv_binds{0};
    std::array<int, 64> bind_sequence{};
    size_t sequence_stored = 0;
    unsigned sequence_transitions = 0;
    int sequence_last = -2;
};
static constexpr size_t kMaxTrackedContexts = 32;
static std::array<std::unique_ptr<PerContextData>, kMaxTrackedContexts> s_tracked_contexts{};
static std::atomic<size_t> s_tracked_context_count{0};
static std::mutex s_tracked_contexts_mutex;
static std::atomic<ID3D11DeviceContext*> s_last_warned_scene_ctx{nullptr};

PerContextData* getOrCreateContextData(ID3D11DeviceContext* context) {
    if (!context) return nullptr;
    const size_t fastCount = s_tracked_context_count.load(std::memory_order_acquire);
    if (fastCount == 1 && context == observed_context.load(std::memory_order_relaxed) &&
        s_tracked_contexts[0] && s_tracked_contexts[0]->context == context) {
        return s_tracked_contexts[0].get();
    }
    ID3D11Device* curDevice = nullptr;
    context->GetDevice(&curDevice);
    if (curDevice) curDevice->Release();

    const size_t count = s_tracked_context_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        if (s_tracked_contexts[i] &&
            s_tracked_contexts[i]->context == context &&
            s_tracked_contexts[i]->owningDevice == curDevice) {
            return s_tracked_contexts[i].get();
        }
    }
    std::lock_guard lock(s_tracked_contexts_mutex);
    const size_t cur_count = s_tracked_context_count.load(std::memory_order_relaxed);
    for (size_t i = 0; i < cur_count; ++i) {
        if (s_tracked_contexts[i] &&
            s_tracked_contexts[i]->context == context &&
            s_tracked_contexts[i]->owningDevice == curDevice) {
            return s_tracked_contexts[i].get();
        }
    }
    if (cur_count >= kMaxTrackedContexts) return nullptr;

    auto data = std::make_unique<PerContextData>();
    data->context = context;
    data->owningDevice = curDevice;

    PerContextData* ptr = data.get();
    s_tracked_contexts[cur_count] = std::move(data);
    s_tracked_context_count.store(cur_count + 1, std::memory_order_release);
    return ptr;
}

PerContextData* findSceneContextData(bool logWarning = true) {
    auto* presentCtx = observed_context.load(std::memory_order_acquire);
    PerContextData* presentData = presentCtx ? getOrCreateContextData(presentCtx) : nullptr;
    PerContextData* best = presentData;
    unsigned long long bestDsvCalls = presentData ? presentData->nonNullDsvCalls.load(std::memory_order_relaxed) : 0;

    const size_t count = s_tracked_context_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        auto* data = s_tracked_contexts[i].get();
        if (!data) continue;
        const unsigned long long dsvCalls = data->nonNullDsvCalls.load(std::memory_order_relaxed);
        if (dsvCalls > bestDsvCalls) {
            bestDsvCalls = dsvCalls;
            best = data;
        }
    }

    if (logWarning && best && presentCtx && best->context != presentCtx && bestDsvCalls > 0) {
        ID3D11Device* presentDev = presentData ? presentData->owningDevice : s_present_device.load(std::memory_order_acquire);
        if (s_last_warned_scene_ctx.exchange(best->context, std::memory_order_acq_rel) != best->context) {
            wchar_t warnMsg[320];
            swprintf_s(warnMsg,
                L"EDPE: WARNING: scene context differs from present context (scene ctx=%p device=%p, present ctx=%p device=%p, dsvCalls=%llu vs %llu)",
                best->context, best->owningDevice, presentCtx, presentDev,
                bestDsvCalls,
                presentData ? presentData->nonNullDsvCalls.load(std::memory_order_relaxed) : 0ull);
            EdpeLog(warnMsg);
        }
    }
    return best;
}

ID3D11DeviceContext* activeSceneContext() {
    if (PerContextData* scene = findSceneContextData(false)) {
        if (scene->context && scene->nonNullDsvCalls.load(std::memory_order_relaxed) > 0) {
            return scene->context;
        }
    }
    return observed_context.load(std::memory_order_acquire);
}

bool isObservedImmediateContext(ID3D11DeviceContext* context) {
    if (!context) return false;
    if (context == observed_context.load(std::memory_order_acquire)) return true;
    const size_t count = s_tracked_context_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        if (s_tracked_contexts[i] && s_tracked_contexts[i]->context == context) {
            return context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
        }
    }
    return context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
}

extern std::atomic<UINT> s_observed_scene_width;
extern std::atomic<UINT> s_observed_scene_height;
extern std::atomic<UINT> s_swapchain_width;
extern std::atomic<UINT> s_swapchain_height;

void updateContextMaxDsv(PerContextData* data, ID3D11DepthStencilView* dsv, ID3D11RenderTargetView* rtv0 = nullptr) {
    if (!data || !dsv || !rtv0) return;
    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    if (!res) return;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(tex.GetAddressOf())))) {
        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS && desc.Format != DXGI_FORMAT_R24G8_TYPELESS) {
            res->Release();
            return;
        }
        const UINT scW = s_swapchain_width.load(std::memory_order_relaxed);
        const UINT scH = s_swapchain_height.load(std::memory_order_relaxed);
        if (scW > scH && desc.Width == desc.Height) {
            res->Release();
            return;
        }
        UINT curMaxW = data->maxDsvWidth.load(std::memory_order_relaxed);
        UINT curMaxH = data->maxDsvHeight.load(std::memory_order_relaxed);
        if (desc.Width > curMaxW || (desc.Width == curMaxW && desc.Height > curMaxH)) {
            data->maxDsvWidth.store(desc.Width, std::memory_order_relaxed);
            data->maxDsvHeight.store(desc.Height, std::memory_order_relaxed);
        }
    }
    res->Release();
}

std::mutex seen_mutex;
std::atomic<unsigned long long> dsv_binds{0};
std::atomic<bool> sequence_requested{false};
std::atomic<bool> sequence_active{false};
ID3D11DeviceContext* snapshot_target_context = nullptr;
ID3D11DeviceContext* camera_pair_context = nullptr;
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
std::array<unsigned long long, 2> camera_pair_sample_frames{~0ull, ~0ull};
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
    ID3D11DeviceContext* context = nullptr;
    unsigned long long frame = 0;
};
constexpr size_t kCameraDeltaRing = 4;
std::array<CameraDeltaSlot, kCameraDeltaRing> camera_delta_ring{};
size_t camera_delta_head = 0;  // next slot to read
size_t camera_delta_count = 0; // slots holding queued copies
Microsoft::WRL::ComPtr<ID3D11Buffer> camera_delta_source; // copied at the next OMSetRenderTargets
ID3D11DeviceContext* camera_delta_source_context = nullptr;
unsigned long long camera_delta_source_frame = 0;
unsigned long long camera_delta_taken_frame = ~0ull;
unsigned camera_delta_dropped = 0;  // ring full or map failure, reset each summary
bool camera_delta_disabled = false; // staging creation failed; log stays off
bool camera_delta_announced = false;
edpe::CameraDeltaTracker camera_delta_tracker;
static edpe::CameraProjection s_camera_previous{};
static edpe::CameraProjection s_camera_now{};
static edpe::Vec3 s_last_valid_velocity{0.0f, 0.0f, 0.0f};
static edpe::Vec3 s_prev_valid_velocity{0.0f, 0.0f, 0.0f};
static bool s_camera_reset = false;
static unsigned long long s_camera_frame = ~0ull;
static unsigned long long s_camera_previous_frame = ~0ull;
static unsigned long long s_hdr_slot0_seen_frame = ~0ull;
static unsigned s_consecutive_skipped_frames = 0;

// Authoritative Camera Anchor state (Steps 2-4)
static edpe::CameraProjection s_anchor_camera{};
static bool s_anchor_valid = false;
static unsigned long long s_anchor_frame = ~0ull;
static bool s_probe_seen_this_frame = false;
static unsigned s_spike_reject_streak = 0;
static unsigned s_consecutive_fallback_frames = 0;
static unsigned s_unreconstructed_jitter_streak = 0;

// Staging buffer for current frame camera candidate (Two-Phase Staging & Commit)
static edpe::CameraProjection s_staged_camera{};
static unsigned long long s_staged_camera_frame = ~0ull;
static bool s_has_staged_camera = false;
static bool s_staged_from_in_scene = false;
static bool s_last_committed_scene_cut = false;
static unsigned s_teleport_rejection_streak = 0;
static unsigned long long s_last_teleport_rejection_frame = ~0ull;
static unsigned s_cut_rejection_streak = 0;
static unsigned long long s_last_cut_rejection_frame = ~0ull;
struct UnmapFrameStats {
    unsigned total = 0, noMapped = 0, parseFail = 0, gbufDone = 0, probe = 0;
    unsigned filterFail = 0, stageDenied = 0, accepted = 0, jitterOff = 0;
    unsigned acceptedIdx = 0, nearLate = 0, firstNearLateIdx = 0, gbufStartIdx = 0;
    unsigned failReason[12]{};
    unsigned blockOnly = 0;
    unsigned firstFailIdx = 0;
    int firstFailReason = 0;
    bool haveFirstOk = false, haveFirstFail = false;
    float firstOkWords[28]{};
    float firstFailWords[28]{};
    unsigned long long mapsAtReset = 0;
    float lastRDist = 0.0f, lastRTrace = 0.0f;
};
static UnmapFrameStats s_ustat{};
static std::atomic<uint64_t> s_valid_camera_seen_counter{0};

// Frame timing for kinematic 2nd-order prediction
static float s_last_frame_dt = 0.01667f;
static float s_current_frame_dt = 0.01667f;

static std::atomic<bool> s_inside_main_scene{false};
static std::atomic<bool> s_in_edpe_present{false};
static edpe::ProjectionJitter s_injected_jitter{};
static unsigned long long s_jitter_injected_frame = ~0ull;
static std::atomic<unsigned long long> s_jitter_standdown_until{0};
static unsigned long long s_jitter_standdown_len = 60;
static edpe::ProjectionJitter s_last_rendered_jitter{};
static float s_prev_eval_jitter_x = 0.0f;
static float s_prev_eval_jitter_y = 0.0f;

inline bool isSceneCameraAspect(float scaleX, float scaleY, UINT width, UINT height) {
    if (scaleX <= 0.0f || scaleY <= 0.0f) return false;
    const float camAspect = scaleY / scaleX;
    if (width > height && std::fabs(camAspect - 1.0f) < 0.03f) return false; // Square cameras are cubemaps/RTTs on widescreen displays
    if (!width || !height) return true;
    const float screenAspect = static_cast<float>(width) / static_cast<float>(height);
    return std::fabs(camAspect - screenAspect) < 0.05f;
}

inline float getJitterLogTimeMs() {
    static LARGE_INTEGER s_base{};
    static LARGE_INTEGER s_freq{};
    if (s_freq.QuadPart == 0) {
        QueryPerformanceFrequency(&s_freq);
        QueryPerformanceCounter(&s_base);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (s_freq.QuadPart > 0)
        ? static_cast<float>(static_cast<double>(now.QuadPart - s_base.QuadPart) * 1000.0 / static_cast<double>(s_freq.QuadPart))
        : 0.0f;
}

using edpe::cameraRotationDeterminant;
using edpe::cameraRotationTrace;
using edpe::isReflectionOrAuxiliaryCamera;
using edpe::isStereoEyeOrIpdProbe;
using edpe::isRearOrAuxiliaryProbe;

inline bool isAuthoritativeMainSceneCamera(const float* words, size_t wordCount,
                                           const edpe::CameraProjection& parsed_camera,
                                           UINT outW, UINT outH,
                                           const edpe::CameraProjection& committed_camera,
                                           const edpe::Vec3& last_velocity,
                                           float dtRatio) {
    if (!words || wordCount != 5376 / sizeof(float)) return false;
    if (parsed_camera.scaleX <= 0.0f || parsed_camera.scaleY <= 0.0f) return false;

    // 1. Strict Near Plane (depthB) range filter (0.01m .. 0.25m):
    // In Cobra Odyssey, reversed-Z perspective depth is rawDepth = depthB / viewZ.
    // The player's main eye camera consistently uses near plane depthB around 0.025m (2.5 cm, range 0.01m..0.25m).
    if (parsed_camera.depthB < 0.01f || parsed_camera.depthB > 0.25f) {
        return false;
    }

    // 2. Strict perspective vs orthographic check:
    const double w0 = words[1083], w1 = words[1087], w2 = words[1091];
    const double wLenSq = w0 * w0 + w1 * w1 + w2 * w2;
    if (wLenSq < 0.5 || wLenSq > 1.5) {
        return false;
    }

    // 3. Viewport / resolution filtering from words 1328..1331:
    const float vpW = words[1328];
    const float vpH = words[1329];
    if (std::isfinite(vpW) && std::isfinite(vpH) && vpW > 0.0f && vpH > 0.0f) {
        if (outW > outH && std::fabs(vpW - vpH) < 1.0f) {
            return false;
        }
        if (vpW < 64.0f || vpH < 64.0f) {
            return false;
        }
    }

    // 4. Camera aspect ratio validation:
    const float camAspect = parsed_camera.scaleY / parsed_camera.scaleX;
    if (outW > outH && std::fabs(camAspect - 1.0f) < 0.03f) {
        return false;
    }
    if (outW > 0 && outH > 0) {
        const float screenAspect = static_cast<float>(outW) / static_cast<float>(outH);
        if (std::fabs(camAspect - screenAspect) > 0.05f) {
            return false;
        }
    }

    // 5. Coordinate chirality / orthonormality check (rejects planar reflections det < 0.85 or det > 1.15):
    const float det = cameraRotationDeterminant(parsed_camera);
    if (det < 0.85f || det > 1.15f) {
        return false;
    }

    // 6. Повний фільтр допоміжних камер та стерео-зондів відносно зафіксованої камери:
    if (isReflectionOrAuxiliaryCamera(parsed_camera, committed_camera, last_velocity, dtRatio)) {
        return false;
    }

    return true;
}

// Single-point camera commitment for the frame (Two-Phase Staging & Commit)
inline void commitMainCamera(unsigned long long rendered_frame, bool& out_scene_cut) {
    if (s_camera_frame == rendered_frame) {
        out_scene_cut = s_last_committed_scene_cut;
        return;
    }

    out_scene_cut = false;
    if (s_has_staged_camera && s_staged_camera_frame == rendered_frame) {
        float dtRatio = 1.0f;
        if (s_last_frame_dt > 1e-5f && s_current_frame_dt > 1e-5f) {
            dtRatio = s_current_frame_dt / s_last_frame_dt;
        }

        if (s_camera_now.scaleX > 0.0f) {
            const float rotTrace = edpe::cameraRotationTrace(s_staged_camera, s_camera_now);
            const bool fovChanged =
                std::fabs(s_camera_now.scaleX - s_staged_camera.scaleX) > 0.01f * s_camera_now.scaleX;

            s_prev_valid_velocity = s_last_valid_velocity;
            if (rotTrace < 2.414f || fovChanged) {
                out_scene_cut = true;
                s_last_valid_velocity = {0.0f, 0.0f, 0.0f};
            } else {
                out_scene_cut = false;
                const edpe::Vec3 newDelta =
                    edpe::cameraPosition(s_staged_camera) - edpe::cameraPosition(s_camera_now);
                const edpe::Vec3 right = edpe::cameraRight(s_staged_camera);
                const edpe::Vec3 up    = edpe::cameraUp(s_staged_camera);
                const edpe::Vec3 fwd   = edpe::cameraForward(s_staged_camera);

                const float dLen   = edpe::length(newDelta);
                const float dRight = std::fabs(edpe::dot(newDelta, right));
                const float dUp    = std::fabs(edpe::dot(newDelta, up));
                const float dFwd   = std::fabs(edpe::dot(newDelta, fwd));

                const bool isLateralSpike = (dRight >= 0.80f * dLen && dUp < 0.045f && dFwd < 0.045f);
                const bool isCockpitHeadSpike = (dRight < 0.025f && dUp >= 0.65f * dLen && dFwd >= 0.45f * dLen);

                const bool looksLikeSuddenSpike =
                    (rotTrace > 2.992f && dLen >= 0.032f && dLen <= 0.36f &&
                     (isLateralSpike || isCockpitHeadSpike));

                if (looksLikeSuddenSpike && s_anchor_valid && (rendered_frame <= s_anchor_frame + 2) &&
                    !edpe::matchesAnchorPrediction(s_staged_camera, s_anchor_camera, s_last_valid_velocity, dtRatio)) {
                    if (++s_spike_reject_streak < 6) {
                        // Відхиляємо staged камеру - утримуємо попередню валідну камеру з екстраполяцією:
                        s_has_staged_camera = false;
                        s_camera_previous = s_camera_now;
                        s_camera_previous_frame = s_camera_frame;
                        const edpe::Vec3 extPos = edpe::cameraPosition(s_camera_now) + s_last_valid_velocity;
                        edpe::cameraSetPosition(s_camera_now, extPos);
                        s_camera_frame = rendered_frame;
                        out_scene_cut = false;
                        s_last_committed_scene_cut = out_scene_cut;
                        return;
                    }
                }

                s_last_valid_velocity = newDelta;
            }
            s_camera_previous = s_camera_now;
            s_camera_previous_frame = s_camera_frame;
        } else {
            s_camera_previous = s_staged_camera;
            s_camera_previous_frame = rendered_frame;
            out_scene_cut = true;
            s_prev_valid_velocity = {0.0f, 0.0f, 0.0f};
            s_last_valid_velocity = {0.0f, 0.0f, 0.0f};
        }

        s_consecutive_fallback_frames = 0;
        s_spike_reject_streak = 0;
        s_cut_rejection_streak = 0;
        s_teleport_rejection_streak = 0;
        s_anchor_camera = s_staged_camera;
        s_anchor_valid = true;
        s_anchor_frame = rendered_frame;

        s_camera_now = s_staged_camera;
        s_camera_frame = rendered_frame;
        s_has_staged_camera = false;
        s_staged_from_in_scene = false;
    } else if (s_camera_now.scaleX > 0.0f) {
        // Fallback: жоден пас не оновив камеру — плавна екстраполяція без скидання TAA/DLAA
        if (++s_consecutive_fallback_frames > 2) {
            s_last_valid_velocity = {0.0f, 0.0f, 0.0f};
            s_anchor_valid = false;
        }
        s_prev_valid_velocity = s_last_valid_velocity;
        s_camera_previous = s_camera_now;
        s_camera_previous_frame = s_camera_frame;
        const edpe::Vec3 extPos = edpe::cameraPosition(s_camera_now) + s_last_valid_velocity;
        edpe::cameraSetPosition(s_camera_now, extPos);
        s_camera_frame = rendered_frame;
        out_scene_cut = false;
    }
    s_last_committed_scene_cut = out_scene_cut;
}
Microsoft::WRL::ComPtr<ID3D11Texture2D> scene_depth_source;
Microsoft::WRL::ComPtr<ID3D11Texture2D> scene_ldr_source;
unsigned long long scene_ldr_frame = ~0ull;
Microsoft::WRL::ComPtr<ID3D11Texture2D> s_recent_scene_ldr_source;
unsigned long long s_recent_scene_ldr_frame = ~0ull;
Microsoft::WRL::ComPtr<ID3D11Texture2D> scene_hdr_color_source;
unsigned long long scene_hdr_color_frame = ~0ull;
Microsoft::WRL::ComPtr<ID3D11Texture2D> s_last_full_hdr_post_source;
unsigned long long s_last_full_hdr_post_frame = ~0ull;
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
Microsoft::WRL::ComPtr<ID3D11Texture2D> s_motion_prev_depth;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_motion_prev_depth_view;
bool s_has_motion_prev_depth = false;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> motion_color_source;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motion_color_view;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motion_color_snapshot;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motion_early_color_view;
static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_clean_hdr_scene_texture;
static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_clean_hdr_scene_srv;
static Microsoft::WRL::ComPtr<ID3D11RenderTargetView> s_clean_hdr_scene_rtv;
static UINT s_clean_hdr_scene_width = 0;
static UINT s_clean_hdr_scene_height = 0;
static unsigned long long s_clean_hdr_scene_frame = ~0ull;
static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_hud_merged_hdr_texture;
static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_hud_merged_hdr_srv;
static Microsoft::WRL::ComPtr<ID3D11RenderTargetView> s_hud_merged_hdr_rtv;
static UINT s_hud_merged_hdr_width = 0;
static UINT s_hud_merged_hdr_height = 0;
static DXGI_FORMAT s_hud_merged_hdr_format = DXGI_FORMAT_UNKNOWN;
static ID3D11Resource* s_hud_merged_hdr_source_res = nullptr;
static unsigned long long s_hud_merged_hdr_frame = ~0ull;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motion_early_color_snapshot;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motion_depth_snapshot;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_hud_mask_snapshot;
UINT s_hud_mask_snapshot_width = 0;
UINT s_hud_mask_snapshot_height = 0;
bool s_hud_mask_snapshot_queued = false;
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
bool s_first_hud_draw_logged = false;
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
ID3D11DeviceContext* motion_grid_context = nullptr;
std::unique_ptr<edpe::MotionPass> motion_pass;
std::unique_ptr<edpe::UiCompositePass> ui_composite_pass;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motion_snapshot;
UINT motion_snapshot_width = 0;
UINT motion_snapshot_height = 0;
static ID3D11Device* s_hud_mask_device = nullptr;
Microsoft::WRL::ComPtr<ID3D11Texture2D> s_hud_mask_texture;
Microsoft::WRL::ComPtr<ID3D11Texture2D> s_hud_mask_prev_texture;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> s_hud_mask_rtv;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_hud_mask_srv;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_hud_mask_prev_srv;
Microsoft::WRL::ComPtr<ID3D11Texture2D> s_hud_tonemapped_texture;
Microsoft::WRL::ComPtr<ID3D11Texture2D> s_hud_tonemapped_prev_texture;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> s_hud_tonemapped_rtv;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_hud_tonemapped_srv;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_hud_tonemapped_prev_srv;
Microsoft::WRL::ComPtr<ID3D11Texture2D> s_zero_hdr_texture;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_zero_hdr_srv;
Microsoft::WRL::ComPtr<ID3D11Texture2D> s_far_linear_depth_texture;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> s_far_linear_depth_rtv;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_far_linear_depth_srv;
bool s_far_linear_depth_cleared = false;
Microsoft::WRL::ComPtr<ID3D11Texture2D> s_hud_depth_stencil_texture;
Microsoft::WRL::ComPtr<ID3D11DepthStencilView> s_hud_depth_stencil_dsv;
DXGI_FORMAT s_hud_depth_stencil_tex_fmt = DXGI_FORMAT_UNKNOWN;
UINT s_hud_depth_stencil_width = 0;
UINT s_hud_depth_stencil_height = 0;
bool s_hud_depth_stencil_seeded = false;
bool s_hud_depth_only_seeded = false;
ID3D11Texture2D* s_seed_cached_depth_tex = nullptr;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_seed_cached_depth_srv;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_seed_cached_stencil_srv;
Microsoft::WRL::ComPtr<ID3D11PixelShader> s_hud_depth_seed_ps;
Microsoft::WRL::ComPtr<ID3D11PixelShader> s_hud_stencil_seed_ps[8];
Microsoft::WRL::ComPtr<ID3D11DepthStencilState> s_hud_seed_ds_write_depth;
Microsoft::WRL::ComPtr<ID3D11DepthStencilState> s_hud_seed_ds_write_stencil[8];
struct HudRoDsCacheEntry {
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> orig;
    bool forceDepthGe = false;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> ro;
};
static std::array<HudRoDsCacheEntry, 16> s_hud_ro_ds_cache{};
Microsoft::WRL::ComPtr<ID3D11VertexShader> s_hud_hdr_merge_vs;
Microsoft::WRL::ComPtr<ID3D11PixelShader> s_hud_hdr_merge_ps;
Microsoft::WRL::ComPtr<ID3D11SamplerState> s_hud_hdr_merge_sampler;
Microsoft::WRL::ComPtr<ID3D11BlendState> s_hud_hdr_merge_blend;
Microsoft::WRL::ComPtr<ID3D11BlendState> s_hud_mask_blend_add;
Microsoft::WRL::ComPtr<ID3D11BlendState> s_hud_mask_blend_scaled_add;
Microsoft::WRL::ComPtr<ID3D11BlendState> s_hud_mask_blend_alpha;
Microsoft::WRL::ComPtr<ID3D11BlendState> s_hud_mask_blend_premul;
Microsoft::WRL::ComPtr<ID3D11BlendState> s_hud_mask_blend_opaque;
Microsoft::WRL::ComPtr<ID3D11BlendState> s_hud_mask_blend_darken;
Microsoft::WRL::ComPtr<ID3D11BlendState> s_hud_mask_blend_colorless;
Microsoft::WRL::ComPtr<ID3D11BlendState> s_hud_tone_rgb_only_blend;
Microsoft::WRL::ComPtr<ID3D11BlendState> s_hud_pass54_tone_atten_blend;
Microsoft::WRL::ComPtr<ID3D11BlendState> s_hud_pass54_mul_wash_blend;
Microsoft::WRL::ComPtr<ID3D11DepthStencilState> s_hud_mask_ds_state;
Microsoft::WRL::ComPtr<ID3D11DepthStencilState> s_hud_mask_ds_state_no_depth;
Microsoft::WRL::ComPtr<ID3D11Texture2D> s_hud_earlyz_texture;
Microsoft::WRL::ComPtr<ID3D11DepthStencilView> s_hud_earlyz_dsv;
Microsoft::WRL::ComPtr<ID3D11DepthStencilState> s_hud_earlyz_write_ds;
Microsoft::WRL::ComPtr<ID3D11DepthStencilState> s_hud_earlyz_test_ds;
static unsigned long long s_hud_earlyz_valid_frame = ~0ull;
UINT s_hud_mask_width = 0;
UINT s_hud_mask_height = 0;
bool s_hud_mask_cleared_for_frame = false;
bool s_hud_mask_active_pass = false;
unsigned long long s_hud_mask_active_frame = ~0ull;
unsigned s_hud_draws_replayed_frame = 0;
unsigned long long s_hud_mask_last_valid_frame = ~0ull;
unsigned s_hud_mask_last_valid_draws = 0;
Microsoft::WRL::ComPtr<ID3D11Texture2D> s_hud_mask_staging;
UINT s_hud_mask_staging_width = 0;
UINT s_hud_mask_staging_height = 0;
unsigned long long s_hud_mask_coverage_pending_frame = ~0ull;
static Microsoft::WRL::ComPtr<ID3D11Buffer> s_clean_camera_cb;
static float s_last_clean_cb1_data[1344]{};
static bool s_has_clean_cb1_data = false;
static bool s_clean_camera_cb_dirty = false;
static unsigned long long s_clean_camera_frame = ~0ull;
static unsigned long long s_hud_mask_finalized_frame = ~0ull;
static std::atomic<unsigned> s_clean_camera_substitutions{0};
// Per-frame HUD diagnostics (reset at Present, logged by the "HUD diag" line).
static unsigned s_diag_hud_candidates = 0;   // draws classified as HudReplay by observeHudDraw
static unsigned s_diag_hud_replayed = 0;     // draws actually redirected into the HUD mask
static int s_diag_composite_path = 0;        // 0 = none, 1 = UI composite, 2 = composite disabled
static int s_diag_hud_src = 0;               // 0 = no mask, 1 = current mask, 2 = previous-frame mask
static UINT s_diag_hud_mask_w = 0;           // HUD mask width at composite time
static int s_diag_eval_stage = 0;            // 0 = not evaluated, 1 = beforePass54, 2 = Present
static unsigned s_diag_ldr_updates = 0;      // number of scene_ldr_source updates this frame
static unsigned s_diag_hud_unhook_events = 0;// number of times d3d11.dll overwrote Draw* hooks this frame
static unsigned s_diag_hud_declined_draws = 0;// number of HudReplay draws declined by replayHudDraw
static unsigned long long s_pass53_blit_seen_frame = ~0ull;
static bool s_upscale_evaluated_this_frame = false;
static bool s_hud_merged_into_hdr_this_frame = false;
static unsigned long long s_hud_merged_into_main_hdr_frame = ~0ull;
static unsigned long long s_verified_ldr_tonemap_frame = ~0ull;
static unsigned long long s_hud_tonemapped_valid_frame = ~0ull;
static unsigned long long s_hud_tonemapped_last_valid_frame = ~0ull;
static bool s_hud_tonemapped_alpha_dirty = true;
static unsigned long long s_hdr_blurred_in_place_frame = ~0ull;
static unsigned long long s_pause_menu_seen_frame = ~0ull;
static bool isPauseOrModalMenuActive(unsigned long long frame) {
    return s_pause_menu_seen_frame != ~0ull && frame <= s_pause_menu_seen_frame + 1;
}
static bool s_ldr_tonemap_candidate_bound = false;
static bool s_post_ldr_backbuffer_bound = false;
static bool s_post_ldr_backbuffer_no_dsv = false;
static unsigned s_post_ldr_backbuffer_ui_draws = 0;
static std::uint64_t s_current_hud_vs_hash = 0;
static std::uint64_t s_current_hud_ps_hash = 0;
struct HudPairFrameStat {
    std::uint64_t vs = 0;
    std::uint64_t ps = 0;
    unsigned candidates = 0;
    unsigned replayed = 0;
    unsigned stencil_only = 0;
    unsigned cb1_substituted = 0;
    unsigned declined = 0;
};
static constexpr size_t kMaxHudPairStats = 32;
static std::array<HudPairFrameStat, kMaxHudPairStats> s_hud_pair_stats_cur{};
static size_t s_hud_pair_stats_cur_count = 0;
static std::array<HudPairFrameStat, kMaxHudPairStats> s_hud_pair_stats_prev{};
static size_t s_hud_pair_stats_prev_count = 0;

static HudPairFrameStat* findOrAddHudPairStat(std::uint64_t vs, std::uint64_t ps) {
    for (size_t i = 0; i < s_hud_pair_stats_cur_count; ++i) {
        if (s_hud_pair_stats_cur[i].vs == vs && s_hud_pair_stats_cur[i].ps == ps) {
            return &s_hud_pair_stats_cur[i];
        }
    }
    if (s_hud_pair_stats_cur_count < kMaxHudPairStats) {
        auto& item = s_hud_pair_stats_cur[s_hud_pair_stats_cur_count++];
        item = {vs, ps, 0, 0, 0, 0, 0};
        return &item;
    }
    return nullptr;
}
static std::atomic<ID3D11Resource*> s_jittered_cb1_buffer{nullptr};
unsigned motion_grid_wait = 0;
unsigned motion_hook_watch = 0;
struct PairDepthSample {
    ID3D11DepthStencilView* view = nullptr;
    ID3D11Texture2D* staging = nullptr;
    ID3D11DeviceContext* context = nullptr;
    unsigned long long after_present = 0;
    UINT width = 0;
    UINT height = 0;
    unsigned wait = 0;
};
std::array<PairDepthSample, 2> camera_pair_depth{}; // One-shot diagnostic only.
struct ConstantBufferSample {
    ID3D11Buffer* buffer = nullptr;
    ID3D11DeviceContext* context = nullptr;
    unsigned bind = 0;
    unsigned long long after_present = 0;
    unsigned wait = 0;
    std::atomic<bool> claimed{false};
    std::atomic<bool> ready{false};
};
std::array<ConstantBufferSample, 9> constant_buffer_samples{}; // Snapshot, adjacent-frame, and alternate-projection probes.
struct PipelineSample {
    ID3D11Query* query = nullptr;
    ID3D11DeviceContext* context = nullptr;
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
std::atomic<float> s_sharpness{0.0f};

std::atomic<bool> s_jitter_debug_view{false};
std::atomic<float> s_jitter_debug_sensitivity{1.5f};
std::atomic<float> s_jitter_debug_threshold{0.02f};
std::atomic<int> s_jitter_debug_mode{0};

std::atomic<bool> s_hud_debug_view{false};
std::atomic<float> s_hud_debug_opacity{1.0f};
std::atomic<float> s_hud_debug_threshold{0.02f};
std::atomic<int> s_hud_debug_mode{0};
std::atomic<float> s_hud_mask_latest_coverage{0.0f};
std::atomic<unsigned> s_hud_mask_latest_draws{0};


std::atomic<bool> s_zero_motion_vectors{false};

static std::atomic<bool> s_scene_jitter_consumed{true};
static std::atomic<unsigned> s_scene_writes_since_eval{0};

edpe::ProjectionJitter getOrUpdateSceneProjectionJitter() {
    std::lock_guard lock(jitter_mutex);
    if (s_scene_jitter_consumed.exchange(false, std::memory_order_acq_rel)) {
        const unsigned long long scene_seq = s_rendered_scene_frame_id.fetch_add(1, std::memory_order_relaxed) + 1;

        const UINT scW = s_swapchain_width.load(std::memory_order_relaxed);
        const UINT scH = s_swapchain_height.load(std::memory_order_relaxed);
        const UINT sceneW = s_observed_scene_width.load(std::memory_order_relaxed);
        const UINT sceneH = s_observed_scene_height.load(std::memory_order_relaxed);
        const UINT rW = (sceneW > 0 && (!scW || sceneW <= scW)) ? sceneW : scW;
        const UINT rH = (sceneH > 0 && (!scH || sceneH <= scH)) ? sceneH : scH;

        const unsigned base_phases = s_jitter_phase_count.load(std::memory_order_relaxed);
        unsigned phases = base_phases ? base_phases : 32;
        if (scW > rW && rW > 0) {
            const float upscaleRatio = static_cast<float>(scW) / static_cast<float>(rW);
            phases = static_cast<unsigned>(std::clamp(
                std::lround(static_cast<float>(phases) * upscaleRatio * upscaleRatio), 8l, 256l));
        }
        s_active_jitter_phases.store(phases ? phases : 32, std::memory_order_relaxed);

        auto jitter = edpe::projectionJitter(scene_seq, rW ? rW : 2560, rH ? rH : 1440, phases ? phases : 32);
        const float scale = s_jitter_scale.load(std::memory_order_relaxed);
        jitter.pixelX *= scale;
        jitter.pixelY *= scale;
        jitter.ndcX *= scale;
        jitter.ndcY *= scale;
        active_projection_jitter = jitter;
        jitter_frame_index.store(scene_seq, std::memory_order_relaxed);
        s_scene_writes_since_eval.store(0, std::memory_order_relaxed);
    }
    return active_projection_jitter;
}
std::atomic<bool> s_motion_invert_x{false};
std::atomic<bool> s_motion_invert_y{false};
std::atomic<float> s_motion_scale{1.0f};
static bool s_config_loaded = false;

void queueConstantBufferSample(ID3D11DeviceContext* context, ID3D11Buffer* source,
    const D3D11_BUFFER_DESC& source_desc, unsigned bind_ordinal);
void captureEarlyMotionColor(ID3D11DeviceContext* context, bool from_hud);
void tryTemporalUpscale(ID3D11DeviceContext* context, unsigned long long rendered, ID3D11Texture2D* backbuffer);
void renderJitterDebugView(ID3D11DeviceContext* context, unsigned long long rendered, ID3D11Texture2D* backbuffer);
void renderHudDebugView(ID3D11DeviceContext* context, unsigned long long rendered, ID3D11Texture2D* backbuffer);
void rearmContextHooks();

static ID3D11RenderTargetView* s_cached_hdr_rtv0 = nullptr;

struct CachedShaderHashEntry {
    const void* shader = nullptr;
    std::uint64_t hash = 0;
};
static constexpr size_t kShaderHashCacheSize = 512;
static CachedShaderHashEntry s_vs_hash_cache[kShaderHashCacheSize]{};
static CachedShaderHashEntry s_ps_hash_cache[kShaderHashCacheSize]{};
static inline std::uint64_t lookupOrFetchShaderHash(ID3D11DeviceChild* shader,
    CachedShaderHashEntry (&cache)[kShaderHashCacheSize]) {
    if (!shader) return 0;
    const size_t slot = (reinterpret_cast<uintptr_t>(shader) >> 4) & (kShaderHashCacheSize - 1);
    if (cache[slot].shader == shader && cache[slot].hash != 0) {
        return cache[slot].hash;
    }
    std::uint64_t hash = 0;
    UINT sz = sizeof(hash);
    if (SUCCEEDED(shader->GetPrivateData(kEdpeShaderHashGuid, &sz, &hash)) && sz == sizeof(hash) && hash != 0) {
        cache[slot].shader = shader;
        cache[slot].hash = hash;
        return hash;
    }
    return 0;
}

static inline bool getFastBoundShaderHashes(ID3D11DeviceContext* context,
    std::uint64_t& outVsHash, std::uint64_t& outPsHash) {
    if (motion_pair_active) {
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
        context->VSGetShader(&vs, nullptr, nullptr);
        context->PSGetShader(&ps, nullptr, nullptr);
        outVsHash = 0;
        outPsHash = 0;
        if (!vs || !ps) return false;
        UINT sz = sizeof(outVsHash);
        if (FAILED(vs->GetPrivateData(kEdpeShaderHashGuid, &sz, &outVsHash)) || sz != sizeof(outVsHash)) return false;
        sz = sizeof(outPsHash);
        if (FAILED(ps->GetPrivateData(kEdpeShaderHashGuid, &sz, &outPsHash)) || sz != sizeof(outPsHash)) return false;
        return true;
    }
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    context->VSGetShader(&vs, nullptr, nullptr);
    context->PSGetShader(&ps, nullptr, nullptr);
    outVsHash = lookupOrFetchShaderHash(vs, s_vs_hash_cache);
    outPsHash = lookupOrFetchShaderHash(ps, s_ps_hash_cache);
    if (vs) vs->Release();
    if (ps) ps->Release();
    return outVsHash != 0 && outPsHash != 0;
}

bool sameRenderTargetResource(ID3D11RenderTargetView* view,
    ID3D11RenderTargetView* source) {
    if (!view || !source) return false;
    if (view == source) return true;
    if (source == motion_color_source.Get() && view == s_cached_hdr_rtv0) return true;
    ID3D11Resource* a = nullptr;
    view->GetResource(&a);
    if (!a) return false;
    if (source == motion_color_source.Get() && scene_hdr_color_source) {
        const bool match = (a == static_cast<ID3D11Resource*>(scene_hdr_color_source.Get()));
        a->Release();
        if (match) s_cached_hdr_rtv0 = view;
        return match;
    }
    ID3D11Resource* b = nullptr;
    source->GetResource(&b);
    if (!b) {
        a->Release();
        return false;
    }
    if (a == b) {
        b->Release();
        a->Release();
        return true;
    }
    Microsoft::WRL::ComPtr<IUnknown> identity_a, identity_b;
    const bool match = SUCCEEDED(a->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(identity_a.GetAddressOf()))) &&
                       SUCCEEDED(b->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(identity_b.GetAddressOf()))) &&
                       identity_a.Get() == identity_b.Get();
    b->Release();
    a->Release();
    return match;
}

// Diagnostic glass-state gate. Only a one-shot diagnostic
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

enum class HudDrawResult {
    None,
    GlassReplay,
    HudReplay
};

bool isHudShaderPair(std::uint64_t vs_hash, std::uint64_t ps_hash) {
    // Verified Cockpit HUD & HDR UI shader families.
    // Explicitly excludes non-HUD Pass 35 world shaders:
    // - 0xD1281DF454A153AD / 0x97DBC87FCAA429C4 (sun corona & radar glow; taking it pulls the real sun over the cockpit)
    // - 0x5559BD94B6852E83 / 0xB12F7A618E1BDE98 (3D depth-sampling target & ship holograms)
    // - 0x71DD8B8B09060A81 / 0x2D037A047171BF3B (world-space brackets)
    // - 0xB018D143700AB803 / 0x258B95AC99520C1F (null-output depth/stencil quad)
    // - 0x4EF6DDB075A927FA (main menu hangar / loading screen quad)
    if (vs_hash == 0x81216C77F90DEDD6ull || // Cockpit holo panels (stock lit A2965EC2, EDHM 16F88966, unlit B4786E0A, text 41B9D7B2, backing 329ACCE7/925ACEDA, icons B357E4E5, radar sweep C610FB6F, AB82734F)
        vs_hash == 0x2CECEC3065EF0D4Aull || // Cockpit HUD stencil setup (A4D03619), frosted HUD panel backing (5F5FA64F), and radar disc backing (1E87C9BF)
        vs_hash == 0x1989E6D3B405FDE0ull || // Cockpit holo panels (DisableGuiEffects variant PS EAB8A1C95A13FFBE)
        vs_hash == 0x925ACEDA0153AA5Bull || // Planetary / Orbital Cruise shadowless holo panels (kUiVsHoloNoShadow)
        vs_hash == 0xB7790CBFC6554097ull || // Flight HUD vector lines, altimeter ladder & pitch ladder (PS 8DEF46452FA459F5 / EDHM B1CA8D8EEF7C886D)
        vs_hash == 0xE508648660A352B2ull || // HUD sprite & target/distance/planetary label composite (kHudSprite, PS 499044B497EE6B2C / 63ABD86359B57D01)
        vs_hash == 0x5DA53D8B0133341Eull || // Target direction indicator quad (kTargetIndicator)
        vs_hash == 0xDF3503CD07F9B10Cull || // Radar disc & stalks hologram family (kHoloFamiliesTake)
        vs_hash == 0x5453D19B6D362364ull || // Radar stalks hologram family (kHoloFamiliesTake)
        vs_hash == 0x44DDCDEBAEA137D8ull || // Radar hologram family
        vs_hash == 0xA2C2D5510BF1926Dull || // Radar contact hologram family 1 (kHoloFamiliesTake)
        vs_hash == 0x9B34C331902DC1EDull || // Radar contact hologram family 2 (kHoloFamiliesTake)
        vs_hash == 0x9611A454527F7FEBull || // Radar contact hologram family 3 (kHoloFamiliesTake)
        vs_hash == 0xB932058F26B76691ull) { // Radar contact hologram family 4 (kHoloFamiliesTake)
        return true;
    }
    return (vs_hash == 0x5559BD94B6852E83ull &&                                      // Target & Own-Ship 3D schematic hologram billboards (kHoloTargetSphere)
            (ps_hash == 0xEA02FAC2BD6C643Cull || ps_hash == 0xE95634B0F61D218Full)) ||
           (vs_hash == 0xB12F7A618E1BDE98ull &&                                      // Ship/target hologram shells
            (ps_hash == 0x203DF51758AADC4Dull || ps_hash == 0x42AC0CACC9CDF72Bull)) ||
           (vs_hash == 0xF8D8A92E96419901ull && ps_hash == 0x16196F69ADE35E77ull) || // Hologram icon core (kHoloFamiliesTake)
           (vs_hash == 0x1989B75BF034ECC3ull && ps_hash == 0xDB79AE788E049DFDull) || // Radar contact triangles & elevation stalks
           (vs_hash == 0xC7FA0C0F5DD49180ull && ps_hash == 0x6EEF165A350DA30Full) || // Orbit / exclusion lines (kOrbitLines)
           (vs_hash == 0xA47A3315FFF5E2E4ull && ps_hash == 0x869FFF43E875906Eull) || // Supercruise speed/distance bars (kSupercruiseBars)
           (vs_hash == 0x9BFC7FD232328391ull && ps_hash == 0xDBF1725726018F52ull) || // Supercruise space dust particles (kSpaceDust)
           (vs_hash == 0xE904D334BC8B11EAull && ps_hash == 0x095030F27D2C362Aull) || // Normal-space dust particles (kNormalSpaceDust)
           (vs_hash == 0x1012E00B3CB44469ull && ps_hash == 0x666EF0C4C616F67Eull) || // HDR UI glyph atlas text / SRV turret HUD (kGuiText)
           (vs_hash == 0xA3E5D3FCBC1165F8ull && ps_hash == 0x019E81F4EECFB371ull) || // HDR UI BC7 icon pages / SRV turret HUD (kGuiIcons)
           (vs_hash == 0x1E8D013722FE71B8ull && ps_hash == 0x3D62E1CE0B0ECDF6ull) || // HDR UI solid/gradient widget primitives (kUiWidget)
           (vs_hash == 0x12382D2EA45E9632ull && ps_hash == 0x855C469156AB997Full) || // Galaxy/System Map & Orrery canvas (kMapCanvas)
           (vs_hash == 0xC31238331D8AC3D4ull &&                                      // Galaxy/System Map & Orrery sprites (kMapSprite)
            (ps_hash == 0xBD0FEB3276C8B2D7ull || ps_hash == 0x539A4858CE3A3477ull || ps_hash == 0x964221955A9E77E7ull));
}

bool isGlassShaderPair(std::uint64_t vs_hash, std::uint64_t ps_hash) {
    return (vs_hash == 0xF512712C40D93C12ull &&
            (ps_hash == 0x4A71EB0D34E9F2EFull || ps_hash == 0xAFED1D4B087E18A9ull)) ||
           (vs_hash == 0x8C091FFD08644E02ull && ps_hash == 0x70F56E2EB7032E1Aull);
}

inline constexpr char kHudHdrMergeShader[] = R"hlsl(
Texture2D<float4> tHud : register(t0);
Texture2D<float4> tScene : register(t1);
SamplerState sLinear : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 sampleHudFootprint(float2 uv) {
    uint hudW = 1, hudH = 1;
    tHud.GetDimensions(hudW, hudH);
    float2 hudSize = float2((float)hudW, (float)hudH);
    float2 duv = float2(abs(ddx(uv.x)), abs(ddy(uv.y)));
    float2 footprint = duv * hudSize;
    if (footprint.x <= 1.05 && footprint.y <= 1.05) {
        int2 maxPos = max(int2((int)hudW - 1, (int)hudH - 1), int2(0, 0));
        int2 p = clamp(int2(uv * hudSize), int2(0, 0), maxPos);
        return tHud.Load(int3(p, 0));
    }
    float2 offset = 0.25 * duv;
    float4 s00 = tHud.SampleLevel(sLinear, uv + float2(-offset.x, -offset.y), 0);
    float4 s10 = tHud.SampleLevel(sLinear, uv + float2( offset.x, -offset.y), 0);
    float4 s01 = tHud.SampleLevel(sLinear, uv + float2(-offset.x,  offset.y), 0);
    float4 s11 = tHud.SampleLevel(sLinear, uv + float2( offset.x,  offset.y), 0);
    return 0.25 * (s00 + s10 + s01 + s11);
}

float4 ps_main(VSOut pin) : SV_Target {
    int2 p = int2(pin.pos.xy);
    float4 h = sampleHudFootprint(pin.uv);
    float t = saturate(h.a);
    if (all(h.rgb <= 1e-5) && t >= 0.999 && (p.x > 1 || p.y > 1)) {
        discard;
    }
    float3 sceneHdr = max(tScene.SampleLevel(sLinear, pin.uv, 0).rgb, 0.0);
    return float4(max(h.rgb, 0.0) + sceneHdr * t, t);
}
)hlsl";

inline constexpr char kHudDepthStencilSeedShader[] = R"hlsl(
Texture2D<float4> tDepth : register(t0);
Texture2D<uint2> tStencil : register(t1);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float ps_depth(VSOut pin) : SV_Depth {
    uint w = 1, h = 1;
    tDepth.GetDimensions(w, h);
    int2 maxPos = max(int2((int)w - 1, (int)h - 1), int2(0, 0));
    int2 p = clamp(int2(pin.uv * float2((float)w, (float)h)), int2(0, 0), maxPos);
    return tDepth.Load(int3(p, 0)).r;
}

uint loadStencilAt(float2 uv) {
    uint w = 1, h = 1;
    tStencil.GetDimensions(w, h);
    int2 maxPos = max(int2((int)w - 1, (int)h - 1), int2(0, 0));
    int2 p = clamp(int2(uv * float2((float)w, (float)h)), int2(0, 0), maxPos);
    return tStencil.Load(int3(p, 0)).y;
}

void ps_s0(VSOut pin) { if ((loadStencilAt(pin.uv) & 0x01u) == 0u) discard; }
void ps_s1(VSOut pin) { if ((loadStencilAt(pin.uv) & 0x02u) == 0u) discard; }
void ps_s2(VSOut pin) { if ((loadStencilAt(pin.uv) & 0x04u) == 0u) discard; }
void ps_s3(VSOut pin) { if ((loadStencilAt(pin.uv) & 0x08u) == 0u) discard; }
void ps_s4(VSOut pin) { if ((loadStencilAt(pin.uv) & 0x10u) == 0u) discard; }
void ps_s5(VSOut pin) { if ((loadStencilAt(pin.uv) & 0x20u) == 0u) discard; }
void ps_s6(VSOut pin) { if ((loadStencilAt(pin.uv) & 0x40u) == 0u) discard; }
void ps_s7(VSOut pin) { if ((loadStencilAt(pin.uv) & 0x80u) == 0u) discard; }
)hlsl";

static void resetHudMaskResources(ID3D11Device* newDevice = nullptr) {
    s_clean_camera_cb.Reset();
    s_has_clean_cb1_data = false;
    s_clean_camera_cb_dirty = false;
    s_clean_camera_frame = ~0ull;
    s_hud_mask_finalized_frame = ~0ull;
    s_pass53_blit_seen_frame = ~0ull;
    s_last_full_hdr_post_source.Reset();
    s_last_full_hdr_post_frame = ~0ull;
    s_hud_merged_into_hdr_this_frame = false;
    s_hud_tonemapped_valid_frame = ~0ull;
    s_hud_tonemapped_last_valid_frame = ~0ull;
    s_hud_tonemapped_alpha_dirty = true;
    s_hdr_blurred_in_place_frame = ~0ull;
    s_pause_menu_seen_frame = ~0ull;
    s_ldr_tonemap_candidate_bound = false;
    s_post_ldr_backbuffer_bound = false;
    s_post_ldr_backbuffer_no_dsv = false;
    s_post_ldr_backbuffer_ui_draws = 0;
    s_clean_camera_substitutions.store(0, std::memory_order_relaxed);
    s_jittered_cb1_buffer.store(nullptr, std::memory_order_relaxed);
    s_hud_mask_texture.Reset();
    s_hud_mask_prev_texture.Reset();
    s_hud_mask_rtv.Reset();
    s_hud_mask_srv.Reset();
    s_hud_mask_prev_srv.Reset();
    s_hud_tonemapped_texture.Reset();
    s_hud_tonemapped_prev_texture.Reset();
    s_hud_tonemapped_rtv.Reset();
    s_hud_tonemapped_srv.Reset();
    s_hud_tonemapped_prev_srv.Reset();
    s_hud_earlyz_texture.Reset();
    s_hud_earlyz_dsv.Reset();
    s_hud_earlyz_valid_frame = ~0ull;
    s_hud_tonemapped_alpha_dirty = true;
    s_hud_merged_hdr_texture.Reset();
    s_hud_merged_hdr_srv.Reset();
    s_hud_merged_hdr_rtv.Reset();
    s_hud_merged_hdr_width = 0;
    s_hud_merged_hdr_height = 0;
    s_hud_merged_hdr_format = DXGI_FORMAT_UNKNOWN;
    s_hud_merged_hdr_source_res = nullptr;
    s_hud_merged_hdr_frame = ~0ull;
    s_zero_hdr_texture.Reset();
    s_zero_hdr_srv.Reset();
    s_far_linear_depth_texture.Reset();
    s_far_linear_depth_rtv.Reset();
    s_far_linear_depth_srv.Reset();
    s_far_linear_depth_cleared = false;
    s_hud_depth_stencil_texture.Reset();
    s_hud_depth_stencil_dsv.Reset();
    s_hud_depth_stencil_tex_fmt = DXGI_FORMAT_UNKNOWN;
    s_hud_depth_stencil_width = 0;
    s_hud_depth_stencil_height = 0;
    s_hud_depth_stencil_seeded = false;
    s_hud_depth_only_seeded = false;
    s_seed_cached_depth_tex = nullptr;
    s_seed_cached_depth_srv.Reset();
    s_seed_cached_stencil_srv.Reset();
    s_hud_depth_seed_ps.Reset();
    for (auto& p : s_hud_stencil_seed_ps) p.Reset();
    s_hud_seed_ds_write_depth.Reset();
    for (auto& s : s_hud_seed_ds_write_stencil) s.Reset();
    for (auto& e : s_hud_ro_ds_cache) { e.orig.Reset(); e.forceDepthGe = false; e.ro.Reset(); }
    s_hud_hdr_merge_vs.Reset();
    s_hud_hdr_merge_ps.Reset();
    s_hud_hdr_merge_sampler.Reset();
    s_hud_hdr_merge_blend.Reset();
    s_hud_mask_blend_add.Reset();
    s_hud_mask_blend_scaled_add.Reset();
    s_hud_mask_blend_alpha.Reset();
    s_hud_mask_blend_premul.Reset();
    s_hud_mask_blend_opaque.Reset();
    s_hud_mask_blend_darken.Reset();
    s_hud_mask_blend_colorless.Reset();
    s_hud_tone_rgb_only_blend.Reset();
    s_hud_pass54_tone_atten_blend.Reset();
    s_hud_pass54_mul_wash_blend.Reset();
    s_hud_mask_ds_state.Reset();
    s_hud_mask_ds_state_no_depth.Reset();
    s_hud_earlyz_texture.Reset();
    s_hud_earlyz_dsv.Reset();
    s_hud_earlyz_write_ds.Reset();
    s_hud_earlyz_test_ds.Reset();
    s_hud_earlyz_valid_frame = ~0ull;
    s_hud_mask_staging.Reset();
    s_hud_mask_width = 0;
    s_hud_mask_height = 0;
    s_hud_mask_staging_width = 0;
    s_hud_mask_staging_height = 0;
    s_hud_mask_cleared_for_frame = false;
    s_hud_mask_active_pass = false;
    s_hud_mask_active_frame = ~0ull;
    s_hud_draws_replayed_frame = 0;
    s_clean_camera_substitutions.store(0, std::memory_order_relaxed);
    s_jittered_cb1_buffer.store(nullptr, std::memory_order_relaxed);
    s_hud_mask_last_valid_frame = ~0ull;
    s_hud_mask_last_valid_draws = 0;
    s_hud_mask_coverage_pending_frame = ~0ull;
    s_hud_mask_device = newDevice;
}

void ensureHudMaskResources(ID3D11Device* device, UINT width, UINT height) {
    if (!device || width == 0 || height == 0) return;
    if (s_hud_mask_device != device) {
        resetHudMaskResources(device);
    }
    if (s_hud_mask_texture && s_hud_mask_width == width && s_hud_mask_height == height) {
        return;
    }
    s_hud_mask_texture.Reset();
    s_hud_mask_prev_texture.Reset();
    s_hud_mask_rtv.Reset();
    s_hud_mask_srv.Reset();
    s_hud_mask_prev_srv.Reset();
    s_hud_mask_last_valid_frame = ~0ull;
    s_hud_mask_last_valid_draws = 0;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(device->CreateTexture2D(&td, nullptr, &s_hud_mask_texture))) {
        EdpeLog(L"EDPE: failed to create HUD mask texture");
        return;
    }

    D3D11_TEXTURE2D_DESC prevTd = td;
    prevTd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    device->CreateTexture2D(&prevTd, nullptr, &s_hud_mask_prev_texture);

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(s_hud_mask_texture.Get(), &rtvDesc, &s_hud_mask_rtv);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(s_hud_mask_texture.Get(), &srvDesc, &s_hud_mask_srv);
    if (s_hud_mask_prev_texture) {
        device->CreateShaderResourceView(s_hud_mask_prev_texture.Get(), &srvDesc, &s_hud_mask_prev_srv);
    }

    s_hud_tonemapped_texture.Reset();
    s_hud_tonemapped_prev_texture.Reset();
    s_hud_tonemapped_rtv.Reset();
    s_hud_tonemapped_srv.Reset();
    s_hud_tonemapped_prev_srv.Reset();
    s_hud_merged_hdr_texture.Reset();
    s_hud_merged_hdr_srv.Reset();
    s_hud_merged_hdr_rtv.Reset();
    s_hud_merged_hdr_width = 0;
    s_hud_merged_hdr_height = 0;
    s_hud_merged_hdr_format = DXGI_FORMAT_UNKNOWN;
    s_hud_merged_hdr_source_res = nullptr;
    s_hud_merged_hdr_frame = ~0ull;
    s_hud_tonemapped_valid_frame = ~0ull;
    s_hud_tonemapped_last_valid_frame = ~0ull;
    {
        D3D11_TEXTURE2D_DESC toneTd = td;
        toneTd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        toneTd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(device->CreateTexture2D(&toneTd, nullptr, &s_hud_tonemapped_texture))) {
            device->CreateRenderTargetView(s_hud_tonemapped_texture.Get(), nullptr, &s_hud_tonemapped_rtv);
            device->CreateShaderResourceView(s_hud_tonemapped_texture.Get(), nullptr, &s_hud_tonemapped_srv);
        }
        toneTd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(device->CreateTexture2D(&toneTd, nullptr, &s_hud_tonemapped_prev_texture))) {
            device->CreateShaderResourceView(s_hud_tonemapped_prev_texture.Get(), nullptr, &s_hud_tonemapped_prev_srv);
        }
    }

    s_far_linear_depth_texture.Reset();
    s_far_linear_depth_rtv.Reset();
    s_far_linear_depth_srv.Reset();
    s_far_linear_depth_cleared = false;
    {
        D3D11_TEXTURE2D_DESC dTd = td;
        dTd.Format = DXGI_FORMAT_R32_FLOAT;
        dTd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(device->CreateTexture2D(&dTd, nullptr, &s_far_linear_depth_texture))) {
            device->CreateRenderTargetView(s_far_linear_depth_texture.Get(), nullptr, &s_far_linear_depth_rtv);
            device->CreateShaderResourceView(s_far_linear_depth_texture.Get(), nullptr, &s_far_linear_depth_srv);
        }
    }

    s_hud_depth_stencil_texture.Reset();
    s_hud_depth_stencil_dsv.Reset();
    s_hud_depth_stencil_tex_fmt = DXGI_FORMAT_R32G8X24_TYPELESS;
    s_hud_depth_stencil_width = width;
    s_hud_depth_stencil_height = height;
    s_hud_depth_stencil_seeded = false;
    s_hud_depth_only_seeded = false;
    {
        D3D11_TEXTURE2D_DESC dsTd = td;
        dsTd.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        dsTd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(device->CreateTexture2D(&dsTd, nullptr, &s_hud_depth_stencil_texture))) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
            dsvDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            device->CreateDepthStencilView(s_hud_depth_stencil_texture.Get(), &dsvDesc, &s_hud_depth_stencil_dsv);
        }
    }
    if (!s_hud_depth_seed_ps) {
        Microsoft::WRL::ComPtr<ID3DBlob> depthPsBlob;
        if (SUCCEEDED(D3DCompile(kHudDepthStencilSeedShader, sizeof(kHudDepthStencilSeedShader) - 1, nullptr, nullptr,
                nullptr, "ps_depth", "ps_5_0", 0, 0, &depthPsBlob, nullptr))) {
            device->CreatePixelShader(depthPsBlob->GetBufferPointer(), depthPsBlob->GetBufferSize(), nullptr, &s_hud_depth_seed_ps);
        }
        static constexpr const char* kStencilEntries[8] = {
            "ps_s0", "ps_s1", "ps_s2", "ps_s3", "ps_s4", "ps_s5", "ps_s6", "ps_s7"
        };
        for (int i = 0; i < 8; ++i) {
            Microsoft::WRL::ComPtr<ID3DBlob> sBlob;
            if (SUCCEEDED(D3DCompile(kHudDepthStencilSeedShader, sizeof(kHudDepthStencilSeedShader) - 1, nullptr, nullptr,
                    nullptr, kStencilEntries[i], "ps_5_0", 0, 0, &sBlob, nullptr))) {
                device->CreatePixelShader(sBlob->GetBufferPointer(), sBlob->GetBufferSize(), nullptr, &s_hud_stencil_seed_ps[i]);
            }
        }
    }
    if (!s_hud_seed_ds_write_depth) {
        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable = TRUE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dsd.StencilEnable = FALSE;
        device->CreateDepthStencilState(&dsd, &s_hud_seed_ds_write_depth);
    }
    for (int i = 0; i < 8; ++i) {
        if (!s_hud_seed_ds_write_stencil[i]) {
            const UINT8 bit = static_cast<UINT8>(1u << i);
            D3D11_DEPTH_STENCIL_DESC dsd{};
            dsd.DepthEnable = FALSE;
            dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
            dsd.StencilEnable = TRUE;
            dsd.StencilReadMask = bit;
            dsd.StencilWriteMask = bit;
            dsd.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
            dsd.BackFace = dsd.FrontFace;
            device->CreateDepthStencilState(&dsd, &s_hud_seed_ds_write_stencil[i]);
        }
    }

    if (!s_zero_hdr_srv) {
        D3D11_TEXTURE2D_DESC zDesc{};
        zDesc.Width = 1;
        zDesc.Height = 1;
        zDesc.MipLevels = 1;
        zDesc.ArraySize = 1;
        zDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        zDesc.SampleDesc.Count = 1;
        zDesc.Usage = D3D11_USAGE_DEFAULT;
        zDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const uint16_t zeroHalf[4] = {0, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA zInit{zeroHalf, sizeof(zeroHalf), 0};
        if (SUCCEEDED(device->CreateTexture2D(&zDesc, &zInit, &s_zero_hdr_texture))) {
            device->CreateShaderResourceView(s_zero_hdr_texture.Get(), nullptr, &s_zero_hdr_srv);
        }
    }

    if (!s_hud_hdr_merge_vs || !s_hud_hdr_merge_ps) {
        Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob;
        if (SUCCEEDED(D3DCompile(kHudHdrMergeShader, sizeof(kHudHdrMergeShader) - 1, nullptr, nullptr,
                nullptr, "vs_main", "vs_5_0", 0, 0, &vsBlob, nullptr)) &&
            SUCCEEDED(D3DCompile(kHudHdrMergeShader, sizeof(kHudHdrMergeShader) - 1, nullptr, nullptr,
                nullptr, "ps_main", "ps_5_0", 0, 0, &psBlob, nullptr))) {
            device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &s_hud_hdr_merge_vs);
            device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &s_hud_hdr_merge_ps);
        }
    }
    if (!s_hud_hdr_merge_sampler) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MinLOD = 0.0f;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&sd, &s_hud_hdr_merge_sampler);
    }
    if (!s_hud_hdr_merge_blend) {
        D3D11_BLEND_DESC mbd{};
        mbd.RenderTarget[0].BlendEnable = TRUE;
        mbd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        mbd.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_ALPHA; // dst.rgb = hud.rgb + saturate(hud.a) * dst.rgb
        mbd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        mbd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        mbd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        mbd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        mbd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&mbd, &s_hud_hdr_merge_blend);
    }

    s_hud_mask_width = width;
    s_hud_mask_height = height;

    if (!s_hud_mask_blend_add) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; // Transmittance untouched: T' = T
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&bd, &s_hud_mask_blend_add);
    }
    if (!s_hud_mask_blend_scaled_add) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; // Scaled additive: L' = L + S.rgb * S.a, T' = T
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&bd, &s_hud_mask_blend_scaled_add);
    }
    if (!s_hud_mask_blend_colorless) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].RenderTargetWriteMask = 0; // Colourless stencil write-back into scene DSV
        device->CreateBlendState(&bd, &s_hud_mask_blend_colorless);
    }
    if (!s_hud_mask_blend_alpha) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; // T' = T * (1 - a)
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&bd, &s_hud_mask_blend_alpha);
    }
    if (!s_hud_mask_blend_premul) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; // T' = T * (1 - a)
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&bd, &s_hud_mask_blend_premul);
    }
    if (!s_hud_mask_blend_opaque) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; // Opaque: T' = 0
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&bd, &s_hud_mask_blend_opaque);
    }
    if (!s_hud_mask_blend_darken) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; // Darkening: T' = T * (1 - a)
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&bd, &s_hud_mask_blend_darken);
    }
    if (!s_hud_tone_rgb_only_blend) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        device->CreateBlendState(&bd, &s_hud_tone_rgb_only_blend);
    }
    if (!s_hud_pass54_tone_atten_blend) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; // T_54' = T_54 * (1 - srcA)
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
        device->CreateBlendState(&bd, &s_hud_pass54_tone_atten_blend);
    }
    if (!s_hud_pass54_mul_wash_blend) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_SRC_ALPHA; // T_54' = T_54 * srcA
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
        device->CreateBlendState(&bd, &s_hud_pass54_mul_wash_blend);
    }
    if (!s_hud_mask_ds_state) {
        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable = TRUE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; // STRICTLY READ ONLY: never modify scene depth
        dsd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;   // Elite Dangerous reversed-Z
        dsd.StencilEnable = FALSE;
        device->CreateDepthStencilState(&dsd, &s_hud_mask_ds_state);
    }
    if (!s_hud_mask_ds_state_no_depth) {
        D3D11_DEPTH_STENCIL_DESC dsdNoDepth{};
        dsdNoDepth.DepthEnable = FALSE;
        dsdNoDepth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dsdNoDepth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dsdNoDepth.StencilEnable = FALSE;
        device->CreateDepthStencilState(&dsdNoDepth, &s_hud_mask_ds_state_no_depth);
    }
    if (!s_hud_earlyz_texture || !s_hud_earlyz_dsv) {
        s_hud_earlyz_texture.Reset();
        s_hud_earlyz_dsv.Reset();
        D3D11_TEXTURE2D_DESC ezDesc{};
        ezDesc.Width = width;
        ezDesc.Height = height;
        ezDesc.MipLevels = 1;
        ezDesc.ArraySize = 1;
        ezDesc.Format = DXGI_FORMAT_D16_UNORM;
        ezDesc.SampleDesc.Count = 1;
        ezDesc.Usage = D3D11_USAGE_DEFAULT;
        ezDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (SUCCEEDED(device->CreateTexture2D(&ezDesc, nullptr, &s_hud_earlyz_texture))) {
            device->CreateDepthStencilView(s_hud_earlyz_texture.Get(), nullptr, &s_hud_earlyz_dsv);
        }
    }
    if (!s_hud_earlyz_write_ds) {
        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable = TRUE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dsd.StencilEnable = FALSE;
        device->CreateDepthStencilState(&dsd, &s_hud_earlyz_write_ds);
    }
    if (!s_hud_earlyz_test_ds) {
        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable = TRUE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dsd.DepthFunc = D3D11_COMPARISON_LESS;
        dsd.StencilEnable = FALSE;
        device->CreateDepthStencilState(&dsd, &s_hud_earlyz_test_ds);
    }

    wchar_t msg[128];
    swprintf_s(msg, L"EDPE: allocated HUD mask target %ux%u R16G16B16A16_FLOAT", width, height);
    EdpeLog(msg);
}

void ensureHudMaskStaging(ID3D11Device* device, UINT width, UINT height) {
    if (!device || width == 0 || height == 0) return;
    if (s_hud_mask_staging) {
        Microsoft::WRL::ComPtr<ID3D11Device> stagingDev;
        s_hud_mask_staging->GetDevice(&stagingDev);
        if (stagingDev.Get() == device &&
            s_hud_mask_staging_width == width &&
            s_hud_mask_staging_height == height) {
            return;
        }
        s_hud_mask_staging.Reset();
        s_hud_mask_staging_width = 0;
        s_hud_mask_staging_height = 0;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &s_hud_mask_staging))) {
        s_hud_mask_staging_width = width;
        s_hud_mask_staging_height = height;
    }
}

void rearmHudDrawProbe(ID3D11DeviceContext* activeCtx = nullptr);
void abandonAmortizedAndRearmHudProbe(ID3D11DeviceContext* context);

ID3D11DepthStencilState* getHudReadOnlyDepthState(
    ID3D11DeviceContext* context,
    ID3D11DepthStencilState* origDs,
    const D3D11_DEPTH_STENCIL_DESC& origDesc,
    bool forceDepthGe = false) {
    if (!origDs) {
        return s_hud_mask_ds_state.Get();
    }
    if (!forceDepthGe && origDesc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO) {
        return origDs;
    }
    for (auto& entry : s_hud_ro_ds_cache) {
        if (entry.orig.Get() == origDs && entry.forceDepthGe == forceDepthGe && entry.ro) {
            return entry.ro.Get();
        }
    }
    Microsoft::WRL::ComPtr<ID3D11Device> dev;
    context->GetDevice(&dev);
    if (!dev) return s_hud_mask_ds_state.Get();
    D3D11_DEPTH_STENCIL_DESC roDesc = origDesc;
    roDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (forceDepthGe) {
        roDesc.DepthEnable = TRUE;
        roDesc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    }
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> created;
    if (FAILED(dev->CreateDepthStencilState(&roDesc, &created)) || !created) {
        return s_hud_mask_ds_state.Get();
    }
    for (auto& entry : s_hud_ro_ds_cache) {
        if (!entry.ro) {
            entry.orig = origDs;
            entry.forceDepthGe = forceDepthGe;
            entry.ro = created;
            return entry.ro.Get();
        }
    }
    s_hud_ro_ds_cache[0].orig = origDs;
    s_hud_ro_ds_cache[0].forceDepthGe = forceDepthGe;
    s_hud_ro_ds_cache[0].ro = created;
    return s_hud_ro_ds_cache[0].ro.Get();
}

ID3D11DepthStencilView* ensureSeededHudDepthStencil(
    ID3D11DeviceContext* context,
    ID3D11DepthStencilView* srcDsv) {
    if (!context || !srcDsv || !s_hud_depth_stencil_texture || !s_hud_depth_stencil_dsv ||
        !s_hud_mask_width || !s_hud_mask_height) {
        return nullptr;
    }
    if (s_hud_depth_stencil_seeded) {
        return s_hud_depth_stencil_dsv.Get();
    }
    Microsoft::WRL::ComPtr<ID3D11Resource> dsvRes;
    srcDsv->GetResource(&dsvRes);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depthTex;
    if (!dsvRes || FAILED(dsvRes.As(&depthTex)) || !depthTex) {
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC dDesc{};
    depthTex->GetDesc(&dDesc);
    if (!dDesc.Width || !dDesc.Height || dDesc.SampleDesc.Count != 1) {
        return nullptr;
    }

    // Fast path (Native Resolution / DLAA / TAA): 1:1 GPU copy of depth and all 8 stencil bits once per frame.
    if (dDesc.Width == s_hud_mask_width && dDesc.Height == s_hud_mask_height) {
        context->CopyResource(s_hud_depth_stencil_texture.Get(), depthTex.Get());
        s_hud_depth_stencil_seeded = true;
        s_hud_depth_only_seeded = true;
        return s_hud_depth_stencil_dsv.Get();
    }

    // Super Resolution path (renderSize < outputSize): resample depth and all 8 stencil bits (0x01..0x80)
    // into the native-sized s_hud_depth_stencil_dsv, caching the SRVs per texture.
    if (!(dDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
        !s_hud_hdr_merge_vs || !s_hud_depth_seed_ps || !s_hud_seed_ds_write_depth) {
        return nullptr;
    }
    if (s_seed_cached_depth_tex != depthTex.Get() || !s_seed_cached_depth_srv) {
        s_seed_cached_depth_tex = nullptr;
        s_seed_cached_depth_srv.Reset();
        s_seed_cached_stencil_srv.Reset();
        s_hud_depth_only_seeded = false;

        Microsoft::WRL::ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (!device) return nullptr;

        DXGI_FORMAT depthSrvFmt = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT stencilSrvFmt = DXGI_FORMAT_UNKNOWN;
        if (dDesc.Format == DXGI_FORMAT_R32G8X24_TYPELESS || dDesc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
            depthSrvFmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            stencilSrvFmt = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        } else if (dDesc.Format == DXGI_FORMAT_R24G8_TYPELESS || dDesc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT) {
            depthSrvFmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            stencilSrvFmt = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        } else if (dDesc.Format == DXGI_FORMAT_R32_TYPELESS || dDesc.Format == DXGI_FORMAT_D32_FLOAT) {
            depthSrvFmt = DXGI_FORMAT_R32_FLOAT;
        } else {
            return nullptr;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = depthSrvFmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(depthTex.Get(), &sd, &s_seed_cached_depth_srv))) {
            return nullptr;
        }
        if (stencilSrvFmt != DXGI_FORMAT_UNKNOWN) {
            sd.Format = stencilSrvFmt;
            device->CreateShaderResourceView(depthTex.Get(), &sd, &s_seed_cached_stencil_srv);
        }
        s_seed_cached_depth_tex = depthTex.Get();
    }

    const OMSetRenderTargetsFn rawSet = resolveOriginalOM(context);
    if (!rawSet) return nullptr;

    ID3D11RenderTargetView* origRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> origDsv;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, origRtvs, origDsv.GetAddressOf());

    ID3D11ShaderResourceView* origPsSrvs[2]{};
    context->PSGetShaderResources(0, 2, origPsSrvs);

    Microsoft::WRL::ComPtr<ID3D11VertexShader> origVs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> origPs;
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> origGs;
    context->VSGetShader(origVs.GetAddressOf(), nullptr, nullptr);
    context->PSGetShader(origPs.GetAddressOf(), nullptr, nullptr);
    context->GSGetShader(origGs.GetAddressOf(), nullptr, nullptr);

    Microsoft::WRL::ComPtr<ID3D11InputLayout> origLayout;
    context->IAGetInputLayout(origLayout.GetAddressOf());
    D3D11_PRIMITIVE_TOPOLOGY origTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    context->IAGetPrimitiveTopology(&origTopo);

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> origRs;
    context->RSGetState(origRs.GetAddressOf());
    UINT numVps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT origVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    context->RSGetViewports(&numVps, origVps);
    UINT numScissors = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT origScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    context->RSGetScissorRects(&numScissors, origScissors);

    Microsoft::WRL::ComPtr<ID3D11BlendState> origBlend;
    FLOAT origBlendFactor[4]{};
    UINT origSampleMask = 0xFFFFFFFF;
    context->OMGetBlendState(origBlend.GetAddressOf(), origBlendFactor, &origSampleMask);
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> origDs;
    UINT origStencilRef = 0;
    context->OMGetDepthStencilState(origDs.GetAddressOf(), &origStencilRef);

    // Unbind origDsv first so depthTex can be bound as PS SRV without D3D11 hazard
    ID3D11ShaderResourceView* nullSrvs[2]{};
    context->PSSetShaderResources(0, 2, nullSrvs);
    rawSet(context, 0, nullptr, s_hud_depth_stencil_dsv.Get());
    context->ClearDepthStencilView(s_hud_depth_stencil_dsv.Get(), D3D11_CLEAR_STENCIL, 0.0f, 0);

    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(s_hud_mask_width),
                            static_cast<float>(s_hud_mask_height), 0.0f, 1.0f};
    context->RSSetViewports(1, &vp);
    const D3D11_RECT fullScissor{0, 0, static_cast<LONG>(s_hud_mask_width), static_cast<LONG>(s_hud_mask_height)};
    context->RSSetScissorRects(1, &fullScissor);
    context->RSSetState(nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(s_hud_hdr_merge_vs.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    const FLOAT blendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    context->OMSetBlendState(s_hud_mask_blend_colorless.Get(), blendFactor, 0xFFFFFFFF);

    // 1. Export nearest-sampled depth into s_hud_depth_stencil_dsv (once per frame per uiLayerSeedKeepsDepth)
    if (!s_hud_depth_only_seeded) {
        ID3D11ShaderResourceView* dSrv = s_seed_cached_depth_srv.Get();
        context->PSSetShaderResources(0, 1, &dSrv);
        context->PSSetShader(s_hud_depth_seed_ps.Get(), nullptr, 0);
        context->OMSetDepthStencilState(s_hud_seed_ds_write_depth.Get(), 0);
        if (auto* drawFn = resolveOriginalDraw(context)) {
            drawFn(context, 3, 0);
        } else {
            context->Draw(3, 0);
        }
        rearmHudDrawProbe(context);
        s_hud_depth_only_seeded = true;
    }

    // 2. Export all 8 stencil bits (0x01..0x80, preserving 21=0x15 on ship nose and 149=0x95 on cockpit)
    if (s_seed_cached_stencil_srv) {
        ID3D11ShaderResourceView* sSrv = s_seed_cached_stencil_srv.Get();
        context->PSSetShaderResources(1, 1, &sSrv);
        for (int i = 0; i < 8; ++i) {
            if (!s_hud_seed_ds_write_stencil[i] || !s_hud_stencil_seed_ps[i]) continue;
            context->PSSetShader(s_hud_stencil_seed_ps[i].Get(), nullptr, 0);
            context->OMSetDepthStencilState(s_hud_seed_ds_write_stencil[i].Get(), 0xFFu);
            if (auto* drawFn = resolveOriginalDraw(context)) {
                drawFn(context, 3, 0);
            } else {
                context->Draw(3, 0);
            }
            rearmHudDrawProbe(context);
        }
    }

    context->PSSetShaderResources(0, 2, nullSrvs);
    rawSet(context, 0, nullptr, nullptr);
    context->PSSetShaderResources(0, 2, origPsSrvs);
    rawSet(context, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, origRtvs, origDsv.Get());
    context->VSSetShader(origVs.Get(), nullptr, 0);
    context->PSSetShader(origPs.Get(), nullptr, 0);
    context->GSSetShader(origGs.Get(), nullptr, 0);
    context->IASetInputLayout(origLayout.Get());
    context->IASetPrimitiveTopology(origTopo);
    context->RSSetState(origRs.Get());
    context->RSSetViewports(numVps, origVps);
    if (numScissors > 0) {
        context->RSSetScissorRects(numScissors, origScissors);
    }
    context->OMSetDepthStencilState(origDs.Get(), origStencilRef);
    context->OMSetBlendState(origBlend.Get(), origBlendFactor, origSampleMask);

    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        if (origRtvs[i]) origRtvs[i]->Release();
    }
    for (int i = 0; i < 2; ++i) {
        if (origPsSrvs[i]) origPsSrvs[i]->Release();
    }
    rearmHudDrawProbe(context);

    s_hud_depth_stencil_seeded = true;
    return s_hud_depth_stencil_dsv.Get();
}

bool replayHudDraw(ID3D11DeviceContext* context, char kind, UINT count, UINT instance_count,
                   UINT start_index_or_vertex, INT base_vertex, UINT start_instance) {
    if (!context || !s_hud_mask_rtv || !s_hud_mask_blend_add || !s_hud_mask_blend_alpha || !s_hud_mask_active_pass) return false;

    ID3D11RenderTargetView* orig_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(1, orig_rtvs, depth.GetAddressOf());
    if (!orig_rtvs[0] || !motion_color_source ||
        (!motion_color_bound && !sameRenderTargetResource(orig_rtvs[0], motion_color_source.Get()))) {
        if (orig_rtvs[0]) orig_rtvs[0]->Release();
        return false;
    }

    const OMSetRenderTargetsFn set_targets = resolveOriginalOM(context);
    if (!set_targets) {
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            if (orig_rtvs[i]) orig_rtvs[i]->Release();
        }
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11BlendState> original_blend;
    FLOAT original_blend_factor[4]{};
    UINT original_sample_mask = 0;
    context->OMGetBlendState(original_blend.GetAddressOf(), original_blend_factor, &original_sample_mask);

    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> original_ds;
    UINT original_stencil_ref = 0;
    context->OMGetDepthStencilState(original_ds.GetAddressOf(), &original_stencil_ref);

    bool orig_depth_enable = true;
    bool orig_stencil_enable = false;
    bool orig_depth_readonly = false;
    bool orig_writes_stencil = false;
    D3D11_DEPTH_STENCIL_DESC origDsDesc{};
    if (original_ds) {
        original_ds->GetDesc(&origDsDesc);
        orig_depth_enable = (origDsDesc.DepthEnable != FALSE);
        orig_stencil_enable = (origDsDesc.StencilEnable != FALSE);
        orig_depth_readonly = (!origDsDesc.DepthEnable || origDsDesc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO);
        if (origDsDesc.StencilEnable && origDsDesc.StencilWriteMask != 0 && orig_depth_readonly) {
            orig_writes_stencil =
                (origDsDesc.FrontFace.StencilPassOp != D3D11_STENCIL_OP_KEEP ||
                 origDsDesc.FrontFace.StencilFailOp != D3D11_STENCIL_OP_KEEP ||
                 origDsDesc.FrontFace.StencilDepthFailOp != D3D11_STENCIL_OP_KEEP ||
                 origDsDesc.BackFace.StencilPassOp != D3D11_STENCIL_OP_KEEP ||
                 origDsDesc.BackFace.StencilFailOp != D3D11_STENCIL_OP_KEEP ||
                 origDsDesc.BackFace.StencilDepthFailOp != D3D11_STENCIL_OP_KEEP);
        }
    }

    const bool isHoloBillboardForDs = (s_current_hud_vs_hash == 0x5559BD94B6852E83ull);
    ID3D11DepthStencilView* depth_to_bind = nullptr;
    if ((orig_depth_enable || orig_stencil_enable) &&
        !isHoloBillboardForDs && depth) {
        depth_to_bind = ensureSeededHudDepthStencil(context, depth.Get());
    }

    UINT srcW = s_observed_scene_width.load(std::memory_order_relaxed);
    UINT srcH = s_observed_scene_height.load(std::memory_order_relaxed);
    if ((srcW == 0 || srcH == 0) && orig_rtvs[0]) {
        Microsoft::WRL::ComPtr<ID3D11Resource> rtRes;
        orig_rtvs[0]->GetResource(&rtRes);
        if (rtRes) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> rtTex;
            if (SUCCEEDED(rtRes.As(&rtTex))) {
                D3D11_TEXTURE2D_DESC rtd{};
                rtTex->GetDesc(&rtd);
                if (rtd.Width > 0 && rtd.Height > 0) {
                    srcW = rtd.Width;
                    srcH = rtd.Height;
                }
            }
        }
    }
    const float scaleX = (srcW > 0 && s_hud_mask_width > 0)
        ? (static_cast<float>(s_hud_mask_width) / static_cast<float>(srcW)) : 1.0f;
    const float scaleY = (srcH > 0 && s_hud_mask_height > 0)
        ? (static_cast<float>(s_hud_mask_height) / static_cast<float>(srcH)) : 1.0f;
    const bool needVpScale = (std::fabs(scaleX - 1.0f) > 1e-4f || std::fabs(scaleY - 1.0f) > 1e-4f);

    UINT numViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT origViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D11_VIEWPORT hudViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT numScissors = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT origScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D11_RECT hudScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    auto applyScaledVpAndScissor = [&]() {
        if (!needVpScale) return;
        if (numViewports > 0) {
            context->RSSetViewports(numViewports, hudViewports);
        } else {
            D3D11_VIEWPORT hudViewport{};
            hudViewport.TopLeftX = 0.0f;
            hudViewport.TopLeftY = 0.0f;
            hudViewport.Width = static_cast<float>(s_hud_mask_width);
            hudViewport.Height = static_cast<float>(s_hud_mask_height);
            hudViewport.MinDepth = 0.0f;
            hudViewport.MaxDepth = 1.0f;
            context->RSSetViewports(1, &hudViewport);
        }
        if (numScissors > 0) {
            context->RSSetScissorRects(numScissors, hudScissors);
        }
    };
    auto restoreOrigVpAndScissor = [&]() {
        if (!needVpScale) return;
        context->RSSetViewports(numViewports, origViewports);
        if (numScissors > 0) {
            context->RSSetScissorRects(numScissors, origScissors);
        }
    };
    if (needVpScale) {
        context->RSGetViewports(&numViewports, origViewports);
        context->RSGetScissorRects(&numScissors, origScissors);
        for (UINT i = 0; i < numViewports; ++i) {
            hudViewports[i].TopLeftX = origViewports[i].TopLeftX * scaleX;
            hudViewports[i].TopLeftY = origViewports[i].TopLeftY * scaleY;
            hudViewports[i].Width = origViewports[i].Width * scaleX;
            hudViewports[i].Height = origViewports[i].Height * scaleY;
            hudViewports[i].MinDepth = origViewports[i].MinDepth;
            hudViewports[i].MaxDepth = origViewports[i].MaxDepth;
        }
        const LONG maxW = static_cast<LONG>(s_hud_mask_width);
        const LONG maxH = static_cast<LONG>(s_hud_mask_height);
        for (UINT i = 0; i < numScissors; ++i) {
            hudScissors[i].left = std::clamp(static_cast<LONG>(std::lround(origScissors[i].left * scaleX)), 0L, maxW);
            hudScissors[i].top = std::clamp(static_cast<LONG>(std::lround(origScissors[i].top * scaleY)), 0L, maxH);
            hudScissors[i].right = std::clamp(static_cast<LONG>(std::lround(origScissors[i].right * scaleX)), 0L, maxW);
            hudScissors[i].bottom = std::clamp(static_cast<LONG>(std::lround(origScissors[i].bottom * scaleY)), 0L, maxH);
        }
    }

    // Bind unjittered 5376-byte camera CB1 when orig_cb1 is the jittered camera buffer or when drawing
    // 5559BD94B6852E83 (Target & Own-Ship 3D hologram billboards, which project through cb1[270..273]
    // and compute 8-tap Sobel relief in PS using cb1[120].z = word 482).
    const bool isHoloBillboard = (s_current_hud_vs_hash == 0x5559BD94B6852E83ull);
    const bool needHoloReliefScale = isHoloBillboard && (scaleX > 1.01f || scaleY > 1.01f);
    Microsoft::WRL::ComPtr<ID3D11Buffer> orig_cb1;
    Microsoft::WRL::ComPtr<ID3D11Buffer> orig_ps_cb1;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> orig_ps_srv1;
    bool cb1_substituted = false;
    bool ps_cb1_substituted = false;
    bool ps_srv1_substituted = false;
    if (s_hud_mask_active_pass &&
        s_has_clean_cb1_data && s_clean_camera_frame == motion_color_frame) {
        context->VSGetConstantBuffers(1, 1, orig_cb1.GetAddressOf());
        if (orig_cb1) {
            D3D11_BUFFER_DESC bd{};
            orig_cb1->GetDesc(&bd);
            if (bd.ByteWidth == 5376) {
                if (!s_clean_camera_cb) {
                    Microsoft::WRL::ComPtr<ID3D11Device> dev;
                    context->GetDevice(&dev);
                    if (dev) {
                        D3D11_BUFFER_DESC cbd{};
                        cbd.ByteWidth = 5376;
                        cbd.Usage = D3D11_USAGE_DYNAMIC;
                        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                        dev->CreateBuffer(&cbd, nullptr, &s_clean_camera_cb);
                        s_clean_camera_cb_dirty = true;
                    }
                }
                if (s_clean_camera_cb && (s_clean_camera_cb_dirty || needHoloReliefScale)) {
                    const auto map_fn = resolveOriginalMap(context);
                    const auto unmap_fn = resolveOriginalUnmap(context);
                    if (map_fn && unmap_fn) {
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        if (SUCCEEDED(map_fn(context, s_clean_camera_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                            std::memcpy(mapped.pData, s_last_clean_cb1_data, 5376);
                            if (needHoloReliefScale) {
                                float* words = static_cast<float*>(mapped.pData);
                                const float kScale = std::sqrt(scaleX * scaleY);
                                words[482] = std::max(words[482], 1.0f) * kScale;
                            }
                            unmap_fn(context, s_clean_camera_cb.Get(), 0);
                            s_clean_camera_cb_dirty = needHoloReliefScale;
                        }
                    }
                }
                if (s_clean_camera_cb && orig_cb1.Get() != s_clean_camera_cb.Get()) {
                    ID3D11Buffer* clean_buf = s_clean_camera_cb.Get();
                    if (auto* vs_set = resolveOriginalVSSetConstantBuffers(context)) {
                        vs_set(context, 1, 1, &clean_buf);
                    } else {
                        context->VSSetConstantBuffers(1, 1, &clean_buf);
                    }
                    cb1_substituted = true;
                    s_clean_camera_substitutions.fetch_add(1, std::memory_order_relaxed);
                    if (needHoloReliefScale) {
                        context->PSGetConstantBuffers(1, 1, orig_ps_cb1.GetAddressOf());
                        context->PSSetConstantBuffers(1, 1, &clean_buf);
                        ps_cb1_substituted = true;
                    }
                }
            }
        }
    }

    auto issueDraw = [&]() {
        switch (kind) {
        case 'X': // DrawIndexedInstanced
            if (auto* fn = resolveOriginalDrawIndexedInstanced(context)) {
                fn(context, count, instance_count, start_index_or_vertex, base_vertex, start_instance);
            }
            break;
        case 'I': // DrawIndexed
            if (auto* fn = resolveOriginalDrawIndexed(context)) {
                fn(context, count, start_index_or_vertex, base_vertex);
            }
            break;
        case 'N': // DrawInstanced
            if (auto* fn = resolveOriginalDrawInstanced(context)) {
                fn(context, count, instance_count, start_index_or_vertex, start_instance);
            }
            break;
        case 'D': // Draw
            if (auto* fn = resolveOriginalDraw(context)) {
                fn(context, count, start_index_or_vertex);
            }
            break;
        }
    };

    auto restoreSubstitutedBindings = [&]() {
        if (ps_srv1_substituted) {
            ID3D11ShaderResourceView* restore_srv1 = orig_ps_srv1.Get();
            context->PSSetShaderResources(1, 1, &restore_srv1);
        }
        if (ps_cb1_substituted) {
            ID3D11Buffer* restore_ps_buf = orig_ps_cb1.Get();
            context->PSSetConstantBuffers(1, 1, &restore_ps_buf);
        }
        if (cb1_substituted) {
            ID3D11Buffer* restore_buf = orig_cb1.Get();
            if (auto* vs_set = resolveOriginalVSSetConstantBuffers(context)) {
                vs_set(context, 1, 1, &restore_buf);
            } else {
                context->VSSetConstantBuffers(1, 1, &restore_buf);
            }
        }
    };

    ID3D11BlendState* blend_to_use = s_hud_mask_blend_add.Get();
    if (!original_blend) {
        blend_to_use = s_hud_mask_blend_opaque.Get();
    } else {
        D3D11_BLEND_DESC origBlendDesc{};
        original_blend->GetDesc(&origBlendDesc);
        const auto& rt0 = origBlendDesc.RenderTarget[0];
        if (rt0.RenderTargetWriteMask == 0) {
            // Stencil-only setup draw (e.g. VS 2CECEC3065EF0D4A / PS A4D03619D631B186):
            // execute with unjittered camera CB1 on both the game's depth-stencil target and
            // s_hud_depth_stencil_dsv so stencil mask 0x04 aligns with unjittered HUD panels.
            issueDraw();
            if (s_hud_depth_stencil_seeded && s_hud_depth_stencil_dsv) {
                applyScaledVpAndScissor();
                set_targets(context, 0, nullptr, s_hud_depth_stencil_dsv.Get());
                issueDraw();
                restoreOrigVpAndScissor();
            }
            restoreSubstitutedBindings();
            set_targets(context, 1, orig_rtvs, depth.Get());
            if (orig_rtvs[0]) orig_rtvs[0]->Release();
            if (auto* stat = findOrAddHudPairStat(s_current_hud_vs_hash, s_current_hud_ps_hash)) {
                ++stat->stencil_only;
                if (cb1_substituted) ++stat->cb1_substituted;
            }
            abandonAmortizedAndRearmHudProbe(context);
            return true;
        }

        if (!rt0.BlendEnable) {
            blend_to_use = s_hud_mask_blend_opaque.Get();
        } else if (rt0.SrcBlend == D3D11_BLEND_ZERO) {
            // Multiplicative darkening / backing quad
            blend_to_use = s_hud_mask_blend_darken.Get();
        } else if (rt0.SrcBlend == D3D11_BLEND_ONE && rt0.DestBlend == D3D11_BLEND_INV_SRC_ALPHA) {
            blend_to_use = s_hud_mask_blend_premul.Get();
        } else if ((rt0.SrcBlend == D3D11_BLEND_SRC_ALPHA || rt0.SrcBlend == D3D11_BLEND_SRC_ALPHA_SAT) &&
                   (rt0.DestBlend == D3D11_BLEND_INV_SRC_ALPHA || rt0.DestBlend == D3D11_BLEND_INV_SRC_COLOR)) {
            blend_to_use = s_hud_mask_blend_alpha.Get();
        } else if ((rt0.SrcBlend == D3D11_BLEND_SRC_ALPHA || rt0.SrcBlend == D3D11_BLEND_SRC_ALPHA_SAT) &&
                   rt0.DestBlend == D3D11_BLEND_ONE) {
            // Scaled additive (Flight HUD B7790CBF/8DEF4645, HUD text 81216C77/41B9D7B2, sweep lines C610FB6F):
            // S.a carries glyph/line coverage while S.rgb is solid color. Must use SRC_ALPHA, ONE!
            blend_to_use = s_hud_mask_blend_scaled_add ? s_hud_mask_blend_scaled_add.Get() : s_hud_mask_blend_add.Get();
        } else if (rt0.DestBlend == D3D11_BLEND_ONE) {
            blend_to_use = s_hud_mask_blend_add.Get();
        } else {
            blend_to_use = s_hud_mask_blend_alpha.Get();
        }
    }

    applyScaledVpAndScissor();

    if (isHoloBillboard && s_far_linear_depth_srv) {
        if (s_far_linear_depth_rtv && !s_far_linear_depth_cleared) {
            const float farDepth[4] = {10000.0f, 10000.0f, 10000.0f, 10000.0f};
            context->ClearRenderTargetView(s_far_linear_depth_rtv.Get(), farDepth);
            s_far_linear_depth_cleared = true;
        }
        context->PSGetShaderResources(1, 1, orig_ps_srv1.GetAddressOf());
        ID3D11ShaderResourceView* farSrv = s_far_linear_depth_srv.Get();
        context->PSSetShaderResources(1, 1, &farSrv);
        ps_srv1_substituted = true;
    }

    ID3D11RenderTargetView* mask = s_hud_mask_rtv.Get();
    set_targets(context, 1, &mask, depth_to_bind);
    const FLOAT blend_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    context->OMSetBlendState(blend_to_use, blend_factor, 0xFFFFFFFF);
    if (depth_to_bind) {
        ID3D11DepthStencilState* roDs = getHudReadOnlyDepthState(
            context, original_ds.Get(), origDsDesc, false);
        context->OMSetDepthStencilState(roDs, original_stencil_ref);
    } else if (s_hud_mask_ds_state_no_depth) {
        context->OMSetDepthStencilState(s_hud_mask_ds_state_no_depth.Get(), 0);
    }
    rearmHudDrawProbe(context);

    issueDraw();
    rearmHudDrawProbe(context);
    ++s_hud_draws_replayed_frame;
    ++s_diag_hud_replayed;
    if (auto* stat = findOrAddHudPairStat(s_current_hud_vs_hash, s_current_hud_ps_hash)) {
        ++stat->replayed;
        if (cb1_substituted) ++stat->cb1_substituted;
    }

    restoreOrigVpAndScissor();
    context->OMSetDepthStencilState(original_ds.Get(), original_stencil_ref);
    set_targets(context, 1, orig_rtvs, depth.Get());
    // When a redirected HUD panel draw also writes stencil (e.g., 0x04 stencil mask),
    // replay a colourless stencil write-back onto the game's scene DSV while unjittered CB1 is still bound.
    if (orig_writes_stencil && depth && depth_to_bind != depth.Get() && s_hud_mask_blend_colorless) {
        context->OMSetBlendState(s_hud_mask_blend_colorless.Get(), blend_factor, 0xFFFFFFFF);
        issueDraw();
        rearmHudDrawProbe(context);
        set_targets(context, 1, orig_rtvs, depth.Get());
    }
    restoreSubstitutedBindings();
    context->OMSetBlendState(original_blend.Get(), original_blend_factor, original_sample_mask);
    if (orig_rtvs[0]) orig_rtvs[0]->Release();
    abandonAmortizedAndRearmHudProbe(context);
    return true;
}

HudDrawResult observeHudDrawImpl(ID3D11DeviceContext* context, char kind, UINT instances) {
    if (!hud_draw_probe_active.load(std::memory_order_relaxed) ||
        (!s_hud_mask_active_pass && !motion_pair_active) ||
        !motion_color_source ||
        (!motion_color_bound && !motion_pair_active)) return HudDrawResult::None;

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

    if (motion_pair_active) {
        ID3D11RenderTargetView* bound[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, bound, nullptr);
        bool target_matches = false;
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            if (bound[i]) {
                target_matches |= sameRenderTargetResource(bound[i], motion_color_source.Get());
                bound[i]->Release();
            }
        }
        motion_color_bound = target_matches;
        if (!target_matches) return HudDrawResult::None;
    }

    if (motion_pair_active) {
        ++hud_target_draws;
        if (hud_first_match_seen) ++hud_after_copy_draws;
    }
    std::uint64_t vs_hash = 0, ps_hash = 0;
    if (!getFastBoundShaderHashes(context, vs_hash, ps_hash)) {
        if (motion_pair_active) ++hud_shader_missing_hash;
        return HudDrawResult::None;
    }
    if (motion_pair_active) {
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
                Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
                context->PSGetShader(&ps, nullptr, nullptr);
                if (ps) probeWorldShaderFanout(context, ps.Get(), vs_hash, ps_hash);
            }
        }
    }

    if (s_hud_depth_stencil_seeded && vs_hash == 0x2CECEC3065EF0D4Aull && !s_hud_mask_active_pass) {
        // Stencil writer (2CECEC30, stencil 0x04) modifies scene stencil mid-pass when HUD replay is inactive.
        s_hud_depth_stencil_seeded = false;
    }

    if (hud_first_match_seen && isGlassShaderPair(vs_hash, ps_hash)) {
        if (motion_pair_active) ++glass_after_copy_draws;
        if (const auto* reason = glassReplayRefusal(context, kind, instances)) {
            declineGlassReplay(reason);
            return HudDrawResult::None;
        }
        return HudDrawResult::GlassReplay;
    }

    if (isHudShaderPair(vs_hash, ps_hash)) {
        s_current_hud_vs_hash = vs_hash;
        s_current_hud_ps_hash = ps_hash;
        if (!hud_first_match_seen) {
            if (motion_pair_active || !s_first_hud_draw_logged) {
                s_first_hud_draw_logged = true;
                wchar_t message[160];
                swprintf_s(message, L"EDPE: HUD draw on HDR target VS=%016llX PS=%016llX frame=%llu",
                    vs_hash, ps_hash, motion_color_frame);
                EdpeLog(message);
            }
            hud_first_match_seen = true;

            if (motion_pair_active || edpe::Config::instance().hdrInput) {
                captureEarlyMotionColor(context, true);
            }
        }
        if (auto* stat = findOrAddHudPairStat(vs_hash, ps_hash)) {
            ++stat->candidates;
            if (!s_hud_mask_active_pass) ++stat->declined;
        }
        if (!s_hud_mask_active_pass) ++s_diag_hud_declined_draws;
        return s_hud_mask_active_pass ? HudDrawResult::HudReplay : HudDrawResult::None;
    }
    return HudDrawResult::None;
}

HudDrawResult observeHudDraw(ID3D11DeviceContext* context, char kind, UINT instances) {
    const HudDrawResult result = observeHudDrawImpl(context, kind, instances);
    if (result == HudDrawResult::HudReplay) {
        ++s_diag_hud_candidates;
    }
    return result;
}

bool isLdrSceneColorFormat(DXGI_FORMAT fmt);

void mergeHudIntoHdrTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* targetRtv,
                           UINT targetW, UINT targetH,
                           ID3D11ShaderResourceView* baseSceneSrv = nullptr) {
    if (!context || !targetRtv || !s_hud_mask_srv || !targetW || !targetH ||
        !s_hud_hdr_merge_vs || !s_hud_hdr_merge_ps ||
        !s_hud_hdr_merge_sampler || !s_hud_hdr_merge_blend) {
        return;
    }
    const OMSetRenderTargetsFn rawSet = resolveOriginalOM(context);
    if (!rawSet) return;

    // Save current pipeline state directly on context (never use SwapDeviceContextState mid-frame,
    // as SwapDeviceContextState swaps CContext's embedded vtable and unhooks D3D11 observers).
    ID3D11RenderTargetView* origRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> origDsv;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, origRtvs, origDsv.GetAddressOf());

    ID3D11ShaderResourceView* origPsSrvs[8]{};
    context->PSGetShaderResources(0, 8, origPsSrvs);
    Microsoft::WRL::ComPtr<ID3D11SamplerState> origSamp0;
    context->PSGetSamplers(0, 1, origSamp0.GetAddressOf());

    Microsoft::WRL::ComPtr<ID3D11VertexShader> origVs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> origPs;
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> origGs;
    context->VSGetShader(origVs.GetAddressOf(), nullptr, nullptr);
    context->PSGetShader(origPs.GetAddressOf(), nullptr, nullptr);
    context->GSGetShader(origGs.GetAddressOf(), nullptr, nullptr);

    Microsoft::WRL::ComPtr<ID3D11InputLayout> origLayout;
    context->IAGetInputLayout(origLayout.GetAddressOf());
    D3D11_PRIMITIVE_TOPOLOGY origTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    context->IAGetPrimitiveTopology(&origTopo);

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> origRs;
    context->RSGetState(origRs.GetAddressOf());
    UINT numVps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT origVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    context->RSGetViewports(&numVps, origVps);
    UINT numScissors = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT origScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    context->RSGetScissorRects(&numScissors, origScissors);

    Microsoft::WRL::ComPtr<ID3D11BlendState> origBlend;
    FLOAT origBlendFactor[4]{};
    UINT origSampleMask = 0xFFFFFFFF;
    context->OMGetBlendState(origBlend.GetAddressOf(), origBlendFactor, &origSampleMask);
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> origDs;
    UINT origStencilRef = 0;
    context->OMGetDepthStencilState(origDs.GetAddressOf(), &origStencilRef);

    // Unbind PS SRVs 0..7 first so targetRtv can be bound as RTV0 without D3D11 hazard
    ID3D11ShaderResourceView* nullSrvs[8]{};
    context->PSSetShaderResources(0, 8, nullSrvs);

    const bool writeEarlyZ =
        (baseSceneSrv != nullptr && s_hud_earlyz_dsv && s_hud_earlyz_write_ds &&
         targetW == s_hud_mask_width && targetH == s_hud_mask_height);
    if (writeEarlyZ) {
        context->ClearDepthStencilView(s_hud_earlyz_dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        rawSet(context, 1, &targetRtv, s_hud_earlyz_dsv.Get());
    } else {
        rawSet(context, 1, &targetRtv, nullptr);
    }

    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(targetW),
                            static_cast<float>(targetH),
                            writeEarlyZ ? 1.0f : 0.0f, 1.0f};
    context->RSSetViewports(1, &vp);
    const D3D11_RECT fullScissor{0, 0, static_cast<LONG>(targetW), static_cast<LONG>(targetH)};
    context->RSSetScissorRects(1, &fullScissor);
    context->RSSetState(nullptr);
    if (writeEarlyZ) {
        context->OMSetDepthStencilState(s_hud_earlyz_write_ds.Get(), 0);
        s_hud_earlyz_valid_frame = last_present_frame.load(std::memory_order_relaxed);
    } else if (s_hud_mask_ds_state_no_depth) {
        context->OMSetDepthStencilState(s_hud_mask_ds_state_no_depth.Get(), 0);
    }
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(s_hud_hdr_merge_vs.Get(), nullptr, 0);
    context->PSSetShader(s_hud_hdr_merge_ps.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* mergeSrvs[2] = {
        s_hud_mask_srv.Get(),
        baseSceneSrv ? baseSceneSrv : s_zero_hdr_srv.Get()
    };
    context->PSSetShaderResources(0, 2, mergeSrvs);
    ID3D11SamplerState* samp = s_hud_hdr_merge_sampler.Get();
    context->PSSetSamplers(0, 1, &samp);
    const FLOAT blendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    context->OMSetBlendState(baseSceneSrv ? nullptr : s_hud_hdr_merge_blend.Get(), blendFactor, 0xFFFFFFFF);

    if (auto* drawFn = resolveOriginalDraw(context)) {
        drawFn(context, 3, 0);
    } else {
        context->Draw(3, 0);
    }
    rearmHudDrawProbe(context);

    // Unbind HUD/scene SRVs and target RTV before restoring original RTVs and PS SRVs
    context->PSSetShaderResources(0, 2, nullSrvs);
    ID3D11RenderTargetView* nullRtv = nullptr;
    rawSet(context, 1, &nullRtv, nullptr);

    rawSet(context, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, origRtvs, origDsv.Get());
    context->PSSetShaderResources(0, 8, origPsSrvs);
    ID3D11SamplerState* restoreSamp0 = origSamp0.Get();
    context->PSSetSamplers(0, 1, &restoreSamp0);
    context->VSSetShader(origVs.Get(), nullptr, 0);
    context->PSSetShader(origPs.Get(), nullptr, 0);
    context->GSSetShader(origGs.Get(), nullptr, 0);
    context->IASetInputLayout(origLayout.Get());
    context->IASetPrimitiveTopology(origTopo);
    context->RSSetState(origRs.Get());
    context->RSSetViewports(numVps, origVps);
    if (numScissors > 0) {
        context->RSSetScissorRects(numScissors, origScissors);
    }
    context->OMSetDepthStencilState(origDs.Get(), origStencilRef);
    context->OMSetBlendState(origBlend.Get(), origBlendFactor, origSampleMask);

    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        if (origRtvs[i]) origRtvs[i]->Release();
    }
    for (int i = 0; i < 8; ++i) {
        if (origPsSrvs[i]) origPsSrvs[i]->Release();
    }
    rearmHudDrawProbe(context);
}

// Build H' = H * T + HUD in a dedicated native-sized scratch HDR texture (s_hud_merged_hdr_texture,
// never overwriting s_clean_hdr_scene_texture) and feed H' to Pass 36 Bloom level 0 and Pass 52
// HUD+scene tonemapping without modifying the game's HUD-free HDR scene texture (H) in-place!
ID3D11ShaderResourceView* buildHudMergedHdrScratchForBloom(
    ID3D11DeviceContext* context, ID3D11Resource* hdrSourceRes,
    ID3D11ShaderResourceView* hdrSourceSrv = nullptr) {
    if (!context || !hdrSourceRes || !s_hud_mask_srv) return nullptr;
    const auto current_frame = last_present_frame.load(std::memory_order_relaxed);
    if (s_hud_merged_hdr_frame == current_frame &&
        s_hud_merged_hdr_source_res == hdrSourceRes &&
        s_hud_merged_hdr_srv) {
        return s_hud_merged_hdr_srv.Get();
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> hdrTex;
    if (FAILED(hdrSourceRes->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(hdrTex.GetAddressOf()))) || !hdrTex) {
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC desc{};
    hdrTex->GetDesc(&desc);
    if (!desc.Width || !desc.Height || desc.SampleDesc.Count != 1) return nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device) return nullptr;

    const UINT targetW = (s_hud_mask_width > desc.Width) ? s_hud_mask_width : desc.Width;
    const UINT targetH = (s_hud_mask_height > desc.Height) ? s_hud_mask_height : desc.Height;
    const DXGI_FORMAT viewFmt =
        (desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS)
            ? DXGI_FORMAT_R16G16B16A16_FLOAT
            : desc.Format;

    if (!s_hud_merged_hdr_texture || !s_hud_merged_hdr_srv || !s_hud_merged_hdr_rtv ||
        s_hud_merged_hdr_width != targetW || s_hud_merged_hdr_height != targetH ||
        s_hud_merged_hdr_format != viewFmt) {
        s_hud_merged_hdr_texture.Reset();
        s_hud_merged_hdr_srv.Reset();
        s_hud_merged_hdr_rtv.Reset();
        D3D11_TEXTURE2D_DESC copyDesc = desc;
        copyDesc.Width = targetW;
        copyDesc.Height = targetH;
        copyDesc.MipLevels = 1;
        copyDesc.ArraySize = 1;
        copyDesc.Format = viewFmt;
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        copyDesc.CPUAccessFlags = copyDesc.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, &s_hud_merged_hdr_texture)) ||
            FAILED(device->CreateShaderResourceView(s_hud_merged_hdr_texture.Get(), nullptr, &s_hud_merged_hdr_srv)) ||
            FAILED(device->CreateRenderTargetView(s_hud_merged_hdr_texture.Get(), nullptr, &s_hud_merged_hdr_rtv))) {
            s_hud_merged_hdr_texture.Reset();
            s_hud_merged_hdr_srv.Reset();
            s_hud_merged_hdr_rtv.Reset();
            return nullptr;
        }
        s_hud_merged_hdr_width = targetW;
        s_hud_merged_hdr_height = targetH;
        s_hud_merged_hdr_format = viewFmt;
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> tempSrcSrv;
    ID3D11ShaderResourceView* srcSrvToUse = hdrSourceSrv;
    if (!srcSrvToUse && (targetW != desc.Width || targetH != desc.Height)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = viewFmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (SUCCEEDED(device->CreateShaderResourceView(hdrTex.Get(), &sd, &tempSrcSrv))) {
            srcSrvToUse = tempSrcSrv.Get();
        }
    }

    if (srcSrvToUse) {
        mergeHudIntoHdrTarget(context, s_hud_merged_hdr_rtv.Get(), targetW, targetH, srcSrvToUse);
    } else {
        context->CopyResource(s_hud_merged_hdr_texture.Get(), hdrTex.Get());
        mergeHudIntoHdrTarget(context, s_hud_merged_hdr_rtv.Get(), targetW, targetH, nullptr);
    }
    s_hud_merged_into_hdr_this_frame = true;
    s_hud_merged_hdr_source_res = hdrSourceRes;
    s_hud_merged_hdr_frame = current_frame;
    return s_hud_merged_hdr_srv.Get();
}

void mergeHudIntoSceneHdr(ID3D11DeviceContext* context) {
    if (!context || !motion_color_source || !scene_hdr_color_source) return;
    D3D11_TEXTURE2D_DESC hdrDesc{};
    scene_hdr_color_source->GetDesc(&hdrDesc);
    const UINT sceneW = s_observed_scene_width.load(std::memory_order_relaxed);
    const UINT sceneH = s_observed_scene_height.load(std::memory_order_relaxed);
    if (!hdrDesc.Width || !hdrDesc.Height ||
        (sceneW > 0 && hdrDesc.Width != sceneW) ||
        (sceneH > 0 && hdrDesc.Height != sceneH)) {
        return;
    }
    mergeHudIntoHdrTarget(context, motion_color_source.Get(), hdrDesc.Width, hdrDesc.Height);
    s_hud_merged_into_hdr_this_frame = true;
    s_hud_merged_into_main_hdr_frame = last_present_frame.load(std::memory_order_relaxed);
}

bool observePostHudDrawAndTonemap(ID3D11DeviceContext* context, char kind, UINT count,
                                  UINT instance_count, UINT start_index_or_vertex,
                                  INT base_vertex, UINT start_instance) {
    const auto current_frame = last_present_frame.load(std::memory_order_relaxed);
    if (s_hdr_slot0_seen_frame != current_frame || motion_color_bound || s_upscale_evaluated_this_frame) {
        return false;
    }
    if (!s_ldr_tonemap_candidate_bound && !s_post_ldr_backbuffer_bound) {
        return false;
    }
    if (s_verified_ldr_tonemap_frame == current_frame) {
        if (s_hud_draws_replayed_frame == 0 || !s_post_ldr_backbuffer_bound) {
            return false;
        }
        if (!isPauseOrModalMenuActive(current_frame) && count < 300 && count != 4 && count != 6) {
            return false;
        }
    } else if (!s_post_ldr_backbuffer_bound && (count < 3 || count > 6 || instance_count > 1)) {
        return false;
    }

    std::uint64_t vs_hash = 0, ps_hash = 0;
    getFastBoundShaderHashes(context, vs_hash, ps_hash);

    auto issueDraw = [&]() {
        switch (kind) {
        case 'X':
            if (auto* fn = resolveOriginalDrawIndexedInstanced(context))
                fn(context, count, instance_count, start_index_or_vertex, base_vertex, start_instance);
            break;
        case 'I':
            if (auto* fn = resolveOriginalDrawIndexed(context))
                fn(context, count, start_index_or_vertex, base_vertex);
            break;
        case 'N':
            if (auto* fn = resolveOriginalDrawInstanced(context))
                fn(context, count, instance_count, start_index_or_vertex, start_instance);
            break;
        case 'D':
            if (auto* fn = resolveOriginalDraw(context))
                fn(context, count, start_index_or_vertex);
            break;
        }
    };

    // Detect in-cockpit Station Services or ESC Pause Menu frosted backdrop / wash
    // ONLY on frames where Pass 35 Cockpit HUD draws are active (s_hud_draws_replayed_frame > 0),
    // so the Main Menu (which has 0 Cockpit HUD draws) is never mistaken for an in-cockpit pause menu:
    // - C4B4B334B26E81A9 / 0146ABCC53240479: frosted blur base of Station Services & ESC Pause Menu
    // - A888D51024D9798E / 015EF9349EC097E8 (n=360): tinted Station Services curved panel
    // - 5DAD41AA66757CE3 / 25D5F73A4616093B: Station Services curved overlay mesh
    // - 4EF6DDB075A927FA: ESC menu darkening multiply wash
    const bool isPostTonemapMenuStage =
        s_hud_draws_replayed_frame > 0 &&
        (s_post_ldr_backbuffer_bound ||
         s_ldr_tonemap_candidate_bound ||
         (s_verified_ldr_tonemap_frame == current_frame ||
          scene_ldr_frame == current_frame ||
          s_pass53_blit_seen_frame == current_frame));
    const bool isModalOrEscMenuDraw =
        isPostTonemapMenuStage &&
        ((vs_hash == 0xC4B4B334B26E81A9ull && ps_hash == 0x0146ABCC53240479ull) ||
         (vs_hash == 0xA888D51024D9798Eull && ps_hash == 0x015EF9349EC097E8ull && count >= 300) ||
         (vs_hash == 0x5DAD41AA66757CE3ull && ps_hash == 0x25D5F73A4616093Bull) ||
         (vs_hash == 0x4EF6DDB075A927FAull));
    if (isModalOrEscMenuDraw) {
        s_pause_menu_seen_frame = current_frame;
    }

    // Pass 54 Post-Tonemap UI Occlusion (active ONLY when an in-cockpit modal/pause menu is open):
    // Replay the alpha attenuation of modal menu backdrop/panel/card draws (C4B4B334 frosted shadow,
    // A888D510 Starport Services / ESC Menu panels, 5DAD41AA curved mesh, 4EF6DDB0 multiply wash,
    // 1E8D0137 widget cards) into s_hud_tonemapped_rtv.a (T_54 = prod(1 - alpha_54)) so
    // UiCompositePass knows the exact hardware transmittance of Pass 54 even where both the 3D
    // background (cLow) and the Pass 54 card/shadow (cGame) are dark, with zero overhead during gameplay.
    const bool isPass54ModalBackdropOrCard =
        (vs_hash == 0xC4B4B334B26E81A9ull ||
         vs_hash == 0xA888D51024D9798Eull ||
         vs_hash == 0x5DAD41AA66757CE3ull ||
         vs_hash == 0x4EF6DDB075A927FAull ||
         vs_hash == 0x1E8D013722FE71B8ull);
    if (isPauseOrModalMenuActive(current_frame) && isPass54ModalBackdropOrCard &&
        (s_verified_ldr_tonemap_frame == current_frame || s_post_ldr_backbuffer_bound) &&
        s_hud_tonemapped_valid_frame == current_frame && s_hud_tonemapped_rtv &&
        (s_hud_pass54_tone_atten_blend || s_hud_pass54_mul_wash_blend)) {
        Microsoft::WRL::ComPtr<ID3D11BlendState> origBlend;
        FLOAT origBlendFactor[4]{};
        UINT origSampleMask = 0xFFFFFFFF;
        context->OMGetBlendState(origBlend.GetAddressOf(), origBlendFactor, &origSampleMask);
        D3D11_BLEND_DESC bd{};
        if (origBlend) origBlend->GetDesc(&bd);
        const D3D11_BLEND destBlend = bd.RenderTarget[0].DestBlend;
        const bool isOverOrMulBlend =
            origBlend && bd.RenderTarget[0].BlendEnable &&
            (destBlend == D3D11_BLEND_INV_SRC_ALPHA || destBlend == D3D11_BLEND_SRC_COLOR);
        if (isOverOrMulBlend) {
            ID3D11RenderTargetView* origRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            Microsoft::WRL::ComPtr<ID3D11DepthStencilView> origDsv;
            context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, origRtvs, origDsv.GetAddressOf());
            bool isPostToneTarget = false;
            UINT rtvW = 0, rtvH = 0;
            if (origRtvs[0]) {
                Microsoft::WRL::ComPtr<ID3D11Resource> rtvRes;
                origRtvs[0]->GetResource(&rtvRes);
                Microsoft::WRL::ComPtr<ID3D11Texture2D> postTex;
                if (rtvRes && SUCCEEDED(rtvRes.As(&postTex)) && postTex) {
                    D3D11_TEXTURE2D_DESC postDesc{};
                    postTex->GetDesc(&postDesc);
                    rtvW = postDesc.Width;
                    rtvH = postDesc.Height;
                    const UINT swapW = s_swapchain_width.load(std::memory_order_relaxed);
                    const UINT swapH = s_swapchain_height.load(std::memory_order_relaxed);
                    const UINT scW = s_observed_scene_width.load(std::memory_order_relaxed);
                    const UINT scH = s_observed_scene_height.load(std::memory_order_relaxed);
                    const bool isBackbufferRes =
                        (rtvRes.Get() == s_swapchain_backbuffer.load(std::memory_order_relaxed));
                    const bool isFullLdrScreen =
                        (postDesc.SampleDesc.Count == 1 &&
                         isLdrSceneColorFormat(postDesc.Format) &&
                         postTex.Get() != scene_ldr_source.Get() &&
                         ((swapW > 0 && rtvW == swapW && rtvH == swapH) ||
                          (scW > 0 && rtvW == scW && rtvH == scH)));
                    isPostToneTarget = isBackbufferRes || isFullLdrScreen;
                }
            }
            if (isPostToneTarget) {
                const OMSetRenderTargetsFn rawSet = resolveOriginalOM(context);
                ID3D11BlendState* attenBlend =
                    (destBlend == D3D11_BLEND_SRC_COLOR)
                        ? s_hud_pass54_mul_wash_blend.Get()
                        : s_hud_pass54_tone_atten_blend.Get();
                if (rawSet && attenBlend && rtvW > 0 && rtvH > 0 &&
                    s_hud_mask_width > 0 && s_hud_mask_height > 0) {
                    issueDraw();
                    rearmHudDrawProbe(context);

                    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> origDs;
                    UINT origStencilRef = 0;
                    context->OMGetDepthStencilState(origDs.GetAddressOf(), &origStencilRef);

                    UINT numVps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
                    D3D11_VIEWPORT origVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
                    context->RSGetViewports(&numVps, origVps);
                    UINT numScissors = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
                    D3D11_RECT origScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
                    context->RSGetScissorRects(&numScissors, origScissors);

                    const float scaleX = static_cast<float>(s_hud_mask_width) / static_cast<float>(rtvW);
                    const float scaleY = static_cast<float>(s_hud_mask_height) / static_cast<float>(rtvH);
                    const bool needScale = (rtvW != s_hud_mask_width || rtvH != s_hud_mask_height);
                    if (needScale && numVps > 0) {
                        D3D11_VIEWPORT scaledVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
                        for (UINT i = 0; i < numVps; ++i) {
                            scaledVps[i] = origVps[i];
                            scaledVps[i].TopLeftX *= scaleX;
                            scaledVps[i].TopLeftY *= scaleY;
                            scaledVps[i].Width *= scaleX;
                            scaledVps[i].Height *= scaleY;
                        }
                        context->RSSetViewports(numVps, scaledVps);
                    }
                    if (needScale && numScissors > 0) {
                        D3D11_RECT scaledScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
                        const LONG maxW = static_cast<LONG>(s_hud_mask_width);
                        const LONG maxH = static_cast<LONG>(s_hud_mask_height);
                        for (UINT i = 0; i < numScissors; ++i) {
                            scaledScissors[i].left = std::clamp(static_cast<LONG>(std::lround(origScissors[i].left * scaleX)), 0L, maxW);
                            scaledScissors[i].top = std::clamp(static_cast<LONG>(std::lround(origScissors[i].top * scaleY)), 0L, maxH);
                            scaledScissors[i].right = std::clamp(static_cast<LONG>(std::lround(origScissors[i].right * scaleX)), 0L, maxW);
                            scaledScissors[i].bottom = std::clamp(static_cast<LONG>(std::lround(origScissors[i].bottom * scaleY)), 0L, maxH);
                        }
                        context->RSSetScissorRects(numScissors, scaledScissors);
                    }

                    ID3D11RenderTargetView* toneRtv = s_hud_tonemapped_rtv.Get();
                    rawSet(context, 1, &toneRtv, nullptr);
                    if (s_hud_mask_ds_state_no_depth) {
                        context->OMSetDepthStencilState(s_hud_mask_ds_state_no_depth.Get(), 0);
                    }
                    const FLOAT oneFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                    context->OMSetBlendState(attenBlend, oneFactor, origSampleMask);

                    issueDraw();
                    rearmHudDrawProbe(context);
                    s_hud_tonemapped_alpha_dirty = true;

                    if (needScale) {
                        context->RSSetViewports(numVps, origVps);
                        if (numScissors > 0) {
                            context->RSSetScissorRects(numScissors, origScissors);
                        }
                    }
                    context->OMSetDepthStencilState(origDs.Get(), origStencilRef);
                    context->OMSetBlendState(origBlend.Get(), origBlendFactor, origSampleMask);
                    rawSet(context, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, origRtvs, origDsv.Get());
                    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
                        if (origRtvs[i]) origRtvs[i]->Release();
                    }
                    abandonAmortizedAndRearmHudProbe(context);
                    return true;
                }
            }
            for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
                if (origRtvs[i]) origRtvs[i]->Release();
            }
        }
    }

    if (s_hdr_slot0_seen_frame != current_frame || s_verified_ldr_tonemap_frame == current_frame) {
        return false;
    }

    if (s_post_ldr_backbuffer_bound || motion_color_bound || !scene_hdr_color_source ||
        count < 3 || count > 6 || instance_count > 1) {
        return false;
    }

    // Inspect the currently bound RTV0 first to distinguish Pass 36 Bloom (half-scene HDR float)
    // from Pass 52 Tonemapping (full-scene LDR target with dsv == nullptr).
    const UINT sceneW = s_observed_scene_width.load(std::memory_order_relaxed);
    const UINT sceneH = s_observed_scene_height.load(std::memory_order_relaxed);
    if (sceneW == 0 || sceneH == 0) {
        return false;
    }

    bool isLdrTonemapDraw = false;
    bool isHalfSceneHdrDraw = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> rtvTex;
    {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> curRtv;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> curDsv;
        context->OMGetRenderTargets(1, curRtv.GetAddressOf(), curDsv.GetAddressOf());
        if (!curRtv || curDsv != nullptr) {
            return false;
        }
        Microsoft::WRL::ComPtr<ID3D11Resource> rtvRes;
        curRtv->GetResource(&rtvRes);
        if (!rtvRes || rtvRes.Get() == s_swapchain_backbuffer.load(std::memory_order_relaxed)) {
            return false;
        }
        if (FAILED(rtvRes.As(&rtvTex)) || !rtvTex) {
            return false;
        }
        D3D11_TEXTURE2D_DESC rtvDesc{};
        rtvTex->GetDesc(&rtvDesc);
        if (rtvDesc.SampleDesc.Count != 1) {
            return false;
        }
        if (rtvDesc.Width == sceneW && rtvDesc.Height == sceneH &&
            isLdrSceneColorFormat(rtvDesc.Format)) {
            isLdrTonemapDraw = true;
        } else {
            const bool isHalfScene =
                (rtvDesc.Width * 2 >= sceneW - 2 && rtvDesc.Width * 2 <= sceneW + 2 &&
                 rtvDesc.Height * 2 >= sceneH - 2 && rtvDesc.Height * 2 <= sceneH + 2);
            const bool isHdrFloat =
                (rtvDesc.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
                 rtvDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                 rtvDesc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS);
            isHalfSceneHdrDraw = (isHalfScene && isHdrFloat);
        }
        if (!isLdrTonemapDraw && !isHalfSceneHdrDraw) {
            return false;
        }
    }

    // Locate the full-scene HDR input SRV slot across PS t0..t7.
    // During camera rotation/movement, Camera Motion Blur or DoF may write an intermediate
    // full-screen R11G11B10_FLOAT texture (s_last_full_hdr_post_source) that Pass 36 Bloom
    // and Pass 52 Tonemapping read instead of scene_hdr_color_source.
    ID3D11ShaderResourceView* psSrvs[8]{};
    context->PSGetShaderResources(0, 8, psSrvs);
    int hdrSlot = -1;
    Microsoft::WRL::ComPtr<ID3D11Resource> hdrSlotRes;
    ID3D11Resource* const primaryHdrRes = static_cast<ID3D11Resource*>(scene_hdr_color_source.Get());
    ID3D11Resource* const postHdrRes =
        (s_last_full_hdr_post_frame == current_frame && s_last_full_hdr_post_source)
            ? static_cast<ID3D11Resource*>(s_last_full_hdr_post_source.Get())
            : nullptr;

    for (int i = 0; i < 8; ++i) {
        if (!psSrvs[i]) continue;
        Microsoft::WRL::ComPtr<ID3D11Resource> res;
        psSrvs[i]->GetResource(&res);
        if (res && (res.Get() == primaryHdrRes || (postHdrRes && res.Get() == postHdrRes))) {
            hdrSlot = i;
            hdrSlotRes = res;
            break;
        }
    }
    if (hdrSlot < 0) {
        // Fallback: check for any bound full-screen (sceneW x sceneH) HDR float render-target SRV
        // (preferring slot 1 where Pass 52 Tonemapping binds the HDR scene, then 0..7).
        const int searchOrder[8] = {1, 0, 2, 3, 4, 5, 6, 7};
        for (int idx = 0; idx < 8; ++idx) {
            const int i = searchOrder[idx];
            if (!psSrvs[i]) continue;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            psSrvs[i]->GetDesc(&sd);
            if (sd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) continue;
            Microsoft::WRL::ComPtr<ID3D11Resource> res;
            psSrvs[i]->GetResource(&res);
            if (!res) continue;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
            if (FAILED(res.As(&tex)) || !tex) continue;
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            const bool isHdrFmt =
                (td.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
                 td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                 td.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS);
            if (isHdrFmt && td.Width == sceneW && td.Height == sceneH &&
                td.SampleDesc.Count == 1 && (td.BindFlags & D3D11_BIND_RENDER_TARGET)) {
                hdrSlot = i;
                hdrSlotRes = res;
                break;
            }
        }
    }

    if (hdrSlot < 0) {
        for (int i = 0; i < 8; ++i) {
            if (psSrvs[i]) psSrvs[i]->Release();
        }
        return false;
    }

    if (isLdrTonemapDraw && rtvTex) {
        s_ldr_tonemap_candidate_bound = true;
        s_verified_ldr_tonemap_frame = current_frame;
        if (scene_ldr_source.Get() != rtvTex.Get() || scene_ldr_frame != current_frame) {
            scene_ldr_source = rtvTex;
            scene_ldr_frame = current_frame;
            s_recent_scene_ldr_source = rtvTex;
            s_recent_scene_ldr_frame = current_frame;
            ++s_diag_ldr_updates;
        }
    }

    if (!isLdrTonemapDraw) {
        // Pass 36 Bloom Pyramid level 0 (isHalfSceneHdrDraw, DFED8E1C / 143AAE05):
        // Build H' = H * T + HUD in s_hud_merged_hdr_texture and bind H' at PS slot hdrSlot
        // for this single Bloom downsample draw, then restore the HUD-free scene SRV!
        if (isHalfSceneHdrDraw && s_hud_draws_replayed_frame > 0 &&
            !s_hud_merged_into_hdr_this_frame && !isPauseOrModalMenuActive(current_frame)) {
            ID3D11ShaderResourceView* mergedSrv =
                buildHudMergedHdrScratchForBloom(context, hdrSlotRes.Get(), psSrvs[hdrSlot]);
            if (mergedSrv) {
                context->PSSetShaderResources(static_cast<UINT>(hdrSlot), 1, &mergedSrv);
                issueDraw();
                rearmHudDrawProbe(context);
                ID3D11ShaderResourceView* origHdrSrv = psSrvs[hdrSlot];
                context->PSSetShaderResources(static_cast<UINT>(hdrSlot), 1, &origHdrSrv);
                for (int i = 0; i < 8; ++i) {
                    if (psSrvs[i]) psSrvs[i]->Release();
                }
                abandonAmortizedAndRearmHudProbe(context);
                return true;
            }
        }
        for (int i = 0; i < 8; ++i) {
            if (psSrvs[i]) psSrvs[i]->Release();
        }
        return false;
    }

    if (s_hud_draws_replayed_frame == 0) {
        for (int i = 0; i < 8; ++i) {
            if (psSrvs[i]) psSrvs[i]->Release();
        }
        return false;
    }

    // Pass 52 Tonemapping reached: close Pass 35 HUD capture for this frame.
    // Do NOT merge the Pass 35 Cockpit HUD into scene_hdr_color_source or scene_ldr_source when
    // Station Services or the ESC Pause Menu is active: keeping scene_ldr_source and the Bloom/blur
    // pyramid HUD-free ensures that Pass 54 modal menus and their frosted blur backdrop never suffer
    // from Cockpit HUD bleed-through.
    s_hud_mask_active_pass = false;

    // 1. Execute the game's Pass 52 Tonemap draw for the HUD-free 3D scene + HUD-inclusive Bloom halo
    issueDraw();
    rearmHudDrawProbe(context);

    // 2. Re-issue the exact same Pass 52 Tonemap draw (with Auto-Exposure VS t0, 3D LUT PS t0,
    //    Bloom pyramid PS t2, and cb2) for the native-resolution merged HDR (H_scene * T + L_hud)
    //    into s_hud_tonemapped_rtv. Evaluating Pass 52 Tonemap on (H_scene * T + L_hud) with the
    //    game's Bloom SRV intact avoids the E8948389 rational filmic curve's <0.0013 black-toe
    //    crush on translucent HUD fills/holograms and preserves in-HUD bloom and 3D LUT grading.
    const OMSetRenderTargetsFn rawSet = resolveOriginalOM(context);
    if (rawSet && s_hud_tonemapped_rtv && s_hud_mask_srv && s_zero_hdr_srv &&
        s_hud_mask_width > 0 && s_hud_mask_height > 0 &&
        s_hud_tonemapped_valid_frame != current_frame) {
        ID3D11ShaderResourceView* mergedHudHdrSrv =
            buildHudMergedHdrScratchForBloom(context, hdrSlotRes.Get(), psSrvs[hdrSlot]);
        ID3D11ShaderResourceView* hudToneSrcSrv =
            mergedHudHdrSrv ? mergedHudHdrSrv : s_hud_mask_srv.Get();

        ID3D11RenderTargetView* origRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> origDsv;
        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, origRtvs, origDsv.GetAddressOf());

        Microsoft::WRL::ComPtr<ID3D11BlendState> origBlend;
        FLOAT origBlendFactor[4]{};
        UINT origSampleMask = 0xFFFFFFFF;
        context->OMGetBlendState(origBlend.GetAddressOf(), origBlendFactor, &origSampleMask);

        UINT numVps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_VIEWPORT origVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        context->RSGetViewports(&numVps, origVps);
        UINT numScissors = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_RECT origScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        context->RSGetScissorRects(&numScissors, origScissors);

        ID3D11ShaderResourceView* hudSrvs[8]{};
        for (int i = 0; i < 8; ++i) {
            if (i == hdrSlot) {
                hudSrvs[i] = hudToneSrcSrv;
            } else if (ps_hash == 0xDE65BFFF2F12ECC6ull && i == 1) {
                // DoF 2 composite (DE65): replace DoF blur slot t1 with merged HUD+scene SRV so (1-f)*H + f*H = H
                hudSrvs[i] = hudToneSrcSrv;
            } else {
                hudSrvs[i] = psSrvs[i];
            }
        }

        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> origToneDs;
        UINT origToneStencilRef = 0;
        context->OMGetDepthStencilState(origToneDs.GetAddressOf(), &origToneStencilRef);

        ID3D11RenderTargetView* toneRtv = s_hud_tonemapped_rtv.Get();
        if (s_hud_tonemapped_alpha_dirty || isPauseOrModalMenuActive(current_frame)) {
            const float oneAlphaClear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            context->ClearRenderTargetView(toneRtv, oneAlphaClear);
            s_hud_tonemapped_alpha_dirty = false;
        }
        const bool useEarlyZ =
            (s_hud_earlyz_valid_frame == current_frame && s_hud_earlyz_dsv && s_hud_earlyz_test_ds);
        rawSet(context, 1, &toneRtv, useEarlyZ ? s_hud_earlyz_dsv.Get() : nullptr);
        if (useEarlyZ) {
            context->OMSetDepthStencilState(s_hud_earlyz_test_ds.Get(), 0);
        }
        const D3D11_VIEWPORT hudVp{0.0f, 0.0f, static_cast<float>(s_hud_mask_width),
                                   static_cast<float>(s_hud_mask_height),
                                   useEarlyZ ? 0.5f : 0.0f, useEarlyZ ? 0.5f : 1.0f};
        context->RSSetViewports(1, &hudVp);
        const D3D11_RECT hudScissor{0, 0, static_cast<LONG>(s_hud_mask_width), static_cast<LONG>(s_hud_mask_height)};
        context->RSSetScissorRects(1, &hudScissor);
        const FLOAT oneFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        context->OMSetBlendState(s_hud_tone_rgb_only_blend.Get(), oneFactor, 0xFFFFFFFF);
        context->PSSetShaderResources(0, 8, hudSrvs);

        issueDraw();
        rearmHudDrawProbe(context);
        s_hud_tonemapped_valid_frame = current_frame;

        context->PSSetShaderResources(0, 8, psSrvs);
        context->RSSetViewports(numVps, origVps);
        if (numScissors > 0) {
            context->RSSetScissorRects(numScissors, origScissors);
        }
        if (useEarlyZ) {
            context->OMSetDepthStencilState(origToneDs.Get(), origToneStencilRef);
        }
        context->OMSetBlendState(origBlend.Get(), origBlendFactor, origSampleMask);
        rawSet(context, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, origRtvs, origDsv.Get());
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            if (origRtvs[i]) origRtvs[i]->Release();
        }
    }

    for (int i = 0; i < 8; ++i) {
        if (psSrvs[i]) psSrvs[i]->Release();
    }
    abandonAmortizedAndRearmHudProbe(context);
    return true;
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
    const auto forward = resolveOriginalVSSetConstantBuffers(context);
    if (!forward) return;

    const bool immediate = isObservedImmediateContext(context);
    ID3D11Buffer* const* effective_buffers = buffers;
    forward(context, start, count, buffers);
    if (!immediate) return;
    rearmHudDrawProbe(context);
    if (!vs_buffer_probe_active.load(std::memory_order_acquire)) return;
    vs_buffer_calls.fetch_add(1, std::memory_order_relaxed);
    if (start > 1 || count <= 1 - start) return;
    ID3D11Buffer* buffer = effective_buffers ? effective_buffers[1 - start] : nullptr;
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

static inline bool isHudDrawInterceptWindowActive() {
    if (motion_pair_active || draw_probe_active.load(std::memory_order_relaxed)) return true;
    if (!hud_draw_probe_active.load(std::memory_order_relaxed) || s_upscale_evaluated_this_frame) return false;
    const auto cur = last_present_frame.load(std::memory_order_relaxed);
    if (s_hdr_slot0_seen_frame != cur) return false;
    if (s_verified_ldr_tonemap_frame == cur && s_hud_draws_replayed_frame == 0) return false;
    return true;
}

static inline bool shouldInspectHudOrTonemapDraw() {
    return (s_hud_mask_active_pass && motion_color_bound) ||
           s_ldr_tonemap_candidate_bound ||
           s_post_ldr_backbuffer_bound ||
           motion_pair_active ||
           draw_probe_active.load(std::memory_order_relaxed);
}

void STDMETHODCALLTYPE observedDrawIndexed(ID3D11DeviceContext* context, UINT count,
    UINT start, INT base) {
    const bool immediate = isObservedImmediateContext(context);
    if (!immediate || !isHudDrawInterceptWindowActive()) {
        if (const auto forward = resolveOriginalDrawIndexed(context)) forward(context, count, start, base);
        return;
    }
    if (!shouldInspectHudOrTonemapDraw()) {
        if (const auto forward = resolveOriginalDrawIndexed(context)) forward(context, count, start, base);
        abandonAmortizedAndRearmHudProbe(context);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_scene_ctx_mutex);
    const auto hudResult = observeHudDraw(context, 'I', 1);
    if (draw_probe_active.load(std::memory_order_acquire)) {
        draw_indexed_calls.fetch_add(1, std::memory_order_relaxed);
        if (vs_scene_buffer_bound.load(std::memory_order_relaxed))
            draw_indexed_scene_calls.fetch_add(1, std::memory_order_relaxed);
        observeFirstSceneDraw(context);
    }
    bool redirected = false;
    if (hudResult == HudDrawResult::HudReplay) {
        redirected = replayHudDraw(context, 'I', count, 1, start, base, 0);
        if (!redirected) {
            ++s_diag_hud_declined_draws;
            if (auto* stat = findOrAddHudPairStat(s_current_hud_vs_hash, s_current_hud_ps_hash)) ++stat->declined;
        }
    }
    if (!redirected) {
        redirected = observePostHudDrawAndTonemap(context, 'I', count, 1, start, base, 0);
    }
    if (!redirected) {
        if (const auto forward = resolveOriginalDrawIndexed(context)) forward(context, count, start, base);
        abandonAmortizedAndRearmHudProbe(context);
    }
}

void STDMETHODCALLTYPE observedDraw(ID3D11DeviceContext* context, UINT count, UINT start) {
    const bool immediate = isObservedImmediateContext(context);
    if (!immediate || !isHudDrawInterceptWindowActive()) {
        if (const auto forward = resolveOriginalDraw(context)) forward(context, count, start);
        return;
    }
    if (!shouldInspectHudOrTonemapDraw()) {
        if (const auto forward = resolveOriginalDraw(context)) forward(context, count, start);
        abandonAmortizedAndRearmHudProbe(context);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_scene_ctx_mutex);
    const auto hudResult = observeHudDraw(context, 'D', 1);
    if (draw_probe_active.load(std::memory_order_acquire)) {
        draw_calls.fetch_add(1, std::memory_order_relaxed);
        if (vs_scene_buffer_bound.load(std::memory_order_relaxed))
            draw_scene_calls.fetch_add(1, std::memory_order_relaxed);
        observeFirstSceneDraw(context);
    }
    bool redirected = false;
    if (hudResult == HudDrawResult::HudReplay) {
        redirected = replayHudDraw(context, 'D', count, 1, start, 0, 0);
        if (!redirected) {
            ++s_diag_hud_declined_draws;
            if (auto* stat = findOrAddHudPairStat(s_current_hud_vs_hash, s_current_hud_ps_hash)) ++stat->declined;
        }
    }
    if (!redirected) {
        redirected = observePostHudDrawAndTonemap(context, 'D', count, 1, start, 0, 0);
    }
    if (!redirected) {
        if (const auto forward = resolveOriginalDraw(context)) forward(context, count, start);
        abandonAmortizedAndRearmHudProbe(context);
    }
}

void STDMETHODCALLTYPE observedDrawIndexedInstanced(ID3D11DeviceContext* context,
    UINT index_count, UINT instance_count, UINT start_index, INT base_vertex,
    UINT start_instance) {
    const bool immediate = isObservedImmediateContext(context);
    if (!immediate || !isHudDrawInterceptWindowActive()) {
        if (const auto forward = resolveOriginalDrawIndexedInstanced(context)) {
            forward(context, index_count, instance_count, start_index, base_vertex, start_instance);
        }
        return;
    }
    if (!shouldInspectHudOrTonemapDraw()) {
        if (const auto forward = resolveOriginalDrawIndexedInstanced(context)) {
            forward(context, index_count, instance_count, start_index, base_vertex, start_instance);
        }
        abandonAmortizedAndRearmHudProbe(context);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_scene_ctx_mutex);
    const auto hudResult = observeHudDraw(context, 'X', instance_count);
    if (draw_probe_active.load(std::memory_order_acquire)) {
        draw_indexed_instanced_calls.fetch_add(1, std::memory_order_relaxed);
        if (vs_scene_buffer_bound.load(std::memory_order_relaxed))
            draw_indexed_instanced_scene_calls.fetch_add(1, std::memory_order_relaxed);
        observeFirstSceneDraw(context);
    }
    bool redirected = false;
    if (hudResult == HudDrawResult::HudReplay) {
        redirected = replayHudDraw(context, 'X', index_count, instance_count,
            start_index, base_vertex, start_instance);
        if (!redirected) {
            ++s_diag_hud_declined_draws;
            if (auto* stat = findOrAddHudPairStat(s_current_hud_vs_hash, s_current_hud_ps_hash)) ++stat->declined;
        }
    }
    if (!redirected) {
        redirected = observePostHudDrawAndTonemap(context, 'X', index_count, instance_count,
            start_index, base_vertex, start_instance);
    }
    if (!redirected) {
        if (const auto forward = resolveOriginalDrawIndexedInstanced(context)) {
            forward(context, index_count, instance_count, start_index, base_vertex, start_instance);
        }
        abandonAmortizedAndRearmHudProbe(context);
        if (hudResult == HudDrawResult::GlassReplay) {
            replayGlassDraw(context, index_count, instance_count,
                start_index, base_vertex, start_instance);
            abandonAmortizedAndRearmHudProbe(context);
        }
    }
}

void STDMETHODCALLTYPE observedDrawInstanced(ID3D11DeviceContext* context,
    UINT vertex_count, UINT instance_count, UINT start_vertex, UINT start_instance) {
    const bool immediate = isObservedImmediateContext(context);
    if (!immediate || !isHudDrawInterceptWindowActive()) {
        if (const auto forward = resolveOriginalDrawInstanced(context)) {
            forward(context, vertex_count, instance_count, start_vertex, start_instance);
        }
        return;
    }
    if (!shouldInspectHudOrTonemapDraw()) {
        if (const auto forward = resolveOriginalDrawInstanced(context)) {
            forward(context, vertex_count, instance_count, start_vertex, start_instance);
        }
        abandonAmortizedAndRearmHudProbe(context);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_scene_ctx_mutex);
    const auto hudResult = observeHudDraw(context, 'N', instance_count);
    if (draw_probe_active.load(std::memory_order_acquire)) {
        draw_instanced_calls.fetch_add(1, std::memory_order_relaxed);
        if (vs_scene_buffer_bound.load(std::memory_order_relaxed))
            draw_instanced_scene_calls.fetch_add(1, std::memory_order_relaxed);
        observeFirstSceneDraw(context);
    }
    bool redirected = false;
    if (hudResult == HudDrawResult::HudReplay) {
        redirected = replayHudDraw(context, 'N', vertex_count, instance_count, start_vertex, 0, start_instance);
        if (!redirected) {
            ++s_diag_hud_declined_draws;
            if (auto* stat = findOrAddHudPairStat(s_current_hud_vs_hash, s_current_hud_ps_hash)) ++stat->declined;
        }
    }
    if (!redirected) {
        redirected = observePostHudDrawAndTonemap(context, 'N', vertex_count, instance_count,
            start_vertex, 0, start_instance);
    }
    if (!redirected) {
        if (const auto forward = resolveOriginalDrawInstanced(context)) {
            forward(context, vertex_count, instance_count, start_vertex, start_instance);
        }
        abandonAmortizedAndRearmHudProbe(context);
    }
}

// In Windows d3d11.dll, CContext::TID3D11DeviceContext_Draw*_<0> sets m_bAmortized = true and swaps
// vtable[12,13,20,21] to Draw*_Amortized<0>. Then, when the game calls any pipeline state-setting
// or resource-updating method (VSSetShader, PSSetShader, IASetInputLayout, IASetVertexBuffers,
// IASetIndexBuffer, IASetPrimitiveTopology, VSSetConstantBuffers, PSSetConstantBuffers,
// PSSetShaderResources, VSSetShaderResources, Samplers, BlendState, DepthStencilState,
// RasterizerState, Viewports, ScissorRects, Copy/Update/Clear), d3d11.dll calls
// CContext::AbandonAmortized() which sets m_bAmortized = false and overwrites vtable[12,13,20,21]
// back to Draw*_<0>. By hooking these state-setting methods to call rearmHudDrawProbe() immediately
// upon return from d3d11.dll, vtable[12,13,20,21] is guaranteed to point to observedDraw* before
// the very next Draw* instruction in EliteDangerous64.exe.
void STDMETHODCALLTYPE observedOMSetRenderTargets(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* dsv);
void STDMETHODCALLTYPE observedOMSetRenderTargetsAndUnorderedAccessViews(
    ID3D11DeviceContext* context, UINT numRTVs, ID3D11RenderTargetView* const* targets,
    ID3D11DepthStencilView* dsv, UINT uavStartSlot, UINT numUAVs,
    ID3D11UnorderedAccessView* const* uavs, const UINT* initialCounts);

void installHudStateRearmHooks() {
    // No-op: HookMode::LiveCopy maintains a private vtable with 16-byte live trampolines,
    // so d3d11.dll's CContext::AbandonAmortized() never unhooks Draw or OM slots.
}

void restoreHudStateRearmHooks() {
    // No-op: state-setting methods are no longer hooked in-place.
}

void abandonAmortizedAndRearmHudProbe(ID3D11DeviceContext* context) {
    if (!context) return;
    rearmHudDrawProbe(context);
}

void beginHudDrawProbe() {
    const size_t tblCount = s_hooked_table_count.load(std::memory_order_acquire);
    if ((!patched_table && tblCount == 0) || hud_draw_hook_mask ||
        draw_probe_active.load(std::memory_order_acquire)) return;
    auto installOnEntry = [&](size_t idx, size_t slot, auto& entry_field, auto& original_fn, auto observer) -> bool {
        auto& entry = s_hooked_tables[idx];
        if (!entry.is_immediate || !entry.table) return false;
        if (s_context_vtable_hooks[idx] && s_context_vtable_hooks[idx]->isInitialized()) {
            void* fwd = nullptr;
            if (s_context_vtable_hooks[idx]->hook(entry.context, slot,
                    reinterpret_cast<void*>(observer), &fwd, s_context_vtable_hooks[idx]->mode())) {
                entry_field = reinterpret_cast<decltype(observer)>(fwd);
                if (!original_fn.load(std::memory_order_acquire)) {
                    original_fn.store(entry_field, std::memory_order_release);
                }
                return true;
            }
        }
        auto forward = reinterpret_cast<decltype(observer)>(entry.table[slot]);
        if (!forward) return false;
        if (forward == observer) return true;
        entry_field = forward;
        if (!original_fn.load(std::memory_order_acquire)) {
            original_fn.store(forward, std::memory_order_release);
        }
        return patchSlot(entry.table, slot, reinterpret_cast<void*>(forward),
            reinterpret_cast<void*>(observer));
    };
    for (size_t i = 0; i < tblCount; ++i) {
        if (!s_hooked_tables[i].is_immediate) continue;
        installOnEntry(i, kVSSetConstantBuffers, s_hooked_tables[i].orig_vs_cb, original_vs_set_buffers, &observedVSSetConstantBuffers);
        if (installOnEntry(i, kDrawIndexed, s_hooked_tables[i].orig_draw_indexed, original_draw_indexed, &observedDrawIndexed))
            hud_draw_hook_mask |= 1u;
        if (installOnEntry(i, kDraw, s_hooked_tables[i].orig_draw, original_draw, &observedDraw))
            hud_draw_hook_mask |= 2u;
        if (installOnEntry(i, kDrawIndexedInstanced, s_hooked_tables[i].orig_draw_indexed_instanced, original_draw_indexed_instanced, &observedDrawIndexedInstanced))
            hud_draw_hook_mask |= 4u;
        if (installOnEntry(i, kDrawInstanced, s_hooked_tables[i].orig_draw_instanced, original_draw_instanced, &observedDrawInstanced))
            hud_draw_hook_mask |= 8u;
    }
    installHudStateRearmHooks();
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
    EdpeLog(L"EDPE: verified HUD shader pairs for Pass 35 cockpit holographic HUD:");
    EdpeLog(L"  VS=0x81216C77F90DEDD6 PS=0xAB82734FE19EFB9D (text/icons/gauges, 12 draws)");
    EdpeLog(L"  VS=0xB7790CBFC6554097 PS=0x8DEF46452FA459F5 (flight HUD vector lines)");
    EdpeLog(L"  VS=0x81216C77F90DEDD6 PS=0xA2965EC2931A39C8 (holo panels)");
    EdpeLog(L"  VS=0xE508648660A352B2 PS=0x63ABD86359B57D01 (target reticle sprite)");
}

static inline bool areDrawAndOmHooksIntact(void** tbl) {
    if (!tbl) return false;
    if (tbl[kOMSetRenderTargets] != reinterpret_cast<void*>(&observedOMSetRenderTargets) ||
        tbl[kOMSetRenderTargetsAndUnorderedAccessViews] !=
            reinterpret_cast<void*>(&observedOMSetRenderTargetsAndUnorderedAccessViews) ||
        tbl[kVSSetConstantBuffers] != reinterpret_cast<void*>(&observedVSSetConstantBuffers)) {
        return false;
    }
    if (hud_draw_probe_active.load(std::memory_order_relaxed)) {
        const unsigned mask = hud_draw_hook_mask;
        if (((mask & 1u) && tbl[kDrawIndexed] != reinterpret_cast<void*>(&observedDrawIndexed)) ||
            ((mask & 2u) && tbl[kDraw] != reinterpret_cast<void*>(&observedDraw)) ||
            ((mask & 4u) && tbl[kDrawIndexedInstanced] != reinterpret_cast<void*>(&observedDrawIndexedInstanced)) ||
            ((mask & 8u) && tbl[kDrawInstanced] != reinterpret_cast<void*>(&observedDrawInstanced))) {
            return false;
        }
    }
    return true;
}

void rearmHudDrawProbe(ID3D11DeviceContext* activeCtx) {
    const size_t tblCount = s_hooked_table_count.load(std::memory_order_acquire);
    if (activeCtx) {
        for (size_t i = 0; i < tblCount; ++i) {
            if (s_hooked_tables[i].context == activeCtx &&
                s_context_vtable_hooks[i] && s_context_vtable_hooks[i]->isInitialized()) {
                s_context_vtable_hooks[i]->reclaim();
                if (s_context_vtable_hooks[i]->mode() == edpe::HookMode::LiveCopy &&
                    areDrawAndOmHooksIntact(s_hooked_tables[i].table)) {
                    return;
                }
                break;
            }
        }
    }

    auto rearmSlotOnEntry = [&](size_t idx, size_t slot, void* observer, unsigned bit) {
        auto& entry = s_hooked_tables[idx];
        if (!entry.is_immediate || !entry.table) return;
        void* current = entry.table[slot];
        if (current && current != observer) {
            if (InterlockedCompareExchangePointer(
                    reinterpret_cast<PVOID volatile*>(entry.table + slot), observer, current) == current ||
                patchSlot(entry.table, slot, current, observer)) {
                if (bit) {
                    hud_draw_rearmed_mask |= bit;
                    ++s_diag_hud_unhook_events;
                }
            }
        }
    };

    const bool needVsCb = vs_buffer_probe_active.load(std::memory_order_relaxed) ||
                          hud_draw_probe_active.load(std::memory_order_relaxed);
    const bool needDraws = hud_draw_probe_active.load(std::memory_order_acquire) &&
                           isHudDrawInterceptWindowActive();

    for (size_t i = 0; i < tblCount; ++i) {
        if (!s_hooked_tables[i].is_immediate) continue;
        if (s_context_vtable_hooks[i] && s_context_vtable_hooks[i]->isInitialized()) {
            s_context_vtable_hooks[i]->reclaim();
        }
        rearmSlotOnEntry(i, kOMSetRenderTargets, reinterpret_cast<void*>(&observedOMSetRenderTargets), 0);
        rearmSlotOnEntry(i, kOMSetRenderTargetsAndUnorderedAccessViews,
            reinterpret_cast<void*>(&observedOMSetRenderTargetsAndUnorderedAccessViews), 0);
        if (needVsCb) {
            rearmSlotOnEntry(i, kVSSetConstantBuffers,
                reinterpret_cast<void*>(&observedVSSetConstantBuffers), 0);
        }
        if (needDraws) {
            if (hud_draw_hook_mask & 1u)
                rearmSlotOnEntry(i, kDrawIndexed, reinterpret_cast<void*>(&observedDrawIndexed), 1u);
            if (hud_draw_hook_mask & 2u)
                rearmSlotOnEntry(i, kDraw, reinterpret_cast<void*>(&observedDraw), 2u);
            if (hud_draw_hook_mask & 4u)
                rearmSlotOnEntry(i, kDrawIndexedInstanced, reinterpret_cast<void*>(&observedDrawIndexedInstanced), 4u);
            if (hud_draw_hook_mask & 8u)
                rearmSlotOnEntry(i, kDrawInstanced, reinterpret_cast<void*>(&observedDrawInstanced), 8u);
        }
    }
}

void endHudDrawProbe() {
    if (!hud_draw_hook_mask) return;
    hud_draw_probe_active.store(false, std::memory_order_release);
    restoreHudStateRearmHooks();
    const size_t tblCount = s_hooked_table_count.load(std::memory_order_acquire);
    unsigned restored = 0;
    unsigned reverted = 0;
    auto restore = [&](size_t slot, auto observer, unsigned bit) {
        if (!(hud_draw_hook_mask & bit)) return;
        bool anyRestored = false;
        bool anyReverted = false;
        for (size_t i = 0; i < tblCount; ++i) {
            void** tbl = s_hooked_tables[i].table;
            if (!s_hooked_tables[i].is_immediate || !tbl) continue;
            void* forward = (s_context_vtable_hooks[i] && s_context_vtable_hooks[i]->isInitialized())
                ? s_context_vtable_hooks[i]->liveStubForSlot(slot)
                : nullptr;
            if (!forward) continue;
            if (patchSlot(tbl, slot, reinterpret_cast<void*>(observer), forward))
                anyRestored = true;
            else if (tbl[slot] == forward)
                anyReverted = true;
        }
        if (anyRestored) restored |= bit;
        else if (anyReverted) reverted |= bit;
    };
    restore(kDrawIndexed, &observedDrawIndexed, 1);
    restore(kDraw, &observedDraw, 2);
    restore(kDrawIndexedInstanced, &observedDrawIndexedInstanced, 4);
    restore(kDrawInstanced, &observedDrawInstanced, 8);
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

MapFn resolveOriginalMap(ID3D11DeviceContext* context);
UnmapFn resolveOriginalUnmap(ID3D11DeviceContext* context);

HRESULT STDMETHODCALLTYPE observedMap(ID3D11DeviceContext* context, ID3D11Resource* resource,
    UINT subresource, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped);
void STDMETHODCALLTYPE observedUnmap(ID3D11DeviceContext* context, ID3D11Resource* resource,
    UINT subresource);

template <typename Fn>
inline Fn resolveContextOriginalSlot(ID3D11DeviceContext* context, size_t slot, void* observer,
                                     const std::atomic<Fn>& fallback) {
    if (context) {
        void** table = *reinterpret_cast<void***>(context);
        const size_t tblCount = s_hooked_table_count.load(std::memory_order_acquire);
        for (size_t i = 0; i < tblCount; ++i) {
            if (s_hooked_tables[i].context == context ||
                s_hooked_tables[i].table == table ||
                s_hooked_tables[i].original_table == table) {
                if (s_context_vtable_hooks[i] && s_context_vtable_hooks[i]->isInitialized()) {
                    void* stub = s_context_vtable_hooks[i]->liveStubForSlot(slot);
                    if (stub && stub != observer) {
                        return reinterpret_cast<Fn>(stub);
                    }
                }
                break;
            }
        }
    }
    return fallback.load(std::memory_order_acquire);
}

VSSetConstantBuffersFn resolveOriginalVSSetConstantBuffers(ID3D11DeviceContext* context) {
    return resolveContextOriginalSlot(context, kVSSetConstantBuffers,
        reinterpret_cast<void*>(&observedVSSetConstantBuffers), original_vs_set_buffers);
}

DrawIndexedFn resolveOriginalDrawIndexed(ID3D11DeviceContext* context) {
    return resolveContextOriginalSlot(context, kDrawIndexed,
        reinterpret_cast<void*>(&observedDrawIndexed), original_draw_indexed);
}

DrawFn resolveOriginalDraw(ID3D11DeviceContext* context) {
    return resolveContextOriginalSlot(context, kDraw,
        reinterpret_cast<void*>(&observedDraw), original_draw);
}

DrawIndexedInstancedFn resolveOriginalDrawIndexedInstanced(ID3D11DeviceContext* context) {
    return resolveContextOriginalSlot(context, kDrawIndexedInstanced,
        reinterpret_cast<void*>(&observedDrawIndexedInstanced), original_draw_indexed_instanced);
}

DrawInstancedFn resolveOriginalDrawInstanced(ID3D11DeviceContext* context) {
    return resolveContextOriginalSlot(context, kDrawInstanced,
        reinterpret_cast<void*>(&observedDrawInstanced), original_draw_instanced);
}

MapFn resolveOriginalMap(ID3D11DeviceContext* context) {
    return resolveContextOriginalSlot(context, kMap,
        reinterpret_cast<void*>(&observedMap), original_map);
}

UnmapFn resolveOriginalUnmap(ID3D11DeviceContext* context) {
    return resolveContextOriginalSlot(context, kUnmap,
        reinterpret_cast<void*>(&observedUnmap), original_unmap);
}

static constexpr size_t kRejectedCacheSize = 256;
static std::atomic<ID3D11Resource*> s_rejected_map_resources[kRejectedCacheSize]{};

inline bool isResourceCachedRejected(ID3D11Resource* res) {
    const size_t slot = (reinterpret_cast<uintptr_t>(res) >> 4) & (kRejectedCacheSize - 1);
    return s_rejected_map_resources[slot].load(std::memory_order_relaxed) == res;
}

inline void cacheRejectedResource(ID3D11Resource* res) {
    const size_t slot = (reinterpret_cast<uintptr_t>(res) >> 4) & (kRejectedCacheSize - 1);
    s_rejected_map_resources[slot].store(res, std::memory_order_relaxed);
}

inline void clearRejectedResourceCache() {
    for (size_t i = 0; i < kRejectedCacheSize; ++i) {
        s_rejected_map_resources[i].store(nullptr, std::memory_order_relaxed);
    }
}

HRESULT STDMETHODCALLTYPE observedMap(ID3D11DeviceContext* context, ID3D11Resource* resource,
    UINT subresource, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) {
    std::lock_guard<std::recursive_mutex> lock(g_scene_ctx_mutex);
    const MapFn forward = resolveOriginalMap(context);
    const HRESULT result = forward ? forward(
        context, resource, subresource, type, flags, mapped) : E_FAIL;
    if (s_in_edpe_present.load(std::memory_order_acquire)) return result;
    if (SUCCEEDED(result) && isObservedImmediateContext(context) &&
        resource && map_hook_active.load(std::memory_order_acquire)) {
        ID3D11Resource* target = scene_map_target.load(std::memory_order_acquire);
        bool is_scene_cb = (resource == target);
        const bool probe_active = vs_buffer_probe_active.load(std::memory_order_relaxed);
        if (!is_scene_cb) {
            if (!isResourceCachedRejected(resource)) {
                edpe::ScopedProfileTimer profTimer(edpe::ProfileZone::MapUnmap);
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
                            const bool had_target = (target != nullptr);
                            scene_map_target.store(buffer, std::memory_order_release);
                            if (probe_active) clearTrackedMappedBuffersExcept(buffer);
                            target = buffer;
                            is_scene_cb = true;
                            if (!had_target && !scene_maps_before_bind.load(std::memory_order_relaxed)) {
                                scene_maps_before_bind.fetch_add(1, std::memory_order_relaxed);
                                EdpeLog(L"EDPE: scene constant buffer target discovered via Map()");
                            }
                        } else {
                            cacheRejectedResource(resource);
                        }
                        buffer->Release();
                    }
                } else {
                    cacheRejectedResource(resource);
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
    std::lock_guard<std::recursive_mutex> lock(g_scene_ctx_mutex);
    const UnmapFn forward = resolveOriginalUnmap(context);
    if (!s_in_edpe_present.load(std::memory_order_acquire) &&
        isObservedImmediateContext(context) &&
        resource &&
        unmap_hook_active.load(std::memory_order_acquire)) {
        void* mapped_ptr = popMappedBuffer(resource);
        ID3D11Resource* target = scene_map_target.load(std::memory_order_acquire);
        if (!mapped_ptr && resource == target) {
            mapped_ptr = scene_mapped_data.exchange(nullptr, std::memory_order_acq_rel);
        }
        if (!mapped_ptr) {
            if (resource == target) ++s_ustat.noMapped;
            if (forward) forward(context, resource, subresource);
            rearmHudDrawProbe(context);
            return;
        }
        edpe::ScopedProfileTimer profTimer(edpe::ProfileZone::MapUnmap);
        if (mapped_ptr) {
            scene_unmaps.fetch_add(1, std::memory_order_relaxed);
            ++s_ustat.total;
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
            // Phase 3: Candidate staging and jitter injection (Two-Phase Staging & Commit)
            edpe::CameraProjection parsed_camera{};
            const auto cur_frame = last_present_frame.load(std::memory_order_relaxed);
            const bool in_main_scene = s_inside_main_scene.load(std::memory_order_acquire);
            if (motion_color_frame == cur_frame && s_ustat.gbufStartIdx == 0) s_ustat.gbufStartIdx = s_ustat.total;
            bool is_main_camera = false;
            float* mutable_unmap_words = static_cast<float*>(mapped_ptr);
            edpe::NdcOffset existing_ndc_offset{};
            if (edpe::estimateEliteBlockNdcOffset(&mutable_unmap_words[1080], &existing_ndc_offset) &&
                std::isfinite(existing_ndc_offset.x) && std::isfinite(existing_ndc_offset.y) &&
                (std::fabs(existing_ndc_offset.x) > 1e-6f || std::fabs(existing_ndc_offset.y) > 1e-6f) &&
                std::fabs(existing_ndc_offset.x) <= 0.05f && std::fabs(existing_ndc_offset.y) <= 0.05f) {
                float unjittered_block[16]{};
                const edpe::ProjectionJitter unjitter{0.0f, 0.0f, -existing_ndc_offset.x, -existing_ndc_offset.y};
                if (edpe::jitterEliteProjectionBlock(&mutable_unmap_words[1080], unjittered_block, unjitter)) {
                    std::memcpy(&mutable_unmap_words[1080], unjittered_block, sizeof(unjittered_block));
                }
            }
            const float* words = mutable_unmap_words;
            int parse_fail_reason = 0;
            const bool parse_ok = edpe::parseEliteCamera(words, 5376 / sizeof(float), &parsed_camera, &parse_fail_reason);
            if (parse_ok && !s_ustat.haveFirstOk) {
                s_ustat.haveFirstOk = true;
                std::memcpy(&s_ustat.firstOkWords[0], &words[932], 12 * sizeof(float));
                std::memcpy(&s_ustat.firstOkWords[12], &words[1080], 16 * sizeof(float));
            }
            if (!parse_ok) {
                if (parse_fail_reason >= 0 && parse_fail_reason < 12) ++s_ustat.failReason[parse_fail_reason];
                if (!s_ustat.haveFirstFail) {
                    s_ustat.haveFirstFail = true;
                    s_ustat.firstFailReason = parse_fail_reason;
                    s_ustat.firstFailIdx = s_ustat.total;
                    std::memcpy(&s_ustat.firstFailWords[0], &words[932], 12 * sizeof(float));
                    std::memcpy(&s_ustat.firstFailWords[12], &words[1080], 16 * sizeof(float));
                }
            }
            // Cobra sometimes rewrites rows 932..943 with another orientation (cubemap faces)
            // while the projection block 1080..1095 still carries the pilot's current projection
            // and rotation. The strict parser rejects those writes (codes 7, 8, 10). Build the
            // camera from the block orientation so the pilot pose keeps updating every frame.
            bool block_cam_ok = false;
            if (!parse_ok && (parse_fail_reason == 7 || parse_fail_reason == 8 || parse_fail_reason == 10) &&
                s_camera_now.scaleX > 0.0f &&
                !(s_hdr_slot0_seen_frame == cur_frame && motion_color_frame == cur_frame)) {
                bool finite_block = true;
                for (size_t i = 1080; i < 1096; ++i)
                    if (!std::isfinite(words[i])) { finite_block = false; break; }
                if (finite_block) {
                    double xl = 0, yl = 0;
                    for (size_t row = 0; row < 3; ++row) {
                        xl += static_cast<double>(words[1080 + row * 4]) * words[1080 + row * 4];
                        yl += static_cast<double>(words[1081 + row * 4]) * words[1081 + row * 4];
                    }
                    const double bsx = std::sqrt(xl), bsy = std::sqrt(yl);
                    bool ok =
                        std::fabs(bsx - s_camera_now.scaleX) < 2e-3 &&
                        std::fabs(bsy - s_camera_now.scaleY) < 2e-3 &&
                        std::fabs(words[1094] - s_camera_now.depthB) < 1e-4f &&
                        std::fabs(words[1082]) <= 0.002f && std::fabs(words[1086]) <= 0.002f &&
                        std::fabs(words[1090]) <= 0.002f && std::fabs(words[1092]) <= 0.002f &&
                        std::fabs(words[1093]) <= 0.002f && std::fabs(words[1095]) <= 0.002f;
                    if (ok) {
                        double col[3][3];
                        for (size_t row = 0; row < 3; ++row) {
                            col[0][row] = words[1080 + row * 4] / bsx;
                            col[1][row] = words[1081 + row * 4] / bsy;
                            col[2][row] = words[1083 + row * 4];
                        }
                        for (int a = 0; a < 3 && ok; ++a) {
                            const double len = col[a][0] * col[a][0] + col[a][1] * col[a][1] + col[a][2] * col[a][2];
                            if (len < 0.98 || len > 1.02) ok = false;
                            for (int b = 0; b < a && ok; ++b) {
                                const double dp = col[a][0] * col[b][0] + col[a][1] * col[b][1] + col[a][2] * col[b][2];
                                if (std::fabs(dp) > 0.02) ok = false;
                            }
                        }
                    }
                    if (ok) {
                        edpe::CameraProjection bc = s_camera_now;
                        for (size_t row = 0; row < 3; ++row) {
                            bc.worldFromView[row * 4] = static_cast<float>(words[1080 + row * 4] / bsx);
                            bc.worldFromView[row * 4 + 1] = static_cast<float>(words[1081 + row * 4] / bsy);
                            bc.worldFromView[row * 4 + 2] = words[1083 + row * 4];
                        }
                        bc.scaleX = static_cast<float>(bsx);
                        bc.scaleY = static_cast<float>(bsy);
                        bc.depthB = words[1094];
                        edpe::cameraSetPosition(bc, edpe::cameraPosition(s_camera_now) + s_last_valid_velocity);
                        parsed_camera = bc;
                        block_cam_ok = true;
                        ++s_ustat.blockOnly;
                    }
                }
            }
            if (parse_ok || block_cam_ok) {
                const UINT outW = s_swapchain_width.load(std::memory_order_relaxed);
                const UINT outH = s_swapchain_height.load(std::memory_order_relaxed);

                // Як тільки 4-MRT G-Buffer завершився (перехід до HDR slot0 освітлення)
                // прийом основної камери в цьому кадрі закрито назавжди:
                const bool main_gbuffer_already_finished =
                    (s_hdr_slot0_seen_frame == cur_frame && motion_color_frame == cur_frame);

                float dtRatio = 1.0f;
                if (s_last_frame_dt > 1e-5f && s_current_frame_dt > 1e-5f) {
                    dtRatio = s_current_frame_dt / s_last_frame_dt;
                }

                if (!main_gbuffer_already_finished) {
                    const bool anchor_fresh = s_anchor_valid && (cur_frame <= s_anchor_frame + 2);
                    const bool matches_anchor = anchor_fresh &&
                        edpe::matchesAnchorPrediction(parsed_camera, s_anchor_camera, s_last_valid_velocity, dtRatio);

                    const bool probe_vs_anchor = anchor_fresh && !matches_anchor &&
                        isStereoEyeOrIpdProbe(parsed_camera, s_anchor_camera, s_last_valid_velocity, dtRatio);

                    const bool probe_vs_committed = (s_camera_now.scaleX > 0.0f) && !matches_anchor &&
                        isStereoEyeOrIpdProbe(parsed_camera, s_camera_now, s_last_valid_velocity, dtRatio);

                    const bool is_probe = probe_vs_anchor || probe_vs_committed;

                    if (is_probe) {
                        s_probe_seen_this_frame = true;
                        ++s_ustat.probe;
                    }

                    bool passes_filter = !is_probe &&
                        isAuthoritativeMainSceneCamera(
                            words, 5376 / sizeof(float), parsed_camera,
                            outW, outH, s_camera_now, s_last_valid_velocity, dtRatio);

                    // Real camera rotation or FOV change:
                    // 1) Pure in-place pilot head/ship rotation (rawDist < 0.05m) with valid FOV and chirality
                    //    is accepted immediately so fast head-look/mouse-look turns never drop camera updates.
                    // 2) Any camera/FOV change persisting >= 2 frames is accepted as a new baseline even if
                    //    Cobra unmaps VS cb1 before binding the 4-MRT G-buffer.
                    if (!passes_filter && !is_probe &&
                        cameraRotationDeterminant(parsed_camera) >= 0.85f &&
                        cameraRotationDeterminant(parsed_camera) <= 1.15f &&
                        parsed_camera.depthB >= 0.01f && parsed_camera.depthB <= 0.25f &&
                        !isRearOrAuxiliaryProbe(parsed_camera, s_camera_now, s_last_valid_velocity, dtRatio)) {
                        const float rawDistToCommitted = (s_camera_now.scaleX > 0.0f)
                            ? edpe::length(edpe::cameraPosition(parsed_camera) - edpe::cameraPosition(s_camera_now))
                            : 0.0f;
                        const bool sameFov = (s_camera_now.scaleX > 0.0f) &&
                            std::fabs(parsed_camera.scaleX - s_camera_now.scaleX) <= 0.02f * s_camera_now.scaleX &&
                            std::fabs(parsed_camera.scaleY - s_camera_now.scaleY) <= 0.02f * s_camera_now.scaleY;
                        if (sameFov && rawDistToCommitted < 0.05f) {
                            passes_filter = true;
                            s_cut_rejection_streak = 0;
                            s_anchor_valid = false;
                        } else if (!(s_has_staged_camera && s_staged_camera_frame == cur_frame)) {
                            if (s_last_cut_rejection_frame != cur_frame) {
                                s_last_cut_rejection_frame = cur_frame;
                                ++s_cut_rejection_streak;
                            }
                            if (s_cut_rejection_streak >= 2 && s_consecutive_fallback_frames >= 1) {
                                passes_filter = true;
                                s_cut_rejection_streak = 0;
                                s_anchor_valid = false;
                            }
                        }
                    } else if (passes_filter) {
                        s_cut_rejection_streak = 0;
                    }

                    bool allow_stage = false;
                    if (passes_filter) {
                        allow_stage = true;

                        // Per-FRAME hologram jump guard (passes 25-26 share one frame).
                        if (s_camera_now.scaleX > 0.0f) {
                            const edpe::Vec3 predicted =
                                edpe::cameraPosition(s_camera_now) +
                                (s_last_valid_velocity * std::clamp(dtRatio, 0.25f, 4.0f));
                            const float jumpResidual =
                                edpe::length(edpe::cameraPosition(parsed_camera) - predicted);
                            if (jumpResidual > 15.0f && jumpResidual < 500.0f) {
                                if (!(s_has_staged_camera && s_staged_camera_frame == cur_frame) &&
                                    s_last_teleport_rejection_frame != cur_frame) {
                                    s_last_teleport_rejection_frame = cur_frame;
                                    ++s_teleport_rejection_streak;
                                }
                                if (s_teleport_rejection_streak < 2 || s_consecutive_fallback_frames < 1) {
                                    allow_stage = false;
                                }
                            } else if (jumpResidual <= 15.0f) {
                                s_teleport_rejection_streak = 0;
                            }
                        }

                        if (allow_stage && s_has_staged_camera && s_staged_camera_frame == cur_frame) {
                            if (in_main_scene && !s_staged_from_in_scene) {
                                allow_stage = true;
                            } else if (!in_main_scene && s_staged_from_in_scene) {
                                allow_stage = false;
                            } else if (isReflectionOrAuxiliaryCamera(parsed_camera, s_staged_camera, {0.0f, 0.0f, 0.0f}, 1.0f) ||
                                       isRearOrAuxiliaryProbe(parsed_camera, s_staged_camera)) {
                                allow_stage = false;
                            } else if (s_staged_from_in_scene) {
                                const float intraDist = edpe::length(
                                    edpe::cameraPosition(parsed_camera) - edpe::cameraPosition(s_staged_camera));
                                if (intraDist > 0.03f) {
                                     allow_stage = false;
                                }
                            }
                        }

                        if (allow_stage) {
                            s_staged_camera = parsed_camera;
                            s_has_staged_camera = true;
                            s_staged_camera_frame = cur_frame;
                            if (in_main_scene) {
                                s_staged_from_in_scene = true;
                            }
                            s_valid_camera_seen_counter.fetch_add(1, std::memory_order_relaxed);
                            is_main_camera = true;
                        } else if (s_has_staged_camera && s_staged_camera_frame == cur_frame) {
                            // Keep Pass 13 G-buffer decal and stencil passes (which bind fewer than 4 MRTs
                            // after the 4-MRT G-buffer unbinds, so in_main_scene is false) jitter-aligned
                            // with the main G-buffer whenever they unmap the same main camera.
                            const float intraDist = edpe::length(
                                edpe::cameraPosition(parsed_camera) - edpe::cameraPosition(s_staged_camera));
                            const float intraTrace = edpe::cameraRotationTrace(parsed_camera, s_staged_camera);
                            const bool sameStagedFov =
                                std::fabs(parsed_camera.scaleX - s_staged_camera.scaleX) <= 0.02f * s_staged_camera.scaleX &&
                                std::fabs(parsed_camera.scaleY - s_staged_camera.scaleY) <= 0.02f * s_staged_camera.scaleY &&
                                std::fabs(parsed_camera.depthB - s_staged_camera.depthB) <= 1e-3f;
                            if (intraDist <= 0.04f && intraTrace > 2.95f && sameStagedFov &&
                                cameraRotationDeterminant(parsed_camera) >= 0.85f &&
                                parsed_camera.depthB >= 0.01f && parsed_camera.depthB <= 0.25f &&
                                !isReflectionOrAuxiliaryCamera(parsed_camera, s_staged_camera, {0.0f, 0.0f, 0.0f}, 1.0f) &&
                                !isRearOrAuxiliaryProbe(parsed_camera, s_staged_camera)) {
                                is_main_camera = true;
                            }
                        }
                    } else if (!is_probe && s_has_staged_camera && s_staged_camera_frame == cur_frame) {
                        // Coherent phase-group jitter for same-pose scene sub-passes whose near plane
                        // was adjusted per-item (e.g. 0.0675 vs 0.025) while sharing the main scene grid.
                        const float intraDist = edpe::length(
                            edpe::cameraPosition(parsed_camera) - edpe::cameraPosition(s_staged_camera));
                        const float intraTrace = edpe::cameraRotationTrace(parsed_camera, s_staged_camera);
                        if (intraDist <= 0.04f && intraTrace > 2.98f &&
                            isSceneCameraAspect(parsed_camera.scaleX, parsed_camera.scaleY, outW, outH) &&
                            cameraRotationDeterminant(parsed_camera) >= 0.85f &&
                            cameraRotationDeterminant(parsed_camera) <= 1.15f &&
                            parsed_camera.depthB >= 0.01f && parsed_camera.depthB <= 0.25f &&
                            !isRearOrAuxiliaryProbe(parsed_camera, s_staged_camera)) {
                            is_main_camera = true;
                        }
                    }

                    if (is_main_camera) {
                        ++s_ustat.accepted;
                        if (s_ustat.acceptedIdx == 0) s_ustat.acceptedIdx = s_ustat.total;
                    } else {
                        if (!passes_filter && !is_probe) ++s_ustat.filterFail;
                        else if (passes_filter) ++s_ustat.stageDenied;
                        s_ustat.lastRDist = edpe::length(
                            edpe::cameraPosition(parsed_camera) - edpe::cameraPosition(s_camera_now));
                        s_ustat.lastRTrace = edpe::cameraRotationTrace(parsed_camera, s_camera_now);
                    }

                    if (!is_main_camera) {
                        static unsigned long long s_last_diag_unmap_frame = 0;
                        static unsigned s_diag_unmap_count = 0;
                        if (cur_frame != s_last_diag_unmap_frame) {
                            s_last_diag_unmap_frame = cur_frame;
                            s_diag_unmap_count = 0;
                        }
                        static unsigned s_diag_total_logged = 0;
                        if (++s_diag_unmap_count <= 2 && s_diag_total_logged < 40) {
                            ++s_diag_total_logged;
                            const edpe::Vec3 candPos = edpe::cameraPosition(parsed_camera);
                            const edpe::Vec3 nowPos = edpe::cameraPosition(s_camera_now);
                            const float rDist = edpe::length(candPos - nowPos);
                            const float rTrace = edpe::cameraRotationTrace(parsed_camera, s_camera_now);
                            const float det = edpe::cameraRotationDeterminant(parsed_camera);
                            wchar_t diagMsg[256];
                            swprintf_s(diagMsg,
                                L"EDPE DIAG: unmap candidate ignored frame=%llu in_scene=%d is_probe=%d passes=%d allow=%d staged=%d rDist=%.4f rTrace=%.4f det=%.3f depthB=%.4f",
                                cur_frame, (int)in_main_scene, (int)is_probe, (int)passes_filter, (int)allow_stage, (int)s_has_staged_camera,
                                rDist, rTrace, det, parsed_camera.depthB);
                            EdpeLog(diagMsg);
                        }
                    }
                } else {
                    ++s_ustat.gbufDone;
                    const edpe::CameraProjection& refCam =
                        (s_has_staged_camera && s_staged_camera_frame == cur_frame)
                            ? s_staged_camera
                            : s_camera_now;
                    if (refCam.scaleX > 0.0f) {
                        const float lateDist = edpe::length(
                            edpe::cameraPosition(parsed_camera) - edpe::cameraPosition(refCam));
                        const float lateTrace = edpe::cameraRotationTrace(parsed_camera, refCam);
                        if (lateDist < 0.1f && lateTrace > 2.95f) {
                            ++s_ustat.nearLate;
                            if (s_ustat.firstNearLateIdx == 0) s_ustat.firstNearLateIdx = s_ustat.total;
                            if (parse_ok) {
                                std::memcpy(s_last_clean_cb1_data, words, 5376);
                                s_has_clean_cb1_data = true;
                                s_clean_camera_cb_dirty = true;
                                s_clean_camera_frame = cur_frame;
                            }
                            const bool preTonemapScenePass =
                                (s_verified_ldr_tonemap_frame != cur_frame &&
                                 !s_ldr_tonemap_candidate_bound &&
                                 !s_post_ldr_backbuffer_bound);
                            const bool sameRefFov =
                                std::fabs(parsed_camera.scaleX - refCam.scaleX) <= 0.02f * refCam.scaleX &&
                                std::fabs(parsed_camera.scaleY - refCam.scaleY) <= 0.02f * refCam.scaleY &&
                                std::fabs(parsed_camera.depthB - refCam.depthB) <= 1e-3f;
                            if (preTonemapScenePass && lateDist <= 0.04f && lateTrace > 2.99f && sameRefFov &&
                                cameraRotationDeterminant(parsed_camera) >= 0.85f &&
                                parsed_camera.depthB >= 0.01f && parsed_camera.depthB <= 0.25f &&
                                !isReflectionOrAuxiliaryCamera(parsed_camera, refCam, {0.0f, 0.0f, 0.0f}, 1.0f) &&
                                !isRearOrAuxiliaryProbe(parsed_camera, refCam)) {
                                is_main_camera = true;
                            }
                        }
                    }
                }
            } else {
                ++s_ustat.parseFail;
            }

            const bool block_only_pilot = block_cam_ok;
            const bool jitter_candidate = is_main_camera || block_only_pilot;

            // Phase 5: Projection jitter injection into words 1080..1095
            // Inject jitter ONLY into authoritative primary scene camera projections during G-Buffer
            bool jitter_applied_this_unmap = false;
            if (jitter_candidate && !ContextCensusIsJitterActive()) ++s_ustat.jitterOff;
            if (jitter_candidate && ContextCensusIsJitterActive()) {
                float* mutable_words = static_cast<float*>(mapped_ptr);
                bool valid_projection = true;
                for (size_t i = 1080; i < 1096; ++i) {
                    if (!std::isfinite(mutable_words[i])) {
                        valid_projection = false;
                        break;
                    }
                }
                if (valid_projection) {
                    const float depthB = mutable_words[1094];
                    if (depthB < 0.01f || depthB > 0.25f ||
                        std::fabs(mutable_words[1092]) > 0.002f ||
                        std::fabs(mutable_words[1093]) > 0.002f ||
                        std::fabs(mutable_words[1095]) > 0.002f) {
                        valid_projection = false;
                    }
                }
                if (valid_projection) {
                    edpe::ProjectionJitter jitter = getOrUpdateSceneProjectionJitter();
                    float jittered_block[16]{};
                    if (edpe::jitterEliteProjectionBlock(&mutable_words[1080], jittered_block, jitter)) {
                        std::memcpy(s_last_clean_cb1_data, mutable_words, 5376);
                        s_has_clean_cb1_data = true;
                        s_clean_camera_cb_dirty = true;
                        s_clean_camera_frame = cur_frame;

                        std::memcpy(&mutable_words[1080], jittered_block, sizeof(jittered_block));
                        s_jittered_cb1_buffer.store(resource, std::memory_order_release);
                        jitter_applied_this_unmap = true;
                        s_injected_jitter = jitter;
                        s_last_rendered_jitter = jitter;
                        s_scene_writes_since_eval.fetch_add(1, std::memory_order_relaxed);
                        s_jitter_injected_frame = cur_frame;
                        const unsigned count = jittered_writes_count.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (count == 1) {
                            edpe::NdcOffset verified_offset{};
                            edpe::estimateEliteBlockNdcOffset(&mutable_words[1080], &verified_offset);
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
            if (!jitter_applied_this_unmap) {
                if (s_jittered_cb1_buffer.load(std::memory_order_acquire) == resource) {
                    // Cobra re-mapped and unmapped this buffer without jitter (e.g. Pass 35 unjittered camera
                    // or per-widget constants); mark it as no longer jittered and do NOT overwrite
                    // s_last_clean_cb1_data (which could otherwise be clobbered by offscreen 2D panel RTTs).
                    s_jittered_cb1_buffer.store(nullptr, std::memory_order_release);
                }
            }
        }
    }
    if (forward) forward(context, resource, subresource);
    if (!s_in_edpe_present.load(std::memory_order_acquire) && isObservedImmediateContext(context)) {
        rearmHudDrawProbe(context);
    }
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
    const bool restored = !hud_draw_probe_active.load(std::memory_order_acquire) &&
        patched_table && patchSlot(patched_table,
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
    if (!forward) {
        EdpeLog(L"EDPE: depth-pass VS bindings unavailable (invalid forward)");
        return;
    }
    if (forward != &observedVSSetConstantBuffers) {
        original_vs_set_buffers.store(forward, std::memory_order_release);
    }
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
    if (forward != &observedVSSetConstantBuffers &&
        !patchSlot(patched_table, kVSSetConstantBuffers,
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
        ID3D11DeviceContext* ctx = sample.context ? sample.context : context;
        if (context && sample.context && sample.context != context) continue;
        logDepthState(ctx, sample, L"end");
        ctx->End(sample.query);
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
    sample.context = context;
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
        ID3D11DeviceContext* ctx = sample.context ? sample.context : context;
        D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
        const HRESULT result = ctx ? ctx->GetData(sample.query, &stats, sizeof(stats),
            D3D11_ASYNC_GETDATA_DONOTFLUSH) : E_FAIL;
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

void recordBind(PerContextData* ctxData, int index) {
    if (!ctxData || index == ctxData->sequence_last) return;
    ctxData->sequence_last = index;
    ++ctxData->sequence_transitions;
    if (ctxData->sequence_stored < ctxData->bind_sequence.size())
        ctxData->bind_sequence[ctxData->sequence_stored++] = index;
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
    sample.context = context;
    sample.bind = bind_ordinal;
    sample.after_present = last_present_frame.load(std::memory_order_relaxed);
    sample.wait = 0;
    sample.ready.store(true, std::memory_order_release);
}

void pollConstantBufferSamples(ID3D11DeviceContext* context) {
    for (auto& sample : constant_buffer_samples) {
        if (!sample.ready.load(std::memory_order_acquire)) continue;
        ID3D11DeviceContext* ctx = sample.context ? sample.context : context;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = ctx ? ctx->Map(sample.buffer, 0, D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) : E_FAIL;
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
            ctx->Unmap(sample.buffer, 0);
        } else if (result == DXGI_ERROR_WAS_STILL_DRAWING && ++sample.wait < 120) {
            continue;
        } else {
            EdpeLog(L"EDPE: scene CB sample unavailable (nonblocking map failed or timed out)");
        }
        sample.buffer->Release();
        sample.buffer = nullptr;
        sample.context = nullptr;
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
    if (!edpe::Config::instance().devMode || edpe::UpscalerManager::instance().isReconstructionActive()) return;
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
    camera_delta_source_context = context;
    camera_delta_source_frame = frame;
}

// Called when the scene-MRT interval ends: queue a 5376-byte GPU copy.
void copyCameraDeltaSource(ID3D11DeviceContext* context) {
    if (!camera_delta_source) return;
    ID3D11DeviceContext* srcCtx = camera_delta_source_context ? camera_delta_source_context : context;
    if (!srcCtx) return;
    edpe::ScopedProfileTimer profTimer(edpe::ProfileZone::CameraDeltaCopy);
    const auto source = std::move(camera_delta_source);
    camera_delta_source_context = nullptr;
    if (camera_delta_count == kCameraDeltaRing) {
        ++camera_delta_dropped;
        return;
    }
    auto& slot = camera_delta_ring[(camera_delta_head + camera_delta_count) % kCameraDeltaRing];
    if (slot.staging && slot.context != srcCtx) {
        slot.staging.Reset();
        slot.context = nullptr;
    }
    if (!slot.staging) { // Created once per slot, never per frame.
        D3D11_BUFFER_DESC desc{};
        source->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        desc.StructureByteStride = 0;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        srcCtx->GetDevice(&device);
        if (!device || FAILED(device->CreateBuffer(&desc, nullptr, &slot.staging))) {
            camera_delta_disabled = true;
            EdpeLog(L"EDPE: camera delta log disabled (staging creation failed)");
            return;
        }
        slot.context = srcCtx;
    }
    srcCtx->CopyResource(slot.staging.Get(), source.Get());
    slot.frame = camera_delta_source_frame;
    ++camera_delta_count;
}

// Called at Present: read finished copies in order without waiting.
void pollCameraDeltaRing(ID3D11DeviceContext* context) {
    while (camera_delta_count) {
        auto& slot = camera_delta_ring[camera_delta_head];
        ID3D11DeviceContext* ctx = slot.context ? slot.context : context;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = ctx ? ctx->Map(slot.staging.Get(), 0, D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) : E_FAIL;
        if (result == DXGI_ERROR_WAS_STILL_DRAWING) return;
        if (SUCCEEDED(result) && mapped.pData) {
            edpe::CameraProjection camera{};
            const bool parsed = edpe::parseEliteCamera(static_cast<const float*>(mapped.pData),
                5376 / sizeof(float), &camera);
            ctx->Unmap(slot.staging.Get(), 0);
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
    camera_delta_source_context = nullptr;
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
    if (!motion_pair_active || after_present != camera_pair_sample_frames[1]) return;
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
        frame <= motion_color_frame || motion_depth_frame != motion_color_frame)
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
    if (!motion_color_source) return;
    if (s_clean_hdr_scene_texture && s_clean_hdr_scene_frame == motion_color_frame && !from_hud) return;
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
    if (!s_clean_hdr_scene_texture || s_clean_hdr_scene_width != desc.Width || s_clean_hdr_scene_height != desc.Height) {
        s_clean_hdr_scene_texture.Reset();
        s_clean_hdr_scene_srv.Reset();
        s_clean_hdr_scene_rtv.Reset();
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &s_clean_hdr_scene_texture)) ||
            FAILED(device->CreateShaderResourceView(s_clean_hdr_scene_texture.Get(), nullptr, &s_clean_hdr_scene_srv)) ||
            FAILED(device->CreateRenderTargetView(s_clean_hdr_scene_texture.Get(), nullptr, &s_clean_hdr_scene_rtv))) {
            EdpeLog(L"EDPE: early HDR copy unavailable (resource creation failed)");
            return;
        }
        s_clean_hdr_scene_width = desc.Width;
        s_clean_hdr_scene_height = desc.Height;
    }
    context->CopyResource(s_clean_hdr_scene_texture.Get(), source.Get());
    s_clean_hdr_scene_frame = motion_color_frame;

    motion_early_color_view = s_clean_hdr_scene_srv;
    motion_clean_color_target = s_clean_hdr_scene_rtv;
    motion_early_color_width = desc.Width;
    motion_early_color_height = desc.Height;
    motion_early_color_from_hud = from_hud;

    static unsigned long long s_last_early_log = ~0ull;
    if (s_last_early_log == ~0ull || motion_color_frame >= s_last_early_log + 300) {
        s_last_early_log = motion_color_frame;
        if (from_hud) EdpeLog(L"EDPE: clean HDR RTV ready for diagnostic world replay");
        EdpeLog(from_hud ? L"EDPE: HDR color copied before first matched HUD draw" :
                           L"EDPE: early HDR color copied at first RTV3 exit");
    }
    return;
#if 0
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
#endif
}

void queueCameraPairDepth(ID3D11DeviceContext* context, unsigned long long frame) {
    ID3D11DeviceContext* pairCtx = camera_pair_context ? camera_pair_context : activeSceneContext();
    if (!pairCtx) pairCtx = context;
    for (auto& sample : camera_pair_depth) {
        ID3D11DepthStencilView* view = nullptr;
        ID3D11DeviceContext* sampleCtx = nullptr;
        {
            std::lock_guard lock(seen_mutex);
            if (sample.view && frame > sample.after_present) {
                view = sample.view;
                sample.view = nullptr;
                sampleCtx = sample.context ? sample.context : pairCtx;
            }
        }
        if (!view || !sampleCtx) {
            if (view) view->Release();
            continue;
        }
        ID3D11DepthStencilView* bound = nullptr;
        sampleCtx->OMGetRenderTargets(0, nullptr, &bound);
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
        sampleCtx->GetDevice(&device);
        queueMotionDepth(sampleCtx, source, desc, sample.after_present);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        const HRESULT result = device ? device->CreateTexture2D(&desc, nullptr, &staging) : E_FAIL;
        if (device) device->Release();
        if (SUCCEEDED(result) && staging) {
            sampleCtx->CopyResource(staging, source); // One-shot full depth copy; no normal-frame readback.
            std::lock_guard lock(seen_mutex);
            sample.staging = staging;
            sample.context = sampleCtx;
            sample.width = desc.Width;
            sample.height = desc.Height;
            sample.wait = 0;
        } else {
            EdpeLog(L"EDPE: adjacent depth sample unavailable (staging creation failed)");
        }
        source->Release();
    }
    queueMotionColor(pairCtx, frame);
}

void pollCameraPairDepth(ID3D11DeviceContext* context) {
    std::lock_guard lock(seen_mutex);
    for (auto& sample : camera_pair_depth) {
        if (!sample.staging) continue;
        ID3D11DeviceContext* ctx = sample.context ? sample.context : context;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = ctx ? ctx->Map(sample.staging, 0, D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) : E_FAIL;
        if (result == DXGI_ERROR_WAS_STILL_DRAWING && ++sample.wait < 120) continue;
        if (SUCCEEDED(result) && mapped.pData) {
            float depth = 0;
            const auto* pixel = static_cast<const unsigned char*>(mapped.pData) +
                static_cast<size_t>(sample.height / 2) * mapped.RowPitch +
                static_cast<size_t>(sample.width / 2) * 8;
            std::memcpy(&depth, pixel, sizeof(depth));
            ctx->Unmap(sample.staging, 0);
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
        sample.context = nullptr;
    }
}

void pollMotionGrid(ID3D11DeviceContext* context) {
    if (!motion_grid_readback) return;
    ID3D11DeviceContext* ctx = motion_grid_context ? motion_grid_context : context;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT result = ctx ? ctx->Map(motion_grid_readback.Get(), 0,
        D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) : E_FAIL;
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
        ctx->Unmap(motion_grid_readback.Get(), 0);
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
    motion_grid_context = nullptr;
}

void tryMotionPair(ID3D11DeviceContext* fallback_context, unsigned long long frame) {
    if (!motion_pair_active) return;
    ID3D11DeviceContext* context = camera_pair_context ? camera_pair_context : activeSceneContext();
    if (!context) context = fallback_context;
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
    if (camera_pair_sample_frames[0] == ~0ull ||
        camera_pair_sample_frames[1] == ~0ull ||
        motion_camera_frames[0] != camera_pair_sample_frames[0] ||
        motion_camera_frames[1] != camera_pair_sample_frames[1] ||
        motion_depth_frame != camera_pair_sample_frames[1] ||
        !motion_depth_view) return;

    D3D11_TEXTURE2D_DESC desc{};
    motion_depth->GetDesc(&desc);
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    static ID3D11Device* s_motion_pass_device = nullptr;
    if (motion_pass && s_motion_pass_device != device.Get()) {
        motion_pass.reset();
    }
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
        s_motion_pass_device = device.Get();
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
        motion_grid_context = context;
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
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return fmt;
    }
}

static std::atomic<bool> s_comparison_capture_requested{false};

// ---- Sequential comparison capture --------------------------------------------------------
// For each available backend in turn: switch to it, let its temporal history settle, capture
// "Before" (scene before reconstruction) and the reconstructed frame, then wait before the next
// backend. The user's original backend/mode is restored at the end. Runs on the Present thread.
namespace {
enum class CmpPhase { Switch, Warmup, Wait };
struct CmpSequence {
    bool active = false;
    std::vector<edpe::UpscalerBackend> list;
    size_t idx = 0;
    CmpPhase phase = CmpPhase::Switch;
    int warmFrames = 0;
    std::chrono::steady_clock::time_point phaseStart{};
    std::chrono::steady_clock::time_point waitUntil{};
    edpe::UpscalerBackend origBackend = edpe::UpscalerBackend::None;
    edpe::UpscaleMode origMode = edpe::UpscaleMode::Native;
};
CmpSequence s_cmp_seq;
wchar_t s_cmp_progress[96]{};
constexpr int kCmpWarmupFrames = 16;
constexpr auto kCmpWaitBetween = std::chrono::seconds(5);
constexpr auto kCmpWarmupTimeout = std::chrono::seconds(8);

const wchar_t* cmpBackendName(edpe::UpscalerBackend b) {
    return b == edpe::UpscalerBackend::NvidiaDlss ? L"DLSS" :
           b == edpe::UpscalerBackend::AmdFsr ? L"FSR" :
           b == edpe::UpscalerBackend::Taa ? L"TAA" : L"None";
}

int cmpBackendId(edpe::UpscalerBackend b) {
    return b == edpe::UpscalerBackend::NvidiaDlss ? 1 :
           b == edpe::UpscalerBackend::AmdFsr ? 2 :
           b == edpe::UpscalerBackend::Taa ? 3 : 0;
}

void cmpFinish(edpe::UpscalerManager& up) {
    up.setBackend(s_cmp_seq.origBackend);
    up.setMode(s_cmp_seq.origMode);
    up.requestReset();
    s_cmp_seq.active = false;
    s_cmp_seq.list.clear();
    s_cmp_progress[0] = L'\0';
    s_comparison_capture_requested.store(false, std::memory_order_release);
    EdpeLog(L"EDPE: sequential comparison capture finished; original upscaler restored");
}
} // namespace

static void advanceComparisonSequence() {
    using clock = std::chrono::steady_clock;
    auto& up = edpe::UpscalerManager::instance();
    auto& s = s_cmp_seq;
    if (!s.active) {
        if (!s_comparison_capture_requested.load(std::memory_order_acquire)) return;
        s.list.clear();
        if (up.isReconstructionActive()) {
            for (auto b : { edpe::UpscalerBackend::NvidiaDlss, edpe::UpscalerBackend::AmdFsr, edpe::UpscalerBackend::Taa }) {
                if (up.isBackendAvailable(b)) s.list.push_back(b);
            }
        }
        if (s.list.empty()) {
            s_comparison_capture_requested.store(false, std::memory_order_release);
            EdpeLog(L"EDPE: comparison capture ignored (no active reconstruction or no backends)");
            return;
        }
        s.origBackend = up.getBackend();
        s.origMode = up.getMode();
        s.idx = 0;
        s.phase = CmpPhase::Switch;
        s.active = true;
        EdpeLog(L"EDPE: sequential comparison capture started");
    }
    const auto now = clock::now();
    switch (s.phase) {
    case CmpPhase::Switch: {
        if (s.idx >= s.list.size()) {
            cmpFinish(up);
            return;
        }
        const auto b = s.list[s.idx];
        up.setBackend(b);
        up.setMode(edpe::UpscaleMode::NativeAA);
        up.requestReset();
        s.warmFrames = kCmpWarmupFrames;
        s.phaseStart = now;
        s.phase = CmpPhase::Warmup;
        swprintf_s(s_cmp_progress, L"Capturing %ls (%zu/%zu)...", cmpBackendName(b), s.idx + 1, s.list.size());
        break;
    }
    case CmpPhase::Warmup:
        if (now - s.phaseStart > kCmpWarmupTimeout) {
            wchar_t msg[128];
            swprintf_s(msg, L"EDPE: comparison capture skipped %ls (no reconstructed frames)", cmpBackendName(s.list[s.idx]));
            EdpeLog(msg);
            ++s.idx;
            s.phase = CmpPhase::Switch;
        }
        break;
    case CmpPhase::Wait: {
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(s.waitUntil - now).count();
        if (now >= s.waitUntil) {
            ++s.idx;
            s.phase = CmpPhase::Switch;
        } else {
            swprintf_s(s_cmp_progress, L"Waiting %lld s before next backend (%zu/%zu done)...",
                static_cast<long long>(left) + 1, s.idx + 1, s.list.size());
        }
        break;
    }
    }
}
static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_backbuffer_copy;
static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_backbuffer_copy_srv;
static Microsoft::WRL::ComPtr<ID3D11RenderTargetView> s_backbuffer_copy_rtv;
static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_comp_eval_tex;
static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_comp_eval_srv;
static Microsoft::WRL::ComPtr<ID3D11RenderTargetView> s_comp_eval_rtv;
static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_comp_final_tex;
static Microsoft::WRL::ComPtr<ID3D11RenderTargetView> s_comp_final_rtv;
static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_dlaa_input;
static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_dlaa_input_srv;
static ID3D11Texture2D* s_direct_color_tex = nullptr;
static DXGI_FORMAT s_direct_color_fmt = DXGI_FORMAT_UNKNOWN;
static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_direct_color_srv;
static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_dlss_output_texture;
static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_dlss_output_srv;
static Microsoft::WRL::ComPtr<ID3D11RenderTargetView> s_backbuffer_rtv;
static ID3D11Texture2D* s_cached_backbuffer_ptr = nullptr;
static Microsoft::WRL::ComPtr<ID3D11RenderTargetView> s_debug_backbuffer_rtv;
static ID3D11Texture2D* s_debug_cached_backbuffer = nullptr;
static Microsoft::WRL::ComPtr<ID3D11RenderTargetView> s_hud_debug_backbuffer_rtv;
static ID3D11Texture2D* s_hud_debug_cached_backbuffer = nullptr;
static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_shared_bb_present;
static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_shared_bb_scene;
static Microsoft::WRL::ComPtr<IDXGIKeyedMutex> s_shared_mutex_present;
static Microsoft::WRL::ComPtr<IDXGIKeyedMutex> s_shared_mutex_scene;
static ID3D11Device* s_shared_present_dev = nullptr;
static ID3D11Device* s_shared_scene_dev = nullptr;
static ID3D11Device* s_temporal_resources_device = nullptr;
static unsigned long long s_last_reconstructed_frame = ~0ull;
static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_last_final_output_present;
static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_last_reconstructed_output;
static edpe::TemporalFrameInputs s_last_temporal_inputs{};
static unsigned long long s_last_temporal_inputs_frame = ~0ull;

static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_debounced_offscreen_color;
static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_pending_offscreen_color;
static bool s_has_debounced_source = false;
static bool s_debounced_is_backbuffer = false;
static bool s_pending_is_backbuffer = false;
static unsigned s_pending_source_streak = 0;

void resetTemporalPathDebounce() {
    s_debounced_offscreen_color.Reset();
    s_pending_offscreen_color.Reset();
    s_has_debounced_source = false;
    s_debounced_is_backbuffer = false;
    s_pending_is_backbuffer = false;
    s_pending_source_streak = 0;
}

constexpr DWORD kBridgeTimeoutMs = 500;

void resetSharedBackbufferBridge() {
    s_shared_mutex_present.Reset();
    s_shared_mutex_scene.Reset();
    s_shared_bb_present.Reset();
    s_shared_bb_scene.Reset();
    s_shared_present_dev = nullptr;
    s_shared_scene_dev = nullptr;
}

// Present device -> shared texture, then releases key 0.
bool bridgeCopyBackbufferIn(ID3D11DeviceContext* presentCtx, ID3D11Resource* presentBackbuffer) {
    if (!s_shared_mutex_present || !s_shared_bb_present) return false;
    if (s_shared_mutex_present->AcquireSync(0, kBridgeTimeoutMs) != S_OK) {
        EdpeLog(L"EDPE: WARNING: shared bridge acquire (present->scene) timed out; resetting bridge");
        resetSharedBackbufferBridge();
        return false;
    }
    presentCtx->CopyResource(s_shared_bb_present.Get(), presentBackbuffer);
    presentCtx->Flush();
    s_shared_mutex_present->ReleaseSync(0);
    return true;
}

// Scene-device ownership of the shared texture for the duration of one pass.
// Always uses key 0 on acquire and release so the present device can never deadlock.
struct SceneBridgeLock {
    bool held = false;
    bool acquire() {
        held = s_shared_mutex_scene && s_shared_mutex_scene->AcquireSync(0, kBridgeTimeoutMs) == S_OK;
        if (!held) {
            EdpeLog(L"EDPE: WARNING: shared bridge acquire (scene) timed out; resetting bridge");
            resetSharedBackbufferBridge();
        }
        return held;
    }
    void release() {
        if (held && s_shared_mutex_scene) {
            s_shared_mutex_scene->ReleaseSync(0);
            held = false;
        }
    }
    ~SceneBridgeLock() { release(); }
};

// Shared texture -> present backbuffer. Caller must have released SceneBridgeLock.
bool bridgeCopyResultOut(ID3D11DeviceContext* presentCtx, ID3D11Resource* presentBackbuffer) {
    if (!s_shared_mutex_present || !s_shared_bb_present) return false;
    if (s_shared_mutex_present->AcquireSync(0, kBridgeTimeoutMs) != S_OK) {
        EdpeLog(L"EDPE: WARNING: shared bridge acquire (scene->present) timed out; resetting bridge");
        resetSharedBackbufferBridge();
        return false;
    }
    presentCtx->CopyResource(presentBackbuffer, s_shared_bb_present.Get());
    presentCtx->Flush();
    s_shared_mutex_present->ReleaseSync(0);
    return true;
}

bool ensureSharedBackbufferBridge(ID3D11Device* presentDevice, ID3D11Device* sceneDevice,
    const D3D11_TEXTURE2D_DESC& bbDesc) {
    if (!presentDevice || !sceneDevice) return false;
    const DXGI_FORMAT fmt = concreteLdrColorFormat(bbDesc.Format);
    if (s_shared_bb_present && s_shared_bb_scene &&
        s_shared_present_dev == presentDevice && s_shared_scene_dev == sceneDevice) {
        D3D11_TEXTURE2D_DESC cur{};
        s_shared_bb_present->GetDesc(&cur);
        if (cur.Width == bbDesc.Width && cur.Height == bbDesc.Height && cur.Format == fmt) {
            return true;
        }
    }
    resetSharedBackbufferBridge();

    D3D11_TEXTURE2D_DESC sharedDesc{};
    sharedDesc.Width = bbDesc.Width;
    sharedDesc.Height = bbDesc.Height;
    sharedDesc.MipLevels = 1;
    sharedDesc.ArraySize = 1;
    sharedDesc.Format = fmt;
    sharedDesc.SampleDesc.Count = 1;
    sharedDesc.Usage = D3D11_USAGE_DEFAULT;
    sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    sharedDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    if (FAILED(presentDevice->CreateTexture2D(&sharedDesc, nullptr, &s_shared_bb_present))) {
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIResource> dxgiRes;
    HANDLE sharedHandle = nullptr;
    if (FAILED(s_shared_bb_present.As(&dxgiRes)) ||
        FAILED(dxgiRes->GetSharedHandle(&sharedHandle)) || !sharedHandle) {
        resetSharedBackbufferBridge();
        return false;
    }
    if (FAILED(sceneDevice->OpenSharedResource(sharedHandle, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(s_shared_bb_scene.GetAddressOf()))) ||
        FAILED(s_shared_bb_present.As(&s_shared_mutex_present)) ||
        FAILED(s_shared_bb_scene.As(&s_shared_mutex_scene))) {
        resetSharedBackbufferBridge();
        return false;
    }
    s_shared_present_dev = presentDevice;
    s_shared_scene_dev = sceneDevice;
    wchar_t msg[220];
    swprintf_s(msg,
        L"EDPE: initialized cross-device shared backbuffer bridge %ux%u format=%u (presentDev=%p -> sceneDev=%p)",
        bbDesc.Width, bbDesc.Height, static_cast<unsigned>(fmt), presentDevice, sceneDevice);
    EdpeLog(msg);
    return true;
}

OMSetRenderTargetsFn resolveOriginalOM(ID3D11DeviceContext* context);
MapFn resolveOriginalMap(ID3D11DeviceContext* context);
UnmapFn resolveOriginalUnmap(ID3D11DeviceContext* context);

struct ScopedD3D11PipelineState {
    ID3D11DeviceContext* ctx = nullptr;
    OMSetRenderTargetsFn rawSetOm = nullptr;
    VSSetConstantBuffersFn rawSetVsCb = nullptr;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11ComputeShader* cs = nullptr;
    ID3D11Buffer* vsCbs[4]{};
    ID3D11Buffer* psCbs[4]{};
    ID3D11Buffer* csCbs[4]{};
    ID3D11ShaderResourceView* psSrvs[8]{};
    ID3D11ShaderResourceView* csSrvs[8]{};
    ID3D11UnorderedAccessView* csUavs[2]{};
    ID3D11SamplerState* psSamps[4]{};
    ID3D11SamplerState* csSamps[4]{};
    ID3D11InputLayout* inputLayout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11RasterizerState* rs = nullptr;
    UINT numVps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT numScissors = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4]{};
    UINT sampleMask = 0xFFFFFFFF;
    ID3D11DepthStencilState* ds = nullptr;
    UINT stencilRef = 0;

    explicit ScopedD3D11PipelineState(ID3D11DeviceContext* c) : ctx(c) {
        if (!ctx) return;
        rawSetOm = resolveOriginalOM(ctx);
        rawSetVsCb = resolveOriginalVSSetConstantBuffers(ctx);
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ctx->PSGetShader(&ps, nullptr, nullptr);
        ctx->GSGetShader(&gs, nullptr, nullptr);
        ctx->CSGetShader(&cs, nullptr, nullptr);
        ctx->VSGetConstantBuffers(0, 4, vsCbs);
        ctx->PSGetConstantBuffers(0, 4, psCbs);
        ctx->CSGetConstantBuffers(0, 4, csCbs);
        ctx->PSGetShaderResources(0, 8, psSrvs);
        ctx->CSGetShaderResources(0, 8, csSrvs);
        ctx->CSGetUnorderedAccessViews(0, 2, csUavs);
        ctx->PSGetSamplers(0, 4, psSamps);
        ctx->CSGetSamplers(0, 4, csSamps);
        ctx->IAGetInputLayout(&inputLayout);
        ctx->IAGetPrimitiveTopology(&topo);
        ctx->RSGetState(&rs);
        ctx->RSGetViewports(&numVps, vps);
        ctx->RSGetScissorRects(&numScissors, scissors);
        ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
        ctx->OMGetDepthStencilState(&ds, &stencilRef);

        if (rawSetOm) {
            ID3D11RenderTargetView* nullRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            rawSetOm(ctx, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, nullRtvs, nullptr);
        }
    }

    ~ScopedD3D11PipelineState() {
        if (!ctx) return;
        rearmContextHooks();
        ID3D11ShaderResourceView* nullSrvs[8]{};
        ID3D11UnorderedAccessView* nullUavs[2]{};
        ctx->PSSetShaderResources(0, 8, nullSrvs);
        ctx->CSSetShaderResources(0, 8, nullSrvs);
        ctx->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
        if (rawSetOm) {
            ID3D11RenderTargetView* nullRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            rawSetOm(ctx, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, nullRtvs, nullptr);
            rawSetOm(ctx, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        }
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->GSSetShader(gs, nullptr, 0);
        ctx->CSSetShader(cs, nullptr, 0);
        if (rawSetVsCb) {
            rawSetVsCb(ctx, 0, 4, vsCbs);
        } else {
            ctx->VSSetConstantBuffers(0, 4, vsCbs);
        }
        ctx->PSSetConstantBuffers(0, 4, psCbs);
        ctx->CSSetConstantBuffers(0, 4, csCbs);
        ctx->PSSetShaderResources(0, 8, psSrvs);
        ctx->CSSetShaderResources(0, 8, csSrvs);
        ctx->CSSetUnorderedAccessViews(0, 2, csUavs, nullptr);
        ctx->PSSetSamplers(0, 4, psSamps);
        ctx->CSSetSamplers(0, 4, csSamps);
        ctx->IASetInputLayout(inputLayout);
        ctx->IASetPrimitiveTopology(topo);
        ctx->RSSetState(rs);
        ctx->RSSetViewports(numVps, vps);
        if (numScissors > 0) ctx->RSSetScissorRects(numScissors, scissors);
        ctx->OMSetBlendState(blend, blendFactor, sampleMask);
        ctx->OMSetDepthStencilState(ds, stencilRef);

        for (auto* p : rtvs) if (p) p->Release();
        if (dsv) dsv->Release();
        if (vs) vs->Release();
        if (ps) ps->Release();
        if (gs) gs->Release();
        if (cs) cs->Release();
        for (auto* p : vsCbs) if (p) p->Release();
        for (auto* p : psCbs) if (p) p->Release();
        for (auto* p : csCbs) if (p) p->Release();
        for (auto* p : psSrvs) if (p) p->Release();
        for (auto* p : csSrvs) if (p) p->Release();
        for (auto* p : csUavs) if (p) p->Release();
        for (auto* p : psSamps) if (p) p->Release();
        for (auto* p : csSamps) if (p) p->Release();
        if (inputLayout) inputLayout->Release();
        if (rs) rs->Release();
        if (blend) blend->Release();
        if (ds) ds->Release();
        rearmContextHooks();
        abandonAmortizedAndRearmHudProbe(ctx);
    }

    ScopedD3D11PipelineState(const ScopedD3D11PipelineState&) = delete;
    ScopedD3D11PipelineState& operator=(const ScopedD3D11PipelineState&) = delete;
};


// - When render resolution == output resolution (DLAA), evaluates on the finished
//   LDR backbuffer (tone mapped, HUD included).
// - When render resolution < output resolution (DLSS Super Resolution via Cobra's
//   SSAAMultiplier / Supersampling < 1.0), evaluates on the tone-mapped low-res LDR
//   scene target (scene_ldr_source, renderWidth x renderHeight) and reconstructs
//   into the native-resolution backbuffer (outputWidth x outputHeight).
// `rendered` is the last_present_frame value seen while this frame rendered.
void finalizeHudMaskForFrame(ID3D11DeviceContext* context, unsigned long long cur_rendered) {
    if (!context || !s_hud_mask_rtv || s_hud_mask_finalized_frame == cur_rendered) return;
    s_hud_mask_finalized_frame = cur_rendered;

    if (!s_hud_mask_cleared_for_frame || s_hud_draws_replayed_frame == 0) {
        if (!s_hud_mask_cleared_for_frame) {
            const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            context->ClearRenderTargetView(s_hud_mask_rtv.Get(), clearColor);
        }
        s_hud_draws_replayed_frame = 0;
        hud_first_match_seen = false;
        s_clean_camera_substitutions.store(0, std::memory_order_relaxed);
        s_jittered_cb1_buffer.store(nullptr, std::memory_order_relaxed);
    } else {
        if (s_hud_debug_view.load(std::memory_order_relaxed) &&
            s_hud_mask_prev_texture && s_hud_mask_texture) {
            context->CopyResource(s_hud_mask_prev_texture.Get(), s_hud_mask_texture.Get());
        }
        if (s_hud_tonemapped_valid_frame == cur_rendered) {
            s_hud_tonemapped_last_valid_frame = cur_rendered;
        }
        s_hud_mask_last_valid_frame = cur_rendered;
        s_hud_mask_last_valid_draws = s_hud_draws_replayed_frame;
    }
}

void tryTemporalUpscale(ID3D11DeviceContext* context, unsigned long long rendered,
    ID3D11Texture2D* backbuffer) {
    edpe::ScopedProfileTimer profTimer(edpe::ProfileZone::TemporalUpscale);
    const auto frame = rendered;
    auto& upscaler = edpe::UpscalerManager::instance();
    const bool upscale_active = upscaler.isReconstructionActive();
    const bool debug_view_active = s_jitter_debug_view.load(std::memory_order_relaxed) ||
                                   s_hud_debug_view.load(std::memory_order_relaxed);
    if ((!upscale_active && !debug_view_active) || !context || !backbuffer) return;

    if (s_upscale_evaluated_this_frame) {
        return;
    }

    std::lock_guard<std::recursive_mutex> sceneLock(g_scene_ctx_mutex);
    ID3D11DeviceContext* presentCtx = context;
    ID3D11Texture2D* presentBackbuffer = backbuffer;

    const wchar_t* skip = nullptr;
    if (!motion_color_source || !scene_depth_source) skip = L"scene color/depth not observed";
    else if (motion_color_frame != rendered) skip = L"no scene MRT pass this frame";
    else if (!s_has_staged_camera && s_camera_now.scaleX <= 0.0f) skip = L"no main camera latched this frame";

    bool scene_cut = false;
    if (!skip) {
        commitMainCamera(rendered, scene_cut);
    }
    if (skip) {
        if (upscale_active && s_last_reconstructed_output && s_last_reconstructed_frame != ~0ull &&
            (rendered >= s_last_reconstructed_frame && rendered - s_last_reconstructed_frame <= 2)) {
            Microsoft::WRL::ComPtr<ID3D11Device> lastDev;
            s_last_reconstructed_output->GetDevice(&lastDev);
            Microsoft::WRL::ComPtr<ID3D11Device> pbDev;
            presentBackbuffer->GetDevice(&pbDev);
            if (lastDev && pbDev && lastDev.Get() == pbDev.Get()) {
                D3D11_TEXTURE2D_DESC curDesc{}, pbDesc{};
                s_last_reconstructed_output->GetDesc(&curDesc);
                presentBackbuffer->GetDesc(&pbDesc);
                if (curDesc.Width == pbDesc.Width && curDesc.Height == pbDesc.Height && curDesc.Format == pbDesc.Format) {
                    presentCtx->CopyResource(presentBackbuffer, s_last_reconstructed_output.Get());
                    return;
                }
            }
        }
        if (++s_consecutive_skipped_frames > 4) {
            s_camera_reset = true;
            if (s_consecutive_skipped_frames > 15) {
                s_camera_now = {};
                s_anchor_valid = false;
                s_cut_rejection_streak = 0;
                s_teleport_rejection_streak = 0;
            }
        }
        if (upscale_active && (s_consecutive_skipped_frames == 10 || s_consecutive_skipped_frames % 600 == 0)) {
            wchar_t msg[192];
            swprintf_s(msg, L"EDPE: reconstruction skipped frame=%llu streak=%u (%ls)",
                frame, s_consecutive_skipped_frames, skip);
            EdpeLog(msg);
        }
        return;
    }

    D3D11_TEXTURE2D_DESC backbufferDesc{};
    presentBackbuffer->GetDesc(&backbufferDesc);
    if (!backbufferDesc.Width || !backbufferDesc.Height || backbufferDesc.SampleDesc.Count != 1) return;

    D3D11_TEXTURE2D_DESC depthDesc{};
    scene_depth_source->GetDesc(&depthDesc);
    if (!depthDesc.Width || !depthDesc.Height ||
        depthDesc.Width > backbufferDesc.Width || depthDesc.Height > backbufferDesc.Height) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> presentDevice;
    presentCtx->GetDevice(&presentDevice);
    if (!presentDevice) return;

    Microsoft::WRL::ComPtr<ID3D11Device> sceneDevice;
    scene_depth_source->GetDevice(&sceneDevice);
    if (!sceneDevice) sceneDevice = presentDevice;

    const bool isSubNative = (depthDesc.Width < backbufferDesc.Width ||
                              depthDesc.Height < backbufferDesc.Height);
    const auto desiredMode = (isSubNative && upscaler.getBackend() != edpe::UpscalerBackend::Taa)
        ? edpe::selectModeByRatio(depthDesc.Width, backbufferDesc.Width)
        : edpe::UpscaleMode::NativeAA;
    upscaler.setMode(desiredMode);
    const bool hdrInput = edpe::Config::instance().hdrInput;

    ID3D11Texture2D* ldrCandidate = nullptr;
    if (scene_ldr_source) {
        ldrCandidate = scene_ldr_source.Get();
    } else if (s_recent_scene_ldr_source && rendered >= s_recent_scene_ldr_frame &&
               (rendered - s_recent_scene_ldr_frame <= 2)) {
        ldrCandidate = s_recent_scene_ldr_source.Get();
    }
    if (ldrCandidate) {
        Microsoft::WRL::ComPtr<ID3D11Device> ldrDev;
        ldrCandidate->GetDevice(&ldrDev);
        if (!ldrDev || ldrDev.Get() != sceneDevice.Get()) {
            ldrCandidate = nullptr;
        }
    }

    ID3D11Texture2D* rawColorChoice = nullptr;
    const bool preserveUi = s_preserve_native_ui.load(std::memory_order_relaxed);
    if (hdrInput) {
        if (preserveUi && s_clean_hdr_scene_texture && s_clean_hdr_scene_frame == rendered) {
            rawColorChoice = s_clean_hdr_scene_texture.Get();
        } else if (isSubNative && scene_hdr_color_source && scene_hdr_color_frame == rendered) {
            rawColorChoice = scene_hdr_color_source.Get();
        } else if (isSubNative && ldrCandidate) {
            rawColorChoice = ldrCandidate;
        } else if (!isSubNative) {
            rawColorChoice = scene_hdr_color_source ? scene_hdr_color_source.Get() : presentBackbuffer;
        } else {
            if (++s_consecutive_skipped_frames > 4) {
                s_camera_reset = true;
            }
            return;
        }
    } else {
        if (isSubNative && ldrCandidate) {
            rawColorChoice = ldrCandidate;
        } else if (!isSubNative) {
            // Always prefer scene_ldr_source (Pass 52 Tonemapping output before Pass 53/54 2D UI)
            // even when Preserve Native HUD is OFF, so Pass 53/54 2D menus (ESC Pause Menu, Main Menu)
            // and mouse cursor are composited cleanly over the upscaled/AA output instead of being lost or smeared.
            const bool ldrAvailable = (ldrCandidate != nullptr);
            rawColorChoice = ldrAvailable ? ldrCandidate : presentBackbuffer;
            static unsigned long long s_native_src_log = ~0ull;
            if (edpe::Config::instance().devMode && (s_native_src_log == ~0ull || frame >= s_native_src_log + 300)) {
                s_native_src_log = frame;
                wchar_t msg[224];
                swprintf_s(msg,
                    L"EDPE: native color source frame=%llu preserveUi=%d ldrSource=%d ldrFrame=%llu chosen=%ls",
                    frame, s_preserve_native_ui.load(std::memory_order_relaxed) ? 1 : 0,
                    ldrCandidate ? 1 : 0, scene_ldr_frame,
                    ldrAvailable ? L"scene_ldr_source" : L"backbuffer");
                EdpeLog(msg);
            }
        } else {
            if (++s_consecutive_skipped_frames > 4) {
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

    // Apply debounce hysteresis filter to prevent flapping between scene target and backbuffer
    const bool rawIsBackbuffer = (rawColorChoice == presentBackbuffer);
    if (s_has_debounced_source && !s_debounced_is_backbuffer) {
        bool validDebounced = false;
        if (s_debounced_offscreen_color) {
            Microsoft::WRL::ComPtr<ID3D11Device> debDev;
            s_debounced_offscreen_color->GetDevice(&debDev);
            D3D11_TEXTURE2D_DESC debDesc{};
            s_debounced_offscreen_color->GetDesc(&debDesc);
            validDebounced = (debDev.Get() == sceneDevice.Get() &&
                              debDesc.Width == depthDesc.Width &&
                              debDesc.Height == depthDesc.Height);
        }
        if (!validDebounced || isSubNative != (depthDesc.Width < backbufferDesc.Width || depthDesc.Height < backbufferDesc.Height)) {
            resetTemporalPathDebounce();
        }
    } else if (s_has_debounced_source && s_debounced_is_backbuffer && isSubNative) {
        resetTemporalPathDebounce();
    }

    ID3D11Texture2D* chosenColor = nullptr;
    if (!s_has_debounced_source) {
        s_has_debounced_source = true;
        s_debounced_is_backbuffer = rawIsBackbuffer;
        s_debounced_offscreen_color = rawIsBackbuffer ? nullptr : rawColorChoice;
        s_pending_is_backbuffer = rawIsBackbuffer;
        s_pending_offscreen_color = rawIsBackbuffer ? nullptr : rawColorChoice;
        s_pending_source_streak = 0;
        chosenColor = rawColorChoice;
    } else if (rawIsBackbuffer == s_debounced_is_backbuffer) {
        s_debounced_offscreen_color = rawIsBackbuffer ? nullptr : rawColorChoice;
        s_pending_is_backbuffer = rawIsBackbuffer;
        s_pending_offscreen_color = rawIsBackbuffer ? nullptr : rawColorChoice;
        s_pending_source_streak = 0;
        chosenColor = rawColorChoice;
    } else {
        if (rawIsBackbuffer == s_pending_is_backbuffer &&
            (rawIsBackbuffer || rawColorChoice == s_pending_offscreen_color.Get())) {
            ++s_pending_source_streak;
        } else {
            s_pending_is_backbuffer = rawIsBackbuffer;
            s_pending_offscreen_color = rawIsBackbuffer ? nullptr : rawColorChoice;
            s_pending_source_streak = 1;
        }

        if (s_pending_source_streak >= 3) {
            s_debounced_is_backbuffer = rawIsBackbuffer;
            s_debounced_offscreen_color = rawIsBackbuffer ? nullptr : rawColorChoice;
            s_pending_source_streak = 0;
            s_camera_reset = true;
            chosenColor = rawColorChoice;
            EdpeLog(L"EDPE: debounced color source switched, DLSS history reset triggered");
        } else {
            chosenColor = s_debounced_is_backbuffer ? presentBackbuffer : s_debounced_offscreen_color.Get();
        }
    }

    if (!chosenColor) {
        chosenColor = presentBackbuffer;
    }
    D3D11_TEXTURE2D_DESC srcColorDesc{};
    if (chosenColor != presentBackbuffer) {
        Microsoft::WRL::ComPtr<ID3D11Device> colorDev;
        chosenColor->GetDevice(&colorDev);
        chosenColor->GetDesc(&srcColorDesc);
        if (!colorDev || colorDev.Get() != sceneDevice.Get() ||
            srcColorDesc.Width != depthDesc.Width || srcColorDesc.Height != depthDesc.Height) {
            resetTemporalPathDebounce();
            if (!isSubNative) {
                chosenColor = presentBackbuffer;
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
    }

    const bool chosenIsBackbuffer = (chosenColor == presentBackbuffer);
    // Always copy presentBackbuffer when using an offscreen scene target so Pass 53/54 2D menus
    // (ESC menu, Main Menu, cursor) are preserved regardless of s_preserve_native_ui.
    const bool needBackbufferIn = true;

    const bool crossDevice = (sceneDevice.Get() != presentDevice.Get());
    SceneBridgeLock sceneBridgeLock;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> sceneCtxHolder;
    if (crossDevice) {
        sceneDevice->GetImmediateContext(&sceneCtxHolder);
        if (!sceneCtxHolder) return;
        if (!ensureSharedBackbufferBridge(presentDevice.Get(), sceneDevice.Get(), backbufferDesc)) {
            return;
        }
        if (needBackbufferIn) {
            if (!bridgeCopyBackbufferIn(presentCtx, presentBackbuffer)) return;
        }
        if (!sceneBridgeLock.acquire()) return;
        context = sceneCtxHolder.Get();
        backbuffer = s_shared_bb_scene.Get();
    }

    ID3D11Texture2D* colorSource = chosenIsBackbuffer ? backbuffer : chosenColor;

    Microsoft::WRL::ComPtr<ID3D11Device> device = sceneDevice;
    if (s_temporal_resources_device != device.Get()) {
        s_clean_hdr_scene_texture.Reset();
        s_clean_hdr_scene_srv.Reset();
        s_clean_hdr_scene_rtv.Reset();
        s_clean_hdr_scene_width = s_clean_hdr_scene_height = 0;
        s_clean_hdr_scene_frame = ~0ull;
        s_dlaa_input.Reset();
        s_dlaa_input_srv.Reset();
        s_direct_color_srv.Reset();
        s_direct_color_tex = nullptr;
        s_direct_color_fmt = DXGI_FORMAT_UNKNOWN;
        motion_depth.Reset();
        motion_depth_view.Reset();
        s_motion_prev_depth.Reset();
        s_motion_prev_depth_view.Reset();
        s_has_motion_prev_depth = false;
        s_cached_hdr_rtv0 = nullptr;
        motion_pass.reset();
        s_dlss_output_texture.Reset();
        s_dlss_output_srv.Reset();
        s_backbuffer_copy.Reset();
        s_backbuffer_copy_srv.Reset();
        s_backbuffer_copy_rtv.Reset();
        s_comp_eval_tex.Reset();
        s_comp_eval_srv.Reset();
        s_comp_eval_rtv.Reset();
        s_comp_final_tex.Reset();
        s_comp_final_rtv.Reset();
        s_backbuffer_rtv.Reset();
        s_cached_backbuffer_ptr = nullptr;
        resetTemporalPathDebounce();
        ui_composite_pass.reset();
        resetHudMaskResources(nullptr);
        s_temporal_resources_device = device.Get();
    }

    colorSource->GetDesc(&srcColorDesc);
    if (srcColorDesc.Width != depthDesc.Width || srcColorDesc.Height != depthDesc.Height) {
        return;
    }

    static bool s_first_exec_logged = false;
    if (!s_first_exec_logged) {
        s_first_exec_logged = true;
        wchar_t msg[192];
        swprintf_s(msg, L"EDPE: tryTemporalUpscale executing frame=%llu render=%ux%u output=%ux%u",
            frame, depthDesc.Width, depthDesc.Height, backbufferDesc.Width, backbufferDesc.Height);
        EdpeLog(msg);
    }

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
    bool useDirectColorSrv = false;
    if (colorSource != backbuffer &&
        upscaler.getBackend() == edpe::UpscalerBackend::Taa &&
        !debug_view_active && !s_cmp_seq.active &&
        (srcColorDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
        if (s_direct_color_tex != colorSource || s_direct_color_fmt != inputFormat || !s_direct_color_srv) {
            s_direct_color_srv.Reset();
            s_direct_color_tex = nullptr;
            s_direct_color_fmt = DXGI_FORMAT_UNKNOWN;
            D3D11_SHADER_RESOURCE_VIEW_DESC cSrvDesc{};
            cSrvDesc.Format = inputFormat;
            cSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            cSrvDesc.Texture2D.MostDetailedMip = 0;
            cSrvDesc.Texture2D.MipLevels = 1;
            if (SUCCEEDED(device->CreateShaderResourceView(colorSource, &cSrvDesc, &s_direct_color_srv))) {
                s_direct_color_tex = colorSource;
                s_direct_color_fmt = inputFormat;
            }
        }
        useDirectColorSrv = (s_direct_color_srv != nullptr);
    }
    if (!useDirectColorSrv) {
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
    }
    ID3D11Texture2D* activeColorInputTex = useDirectColorSrv ? colorSource : s_dlaa_input.Get();
    ID3D11ShaderResourceView* activeColorInputSrv = useDirectColorSrv ? s_direct_color_srv.Get() : s_dlaa_input_srv.Get();

    if (motion_depth) {
        D3D11_TEXTURE2D_DESC curDesc{};
        motion_depth->GetDesc(&curDesc);
        if (curDesc.Width != depthDesc.Width || curDesc.Height != depthDesc.Height) {
            motion_depth.Reset();
            motion_depth_view.Reset();
            s_motion_prev_depth.Reset();
            s_motion_prev_depth_view.Reset();
            s_has_motion_prev_depth = false;
        }
    }

    if (!motion_depth || !s_motion_prev_depth) {
        D3D11_TEXTURE2D_DESC copyDesc = depthDesc;
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copyDesc.CPUAccessFlags = copyDesc.MiscFlags = 0;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        if (!motion_depth) {
            if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, &motion_depth))) return;
            if (FAILED(device->CreateShaderResourceView(motion_depth.Get(), &srvDesc, &motion_depth_view))) return;
        }
        if (!s_motion_prev_depth) {
            if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, &s_motion_prev_depth)) ||
                FAILED(device->CreateShaderResourceView(s_motion_prev_depth.Get(), &srvDesc, &s_motion_prev_depth_view))) {
                s_motion_prev_depth.Reset();
                s_motion_prev_depth_view.Reset();
                s_has_motion_prev_depth = false;
            }
        }
        s_has_motion_prev_depth = false;
    }

    ScopedD3D11PipelineState stateGuard(context);

    const unsigned scene_writes_this_eval = s_scene_writes_since_eval.exchange(0, std::memory_order_acq_rel);
    const bool has_fresh_scene_jitter = (scene_writes_this_eval > 0);
    {
        static unsigned long long s_zw_last_write_frame = 0;
        static unsigned s_zw_budget = 0; // Unmap-stats logging is off: EdpeLog opens the file per line and costs frame time.
        static unsigned s_zw_prev_maps = 0;
        static UnmapFrameStats s_zw_ring[3]{};
        static unsigned long long s_zw_ring_frame[3]{};
        static unsigned s_zw_ring_count = 0;
        const unsigned maps_now = scene_maps.load(std::memory_order_relaxed);
        const unsigned maps_delta = maps_now - s_zw_prev_maps;
        s_zw_prev_maps = maps_now;
        auto logStats = [](const wchar_t* tag, unsigned long long fr, unsigned long long gap,
                           unsigned maps, const UnmapFrameStats& s) {
            wchar_t reasons[160]{};
            size_t used = 0;
            for (int r = 2; r < 12; ++r) {
                if (!s.failReason[r]) continue;
                const int n = swprintf_s(reasons + used, 160 - used, L"%ls%d:%u", used ? L"," : L"", r, s.failReason[r]);
                if (n > 0) used += static_cast<size_t>(n);
            }
            wchar_t m[800];
            swprintf_s(m,
                L"EDPE %ls: frame=%llu gap=%llu maps=%u unmaps=%u parseFail=%u failReasons={%ls} blockOnly=%u firstFailIdx=%u gbufDone=%u nearLate=%u firstNearLateIdx=%u gbufStartIdx=%u acceptedIdx=%u probe=%u filterFail=%u stageDenied=%u accepted=%u rDist=%.3f rTrace=%.3f",
                tag, fr, gap, maps, s.total, s.parseFail, reasons, s.blockOnly, s.firstFailIdx, s.gbufDone, s.nearLate,
                s.firstNearLateIdx, s.gbufStartIdx, s.acceptedIdx, s.probe, s.filterFail, s.stageDenied,
                s.accepted, s.lastRDist, s.lastRTrace);
            EdpeLog(m);
            const bool dumpOk = (tag[1] == L'N') && s.haveFirstOk;
            const bool dumpFail = (tag[1] == L'W') && (gap == 1 || gap == 4) && s.haveFirstFail;
            if (dumpOk || dumpFail) {
                const float* w = dumpOk ? s.firstOkWords : s.firstFailWords;
                wchar_t d[1100];
                swprintf_s(d,
                    L"EDPE ZD: %ls frame=%llu failReason=%d rot=[%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f] proj=[%.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f]",
                    dumpOk ? L"ok" : L"fail", fr, dumpOk ? 0 : s.firstFailReason,
                    w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11],
                    w[12], w[13], w[14], w[15], w[16], w[17], w[18], w[19],
                    w[20], w[21], w[22], w[23], w[24], w[25], w[26], w[27]);
                EdpeLog(d);
            }
        };
        if (has_fresh_scene_jitter) {
            s_zw_last_write_frame = frame;
            s_zw_ring[s_zw_ring_count % 3] = s_ustat;
            s_zw_ring_frame[s_zw_ring_count % 3] = frame;
            ++s_zw_ring_count;
        } else if (s_zw_last_write_frame != 0 && frame - s_zw_last_write_frame < 40 && s_zw_budget > 0) {
            const unsigned long long gap = frame - s_zw_last_write_frame;
            if (gap == 1) {
                const unsigned n = s_zw_ring_count < 3 ? s_zw_ring_count : 3;
                for (unsigned k = 0; k < n; ++k) {
                    const unsigned i = (s_zw_ring_count - n + k) % 3;
                    logStats(L"ZN", s_zw_ring_frame[i], 0, 0, s_zw_ring[i]);
                }
            }
            --s_zw_budget;
            logStats(L"ZW", frame, gap, maps_delta, s_ustat);
        }
    }
    s_scene_jitter_consumed.store(true, std::memory_order_release);

    float jx = 0.0f, jy = 0.0f;
    if (has_fresh_scene_jitter && ContextCensusIsJitterActive()) {
        jx = s_injected_jitter.pixelX;
        jy = s_injected_jitter.pixelY;
        s_last_rendered_jitter = s_injected_jitter;
    } else {
        // Fallback when writes == 0: never pass stale jitter!
        // Always pass exact (0.0, 0.0) to TAA/upscaler to avoid subpixel jumping/flickering.
        jx = 0.0f;
        jy = 0.0f;
        s_last_rendered_jitter = {};
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
            s_last_frame_dt = s_current_frame_dt;
            s_current_frame_dt = dt_ms * 0.001f;
        }
    }
    s_last_upscale_qpc = now_qpc;

    const float* c = s_camera_now.worldFromView;
    const float* p = s_camera_previous.worldFromView;
    const float dx = c[3] - p[3];
    const float dy = c[7] - p[7];
    const float dz = c[11] - p[11];
    const float moveDist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float rotTrace = edpe::cameraRotationTrace(s_camera_now, s_camera_previous);
    const bool teleport_cut = (moveDist > 5000.0f);
    scene_cut = scene_cut || (s_camera_previous_frame == ~0ull) || teleport_cut;

    const bool eval_reset = s_camera_reset || scene_cut;
    const float prev_jx = eval_reset ? 0.0f : s_prev_eval_jitter_x;
    const float prev_jy = eval_reset ? 0.0f : s_prev_eval_jitter_y;

    const bool need_motion = upscale_active || s_jitter_debug_view.load(std::memory_order_relaxed);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> motionTexture;
    ID3D11ShaderResourceView* prevDepthSrv = nullptr;
    if (need_motion) {
        if (s_motion_prev_depth && s_motion_prev_depth_view) {
            std::swap(motion_depth, s_motion_prev_depth);
            std::swap(motion_depth_view, s_motion_prev_depth_view);
            if (s_has_motion_prev_depth && !eval_reset) {
                prevDepthSrv = s_motion_prev_depth_view.Get();
            }
        }
        context->CopyResource(motion_depth.Get(), scene_depth_source.Get());
        s_has_motion_prev_depth = (s_motion_prev_depth && s_motion_prev_depth_view);

        if (!motion_pass) {
            auto candidate = std::make_unique<edpe::MotionPass>();
            if (!candidate->initialize(device.Get(), context)) return;
            motion_pass = std::move(candidate);
        }

        if (s_zero_motion_vectors.load(std::memory_order_relaxed)) {
            motion_pass->clear(context);
        } else {
            // Render motion vectors at render resolution using current and previous camera projections and jitter phases
            motion_pass->render(motion_depth_view.Get(),
                s_camera_now, s_camera_previous, depthDesc.Width, depthDesc.Height,
                jx, jy, prev_jx, prev_jy, eval_reset, prevDepthSrv);
            rearmContextHooks();
        }

        if (!motion_pass->output()) return;

        Microsoft::WRL::ComPtr<ID3D11Resource> motionRes;
        motion_pass->output()->GetResource(&motionRes);
        if (!motionRes || FAILED(motionRes.As(&motionTexture))) return;
    }

    edpe::TemporalFrameInputs inputs{};
    inputs.color = activeColorInputTex;
    inputs.colorSrv = activeColorInputSrv;
    inputs.depth = motion_depth.Get();
    inputs.depthSrv = motion_depth_view.Get();
    inputs.prevDepthSrv = prevDepthSrv;
    inputs.motionVectors = motionTexture ? motionTexture.Get() : nullptr;
    inputs.motionSrv = (motion_pass && need_motion) ? motion_pass->output() : nullptr;
    inputs.renderWidth = depthDesc.Width;
    inputs.renderHeight = depthDesc.Height;
    inputs.outputWidth = backbufferDesc.Width;
    inputs.outputHeight = backbufferDesc.Height;
    inputs.jitterX = jx;
    inputs.jitterY = jy;
    inputs.prevJitterX = prev_jx;
    inputs.prevJitterY = prev_jy;
    inputs.motionScaleX = mScaleX;
    inputs.motionScaleY = mScaleY;
    inputs.frameTimeMs = dt_ms;
    inputs.reset = eval_reset;
    inputs.cameraNow = s_camera_now;
    inputs.cameraPrevious = s_camera_previous;
    inputs.sharpness = s_sharpness.load(std::memory_order_relaxed);
    inputs.depthInverted = true;
    inputs.motionIsLowRes = true;
    const bool isHdrColor = (hdrInput || (preserveUi && rawColorChoice == s_clean_hdr_scene_texture.Get())) &&
                            (srcColorDesc.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
                             srcColorDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                             srcColorDesc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS);
    inputs.colorIsHdr = isHdrColor;

    // Reactive mask removed: HUD and UI are composited via separate native layer.

    s_last_temporal_inputs = inputs;
    s_last_temporal_inputs_frame = rendered;

    if (!upscale_active) {
        s_camera_reset = false;
        s_prev_eval_jitter_x = jx;
        s_prev_eval_jitter_y = jy;
    } else {
        const DXGI_FORMAT bbFormat = concreteLdrColorFormat(backbufferDesc.Format);
        const DXGI_FORMAT outFormat = isHdrColor ? concreteLdrColorFormat(srcColorDesc.Format) : bbFormat;

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

        finalizeHudMaskForFrame(context, rendered);

        // Copy backbuffer (containing post-tonemap 2D menus, ESC menu, Main Menu, and cursor drawn in Pass 53/54)
        // to s_backbuffer_copy BEFORE any modification so UiCompositePass can extract 2D UI against s_dlaa_input.
        if (s_backbuffer_copy) {
            context->CopyResource(s_backbuffer_copy.Get(), backbuffer);
        }

        bool compOk = false;
        const bool upscalerEvalOk = upscaler.evaluate(context, inputs, nullptr, evalDestination);
        rearmContextHooks();
        edpe::updateCrashUpscalerState(
            static_cast<int>(upscaler.getBackend()),
            static_cast<int>(upscaler.getMode()),
            !upscalerEvalOk,
            inputs.renderWidth,
            inputs.renderHeight,
            inputs.outputWidth,
            inputs.outputHeight,
            last_present_frame.load(std::memory_order_relaxed));
        if (upscalerEvalOk) {
            s_camera_reset = false;
            s_prev_eval_jitter_x = jx;
            s_prev_eval_jitter_y = jy;
            s_upscale_evaluated_this_frame = true;
            s_diag_eval_stage = 2;

            // Always run UiCompositePass when colorSource != backbuffer (even if Preserve Native HUD is OFF)
            // so Pass 53/54 2D menus (ESC Pause Menu, Main Menu, mouse cursor) drawn onto backbuffer after
            // Pass 52 (scene_ldr_source) are extracted from s_backbuffer_copy_srv vs s_dlaa_input_srv and
            // never overwritten by s_dlss_output_texture.
            const bool canCompositeUi = (evalDestination == s_dlss_output_texture.Get() &&
                                         s_dlss_output_srv && s_backbuffer_copy_srv &&
                                         activeColorInputSrv && s_backbuffer_rtv &&
                                         colorSource != backbuffer);
            s_diag_composite_path = canCompositeUi ? 1 : 2;
            if (!canCompositeUi) {
                static unsigned long long s_nocomp_log = ~0ull;
                if (s_nocomp_log == ~0ull || frame >= s_nocomp_log + 300) {
                    s_nocomp_log = frame;
                    wchar_t cmsg[256];
                    swprintf_s(cmsg,
                        L"EDPE: UI composite disabled frame=%llu preserveUi=%d evalToIntermediate=%d dlssSrv=%d bbCopySrv=%d inputSrv=%d bbRtv=%d srcIsBackbuffer=%d",
                        frame, s_preserve_native_ui.load(std::memory_order_relaxed) ? 1 : 0,
                        evalDestination == s_dlss_output_texture.Get() ? 1 : 0,
                        s_dlss_output_srv ? 1 : 0, s_backbuffer_copy_srv ? 1 : 0,
                        s_dlaa_input_srv ? 1 : 0, s_backbuffer_rtv ? 1 : 0,
                        colorSource == backbuffer ? 1 : 0);
                    EdpeLog(cmsg);
                }
            }
            ID3D11ShaderResourceView* validHudSrv = nullptr;
            ID3D11ShaderResourceView* validHudTonemappedSrv = nullptr;
            if (canCompositeUi) {
                if (!ui_composite_pass) {
                    auto comp = std::make_unique<edpe::UiCompositePass>();
                    if (comp->initialize(device.Get(), context)) {
                        ui_composite_pass = std::move(comp);
                    }
                }
                if (ui_composite_pass) {
                    const bool hudMergedIntoSceneThisFrame =
                        (s_hud_merged_into_main_hdr_frame == rendered) ||
                        (s_pause_menu_seen_frame != ~0ull && rendered <= s_pause_menu_seen_frame + 1 &&
                         s_hud_draws_replayed_frame == 0);
                    const bool preserveHudActive =
                        (s_preserve_native_ui.load(std::memory_order_relaxed) ||
                         s_hud_debug_view.load(std::memory_order_relaxed) ||
                         s_jitter_debug_view.load(std::memory_order_relaxed)) &&
                        !hudMergedIntoSceneThisFrame;
                    ID3D11ShaderResourceView* hudSrvToUse = nullptr;
                    ID3D11ShaderResourceView* hudTonemappedSrvToUse = nullptr;
                    s_diag_hud_mask_w = s_hud_mask_width;
                    if (preserveHudActive) {
                        s_diag_hud_src = (s_hud_draws_replayed_frame > 0 && s_hud_mask_srv) ? 1 : 0;
                        if (s_hud_draws_replayed_frame > 0 && s_hud_mask_srv) {
                            hudSrvToUse = s_hud_mask_srv.Get();
                            if (s_hud_tonemapped_srv &&
                                (s_hud_tonemapped_valid_frame == rendered ||
                                 (s_hud_tonemapped_last_valid_frame != ~0ull &&
                                  rendered >= s_hud_tonemapped_last_valid_frame &&
                                  rendered - s_hud_tonemapped_last_valid_frame <= 2))) {
                                hudTonemappedSrvToUse = s_hud_tonemapped_srv.Get();
                            }
                        }
                    } else {
                        s_diag_hud_src = (s_pause_menu_seen_frame != ~0ull && rendered <= s_pause_menu_seen_frame + 1) ? 3 : 0;
                    }
                    const bool hudSizeOk =
                        (s_hud_mask_width == backbufferDesc.Width || s_hud_mask_width == depthDesc.Width);
                    validHudSrv =
                        (hudSrvToUse && hudSizeOk) ? hudSrvToUse : nullptr;
                    validHudTonemappedSrv =
                        (validHudSrv && hudTonemappedSrvToUse) ? hudTonemappedSrvToUse : nullptr;

                    compOk = ui_composite_pass->composite(context,
                        s_dlss_output_srv.Get(),
                        s_backbuffer_copy_srv.Get(),
                        activeColorInputSrv,
                        validHudSrv,
                        s_backbuffer_rtv.Get(),
                        backbufferDesc.Width, backbufferDesc.Height,
                        depthDesc.Width, depthDesc.Height,
                        isHdrColor,
                        /*preserveUi=*/true,
                        validHudTonemappedSrv);
                }
                if (compOk) {
                    static unsigned long long s_comp_log = ~0ull;
                    if (edpe::Config::instance().devMode && s_comp_log != frame && (s_comp_log == ~0ull || frame % 300 == 0)) {
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
                    compOk = true;
                }
            } else if (evalDestination == s_dlss_output_texture.Get()) {
                if (!isHdrColor) {
                    context->CopyResource(backbuffer, s_dlss_output_texture.Get());
                    compOk = true;
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
                            nullptr,
                            nullptr,
                            nullptr,
                            s_backbuffer_rtv.Get(),
                            backbufferDesc.Width, backbufferDesc.Height,
                            depthDesc.Width, depthDesc.Height,
                            /*isHdr=*/true,
                            /*preserveUi=*/false);
                    }
                }
            } else {
                compOk = true;
            }

            if (s_cmp_seq.active && s_cmp_seq.phase == CmpPhase::Warmup &&
                s_cmp_seq.idx < s_cmp_seq.list.size() &&
                upscaler.getBackend() == s_cmp_seq.list[s_cmp_seq.idx]) {
                if (s_cmp_seq.warmFrames > 0) {
                    --s_cmp_seq.warmFrames;
                } else {
                    // Capture "Before" (scene before reconstruction) and the reconstructed frame
                    // (already in `backbuffer`, with native UI composited when active).
                    const auto activeBackend = upscaler.getBackend();
                    const wchar_t* activeName = cmpBackendName(activeBackend);
                    SYSTEMTIME captureTime{};
                    GetLocalTime(&captureTime);
                    if (s_backbuffer_copy) {
                        ID3D11Texture2D* beforeTex = s_backbuffer_copy.Get();
                        // Because Pass 35 Cockpit HUD draws are redirected out of the game's HDR target into
                        // s_hud_mask_srv, s_backbuffer_copy is missing the separated Pass 35 HUD. Composite
                        // validHudSrv onto s_backbuffer_copy_srv into s_comp_final_tex so "Before-<Backend>"
                        // screenshots show the complete pre-reconstruction frame with the Cockpit HUD intact.
                        if (validHudSrv && ui_composite_pass && s_backbuffer_copy_srv && s_dlaa_input_srv) {
                            if (s_comp_final_tex) {
                                D3D11_TEXTURE2D_DESC cur{};
                                s_comp_final_tex->GetDesc(&cur);
                                if (cur.Width != backbufferDesc.Width || cur.Height != backbufferDesc.Height ||
                                    cur.Format != bbFormat) {
                                    s_comp_final_tex.Reset();
                                    s_comp_final_rtv.Reset();
                                }
                            }
                            if (!s_comp_final_tex) {
                                D3D11_TEXTURE2D_DESC td = backbufferDesc;
                                td.Format = bbFormat;
                                td.Usage = D3D11_USAGE_DEFAULT;
                                td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                                td.CPUAccessFlags = td.MiscFlags = 0;
                                if (FAILED(device->CreateTexture2D(&td, nullptr, &s_comp_final_tex)) ||
                                    FAILED(device->CreateRenderTargetView(s_comp_final_tex.Get(), nullptr, &s_comp_final_rtv))) {
                                    s_comp_final_tex.Reset();
                                    s_comp_final_rtv.Reset();
                                }
                            }
                            if (s_comp_final_tex && s_comp_final_rtv &&
                                ui_composite_pass->composite(context,
                                    s_backbuffer_copy_srv.Get(),
                                    s_backbuffer_copy_srv.Get(),
                                    s_dlaa_input_srv.Get(),
                                    validHudSrv,
                                    s_comp_final_rtv.Get(),
                                    backbufferDesc.Width, backbufferDesc.Height,
                                    depthDesc.Width, depthDesc.Height,
                                    /*isHdr=*/false,
                                    /*preserveUi=*/true,
                                    validHudTonemappedSrv)) {
                                beforeTex = s_comp_final_tex.Get();
                            }
                        }
                        wchar_t beforeName[48];
                        swprintf_s(beforeName, L"Before-%ls", activeName);
                        UiQueueComparisonItem(context, beforeName, beforeTex, &captureTime, 0);
                    }
                    UiQueueComparisonItem(context, activeName, backbuffer, &captureTime, cmpBackendId(activeBackend));
                    const bool lastBackend = (s_cmp_seq.idx + 1 >= s_cmp_seq.list.size());
                    s_cmp_seq.waitUntil = std::chrono::steady_clock::now() +
                        (lastBackend ? std::chrono::seconds(0) : kCmpWaitBetween);
                    s_cmp_seq.phase = CmpPhase::Wait;
                }
            }

            if (crossDevice) {
                context->Flush();
                sceneBridgeLock.release();
                if (bridgeCopyResultOut(presentCtx, presentBackbuffer)) {
                    s_last_final_output_present = s_shared_bb_present;
                    s_last_reconstructed_frame = rendered;
                    compOk = true;
                } else {
                    compOk = false;
                }
            } else {
                s_last_final_output_present.Reset();
                s_last_reconstructed_frame = rendered;
            }

            // Avoid copying presentBackbuffer (29.5 MB at 1440p) on every frame; s_dlss_output_texture
            // already retains the latest reconstructed scene on the render device.
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
                wchar_t diffBuf[256] = L"";
                if (!s_path_state_valid) {
                    wcscpy_s(diffBuf, L"init");
                } else {
                    int pos = 0;
                    if (s_prev_color_source != colorSource) {
                        pos += swprintf_s(diffBuf + pos, sizeof(diffBuf)/sizeof(diffBuf[0]) - pos,
                            L"src(%p->%p) ", s_prev_color_source, colorSource);
                    }
                    if (s_prev_color_format != srcColorDesc.Format) {
                        pos += swprintf_s(diffBuf + pos, sizeof(diffBuf)/sizeof(diffBuf[0]) - pos,
                            L"fmt(%u->%u) ", (unsigned)s_prev_color_format, (unsigned)srcColorDesc.Format);
                    }
                    if (s_prev_is_hdr != isHdrColor) {
                        pos += swprintf_s(diffBuf + pos, sizeof(diffBuf)/sizeof(diffBuf[0]) - pos,
                            L"hdr(%d->%d) ", s_prev_is_hdr ? 1 : 0, isHdrColor ? 1 : 0);
                    }
                    if (s_prev_eval_intermediate != evalToIntermediate) {
                        pos += swprintf_s(diffBuf + pos, sizeof(diffBuf)/sizeof(diffBuf[0]) - pos,
                            L"eval(%d->%d) ", s_prev_eval_intermediate ? 1 : 0, evalToIntermediate ? 1 : 0);
                    }
                    if (s_prev_comp_ok != compOk) {
                        pos += swprintf_s(diffBuf + pos, sizeof(diffBuf)/sizeof(diffBuf[0]) - pos,
                            L"comp(%d->%d) ", s_prev_comp_ok ? 1 : 0, compOk ? 1 : 0);
                    }
                }
                s_path_state_valid = true;
                s_prev_color_source = colorSource;
                s_prev_color_format = srcColorDesc.Format;
                s_prev_is_hdr = isHdrColor;
                s_prev_eval_intermediate = evalToIntermediate;
                s_prev_comp_ok = compOk;
                if (s_path_log_count < 200) {
                    ++s_path_log_count;
                    wchar_t pmsg[384];
                    swprintf_s(pmsg,
                        L"EDPE: temporal path change frame=%llu src=%p fmt=%u hdr=%d eval=%d comp=%d [reason: %ls]",
                        frame, static_cast<const void*>(colorSource),
                        static_cast<unsigned>(srcColorDesc.Format),
                        isHdrColor ? 1 : 0,
                        evalToIntermediate ? 1 : 0,
                        compOk ? 1 : 0,
                        diffBuf);
                    EdpeLog(pmsg);
                }
            }
        }
    }

    // Push frame jitter diagnostics and log entry to asynchronous JitterLogger (only when DEV=1 and enabled)
    if (edpe::JitterLogger::instance().shouldSampleFrame(rendered, scene_cut || s_camera_reset)) {
        const unsigned phases = s_active_jitter_phases.load(std::memory_order_relaxed);
        const unsigned long long scene_seq = jitter_frame_index.load(std::memory_order_relaxed);

        edpe::JitterFrameLog jlog{};
        jlog.frame = rendered;
        jlog.timeMs = getJitterLogTimeMs();
        jlog.deltaMs = dt_ms;
        jlog.phase = static_cast<unsigned>(scene_seq % (phases ? phases : 32));
        jlog.totalPhases = phases ? phases : 32;
        jlog.pixelX = s_injected_jitter.pixelX;
        jlog.pixelY = s_injected_jitter.pixelY;
        jlog.ndcX = s_injected_jitter.ndcX;
        jlog.ndcY = s_injected_jitter.ndcY;
        jlog.injectedWrites = scene_writes_this_eval;
        jlog.rejections = jitter_rejection_count.load(std::memory_order_relaxed);
        jlog.evalJx = inputs.jitterX;
        jlog.evalJy = inputs.jitterY;
        jlog.syncOk = has_fresh_scene_jitter;
        jlog.cameraValid = (s_camera_now.scaleX > 0.0f);
        jlog.heldJitter = (scene_writes_this_eval == 0);
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
    std::lock_guard<std::recursive_mutex> sceneLock(g_scene_ctx_mutex);

    ID3D11DeviceContext* presentCtx = context;
    ID3D11Texture2D* presentBackbuffer = backbuffer;

    D3D11_TEXTURE2D_DESC bbDesc{};
    presentBackbuffer->GetDesc(&bbDesc);
    if (!bbDesc.Width || !bbDesc.Height || bbDesc.SampleDesc.Count != 1) return;

    Microsoft::WRL::ComPtr<ID3D11Device> presentDevice;
    presentCtx->GetDevice(&presentDevice);
    if (!presentDevice) return;

    Microsoft::WRL::ComPtr<ID3D11Device> device = presentDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> sceneCtxHolder;
    const bool crossDevice = (s_temporal_resources_device &&
                              s_temporal_resources_device != presentDevice.Get() &&
                              s_last_temporal_inputs_frame == rendered);
    SceneBridgeLock sceneBridgeLock;
    if (crossDevice) {
        s_temporal_resources_device->GetImmediateContext(&sceneCtxHolder);
        if (!sceneCtxHolder || !ensureSharedBackbufferBridge(presentDevice.Get(), s_temporal_resources_device, bbDesc)) {
            return;
        }
        if (!bridgeCopyBackbufferIn(presentCtx, presentBackbuffer)) return;
        if (!sceneBridgeLock.acquire()) return;
        context = sceneCtxHolder.Get();
        backbuffer = s_shared_bb_scene.Get();
        device = s_temporal_resources_device;
    }

    static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_debug_input_copy;
    static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_debug_input_srv;
    static ID3D11Device* s_debug_pass_device = nullptr;
    static std::unique_ptr<edpe::JitterDebugPass> s_jitter_debug_pass;

    if (s_debug_pass_device != device.Get()) {
        s_debug_input_copy.Reset();
        s_debug_input_srv.Reset();
        s_debug_backbuffer_rtv.Reset();
        s_debug_cached_backbuffer = nullptr;
        s_jitter_debug_pass.reset();
        s_debug_pass_device = device.Get();
    }

    if (s_debug_cached_backbuffer != backbuffer || !s_debug_backbuffer_rtv) {
        s_debug_cached_backbuffer = backbuffer;
        s_debug_backbuffer_rtv.Reset();
        if (FAILED(device->CreateRenderTargetView(backbuffer, nullptr, &s_debug_backbuffer_rtv))) {
            return;
        }
    }

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
        float debugDtRatio = (s_last_frame_dt > 1e-5f) ? (s_current_frame_dt / s_last_frame_dt) : 1.0f;
        if (s_camera_now.scaleX > 0.0f && s_camera_previous.scaleX > 0.0f &&
            isReflectionOrAuxiliaryCamera(s_camera_now, s_camera_previous, s_prev_valid_velocity, debugDtRatio)) {
            reset = true;
        }
        isHdr = s_last_temporal_inputs.colorIsHdr;
    } else if (s_last_temporal_inputs.colorSrv != nullptr &&
               rendered > s_last_temporal_inputs_frame &&
               rendered - s_last_temporal_inputs_frame <= 2) {
        // Cobra periodically runs a 1-frame secondary probe (~once per second) or drops a frame:
        // retain previous valid HDR scene and motion inputs smoothly with zero jitter instead of
        // violently switching to the SDR backbuffer copy and resetting history, which caused a 1-frame flash.
        inputColorSrv = s_last_temporal_inputs.colorSrv;
        motionSrv = s_last_temporal_inputs.motionSrv;
        depthSrv = s_last_temporal_inputs.depthSrv;
        renderW = s_last_temporal_inputs.renderWidth ? s_last_temporal_inputs.renderWidth : bbDesc.Width;
        renderH = s_last_temporal_inputs.renderHeight ? s_last_temporal_inputs.renderHeight : bbDesc.Height;
        jx = 0.0f;
        jy = 0.0f;
        reset = false;
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
    rearmContextHooks();

    if (crossDevice) {
        context->Flush();
        sceneBridgeLock.release();
        bridgeCopyResultOut(presentCtx, presentBackbuffer);
    }

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

void renderHudDebugView(ID3D11DeviceContext* context, unsigned long long rendered,
    ID3D11Texture2D* backbuffer) {
    if (!context || !backbuffer) return;
    std::lock_guard<std::recursive_mutex> sceneLock(g_scene_ctx_mutex);

    ID3D11DeviceContext* presentCtx = context;
    ID3D11Texture2D* presentBackbuffer = backbuffer;

    D3D11_TEXTURE2D_DESC bbDesc{};
    presentBackbuffer->GetDesc(&bbDesc);
    if (!bbDesc.Width || !bbDesc.Height || bbDesc.SampleDesc.Count != 1) return;

    Microsoft::WRL::ComPtr<ID3D11Device> presentDevice;
    presentCtx->GetDevice(&presentDevice);
    if (!presentDevice) return;

    Microsoft::WRL::ComPtr<ID3D11Device> device = presentDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> sceneCtxHolder;
    const bool crossDevice = (s_temporal_resources_device &&
                              s_temporal_resources_device != presentDevice.Get() &&
                              s_last_temporal_inputs_frame == rendered);
    SceneBridgeLock sceneBridgeLock;
    if (crossDevice) {
        s_temporal_resources_device->GetImmediateContext(&sceneCtxHolder);
        if (!sceneCtxHolder || !ensureSharedBackbufferBridge(presentDevice.Get(), s_temporal_resources_device, bbDesc)) {
            return;
        }
        if (!bridgeCopyBackbufferIn(presentCtx, presentBackbuffer)) return;
        if (!sceneBridgeLock.acquire()) return;
        context = sceneCtxHolder.Get();
        backbuffer = s_shared_bb_scene.Get();
        device = s_temporal_resources_device;
    }

    static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_hud_debug_input_copy;
    static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_hud_debug_input_srv;
    static ID3D11Device* s_hud_debug_pass_device = nullptr;
    static std::unique_ptr<edpe::HudDebugPass> s_hud_debug_pass;

    if (s_hud_debug_pass_device != device.Get()) {
        s_hud_debug_input_copy.Reset();
        s_hud_debug_input_srv.Reset();
        s_hud_debug_backbuffer_rtv.Reset();
        s_hud_debug_cached_backbuffer = nullptr;
        s_hud_debug_pass.reset();
        s_hud_debug_pass_device = device.Get();
    }

    if (s_hud_debug_cached_backbuffer != backbuffer || !s_hud_debug_backbuffer_rtv) {
        s_hud_debug_cached_backbuffer = backbuffer;
        s_hud_debug_backbuffer_rtv.Reset();
        if (FAILED(device->CreateRenderTargetView(backbuffer, nullptr, &s_hud_debug_backbuffer_rtv))) {
            return;
        }
    }

    if (!s_hud_debug_pass) {
        auto pass = std::make_unique<edpe::HudDebugPass>();
        if (!pass->initialize(device.Get(), context)) {
            return;
        }
        s_hud_debug_pass = std::move(pass);
    }

    const DXGI_FORMAT typedFormat = concreteLdrColorFormat(bbDesc.Format);
    if (s_hud_debug_input_copy) {
        D3D11_TEXTURE2D_DESC cur{};
        s_hud_debug_input_copy->GetDesc(&cur);
        if (cur.Width != bbDesc.Width || cur.Height != bbDesc.Height || cur.Format != typedFormat) {
            s_hud_debug_input_copy.Reset();
            s_hud_debug_input_srv.Reset();
        }
    }
    if (!s_hud_debug_input_copy) {
        D3D11_TEXTURE2D_DESC td = bbDesc;
        td.Format = typedFormat;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = td.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &s_hud_debug_input_copy)) ||
            FAILED(device->CreateShaderResourceView(s_hud_debug_input_copy.Get(), nullptr, &s_hud_debug_input_srv))) {
            s_hud_debug_input_copy.Reset();
            s_hud_debug_input_srv.Reset();
            return;
        }
    }

    context->CopyResource(s_hud_debug_input_copy.Get(), backbuffer);
    ID3D11ShaderResourceView* currentFrameSrv = s_hud_debug_input_srv.Get();

    ID3D11ShaderResourceView* gameBackbufferSrv = nullptr;
    if (edpe::UpscalerManager::instance().isReconstructionActive() &&
        s_last_reconstructed_frame == rendered && s_backbuffer_copy_srv) {
        gameBackbufferSrv = s_backbuffer_copy_srv.Get();
    } else {
        gameBackbufferSrv = s_hud_debug_input_srv.Get();
    }

    const bool hasCurrentSceneInputs = (s_last_temporal_inputs_frame == rendered);
    ID3D11ShaderResourceView* lowResWorldSrv =
        (hasCurrentSceneInputs && s_dlaa_input_srv) ? s_dlaa_input_srv.Get() : nullptr;
    ID3D11ShaderResourceView* hudMaskSrv = (s_hud_draws_replayed_frame > 0 && s_hud_mask_srv) ? s_hud_mask_srv.Get() :
        ((s_hud_mask_prev_srv && s_hud_mask_last_valid_frame != ~0ull && rendered >= s_hud_mask_last_valid_frame && rendered - s_hud_mask_last_valid_frame <= 2) ? s_hud_mask_prev_srv.Get() : nullptr);

    const UINT renderW = s_hud_mask_width ? s_hud_mask_width : bbDesc.Width;
    const UINT renderH = s_hud_mask_height ? s_hud_mask_height : bbDesc.Height;

    const float opacity = s_hud_debug_opacity.load(std::memory_order_relaxed);
    const float threshold = s_hud_debug_threshold.load(std::memory_order_relaxed);
    const int mode = s_hud_debug_mode.load(std::memory_order_relaxed);

    bool isHdr = false;
    if (lowResWorldSrv && s_dlaa_input) {
        D3D11_TEXTURE2D_DESC td{};
        s_dlaa_input->GetDesc(&td);
        isHdr = (td.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
                 td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                 td.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS);
    }

    s_hud_debug_pass->render(context,
        currentFrameSrv,
        hudMaskSrv,
        lowResWorldSrv,
        gameBackbufferSrv,
        s_hud_debug_backbuffer_rtv.Get(),
        bbDesc.Width, bbDesc.Height,
        renderW, renderH,
        opacity, threshold, mode, isHdr);
    rearmContextHooks();

    if (crossDevice) {
        context->Flush();
        sceneBridgeLock.release();
        bridgeCopyResultOut(presentCtx, presentBackbuffer);
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

void observeDepthStencilView(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* dsv) {
    if (g_is_evaluating_upscaler.load(std::memory_order_acquire)) return;
    if (isObservedImmediateContext(context)) {
        copyCameraDeltaSource(context);
        rearmHudDrawProbe(context);
    }

    edpe::ScopedProfileTimer profTimer(edpe::ProfileZone::OMSetRenderTargets);
    const bool upscale_active = edpe::UpscalerManager::instance().isReconstructionActive();
    const bool reconstruction_active =
        upscale_active ||
        s_jitter_debug_view.load(std::memory_order_relaxed) ||
        s_hud_debug_view.load(std::memory_order_relaxed);
    const auto current_frame = last_present_frame.load(std::memory_order_relaxed);
    bool is_scene_device_ctx = true;
    if (motion_color_source && motion_color_frame == current_frame) {
        Microsoft::WRL::ComPtr<ID3D11Device> ctxDev, sceneDev;
        context->GetDevice(&ctxDev);
        motion_color_source->GetDevice(&sceneDev);
        if (ctxDev && sceneDev && ctxDev.Get() != sceneDev.Get()) {
            is_scene_device_ctx = false;
        }
    }
    if (isObservedImmediateContext(context) && is_scene_device_ctx) {
        if (s_hud_mask_active_frame != current_frame) {
            s_hud_mask_active_frame = current_frame;
            s_hud_mask_active_pass = false;
            s_hud_mask_cleared_for_frame = false;
            s_hud_draws_replayed_frame = 0;
            s_hud_merged_into_hdr_this_frame = false;
            s_ldr_tonemap_candidate_bound = false;
            s_post_ldr_backbuffer_bound = false;
            s_post_ldr_backbuffer_ui_draws = 0;
            hud_first_match_seen = false;
            s_clean_camera_substitutions.store(0, std::memory_order_relaxed);
            if (s_jitter_injected_frame != current_frame) {
                s_jittered_cb1_buffer.store(nullptr, std::memory_order_relaxed);
            }
        }
        s_ldr_tonemap_candidate_bound = false;
        s_post_ldr_backbuffer_bound = false;
        s_post_ldr_backbuffer_no_dsv = false;
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
            // Main scene G-buffer or Pass 35 HDR pass has completed and unbound!
            s_inside_main_scene.store(false, std::memory_order_release);
            s_hud_mask_active_pass = false;
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
            if (motion_color_frame == current_frame) {
                s_hdr_slot0_seen_frame = current_frame;
            }
            if (s_hud_merged_into_hdr_this_frame && dsv != nullptr) {
                s_hud_merged_into_hdr_this_frame = false;
            }
            s_hud_mask_active_pass =
                (upscale_active && s_preserve_native_ui.load(std::memory_order_relaxed)) ||
                s_hud_debug_view.load(std::memory_order_relaxed) ||
                s_jitter_debug_view.load(std::memory_order_relaxed);

            if (reconstruction_active) {
                if (!hud_draw_hook_mask) {
                    beginHudDrawProbe();
                } else {
                    rearmHudDrawProbe(context);
                }
                hud_draw_probe_active.store(true, std::memory_order_relaxed);
            }
            if (s_hud_mask_active_pass) {
                Microsoft::WRL::ComPtr<ID3D11Device> dev;
                context->GetDevice(&dev);
                if (dev && targets && count >= 1 && targets[0]) {
                    ID3D11Resource* r0 = nullptr;
                    targets[0]->GetResource(&r0);
                    if (r0) {
                        ID3D11Texture2D* t0 = nullptr;
                        if (SUCCEEDED(r0->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t0)))) {
                            D3D11_TEXTURE2D_DESC td{};
                            t0->GetDesc(&td);
                            const UINT swapW = s_swapchain_width.load(std::memory_order_relaxed);
                            const UINT swapH = s_swapchain_height.load(std::memory_order_relaxed);
                            const UINT hudW = (swapW > 0) ? swapW : td.Width;
                            const UINT hudH = (swapH > 0) ? swapH : td.Height;
                            ensureHudMaskResources(dev.Get(), hudW, hudH);
                            t0->Release();
                        }
                        r0->Release();
                    }
                }
                if (s_hud_mask_rtv && !s_hud_mask_cleared_for_frame) {
                    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                    context->ClearRenderTargetView(s_hud_mask_rtv.Get(), clearColor);
                    s_hud_mask_cleared_for_frame = true;
                    s_hud_depth_stencil_seeded = false;
                    s_hud_depth_only_seeded = false;
                }
            }
        }
        if (!motion_color_bound && bound) motion_color_rebound = true;
        motion_color_bound = bound;

        if (bound) {
            scene_ldr_source.Reset();
            scene_ldr_frame = ~0ull;
            s_pass53_blit_seen_frame = ~0ull;
        } else if (count >= 1 && targets && targets[0] &&
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
                    if (!isGbuffer && !isBackbuffer && dsv == nullptr &&
                        !s_upscale_evaluated_this_frame && s_pass53_blit_seen_frame != current_frame) {
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex0;
                        if (SUCCEEDED(res0->QueryInterface(__uuidof(ID3D11Texture2D),
                                reinterpret_cast<void**>(tex0.GetAddressOf())))) {
                            D3D11_TEXTURE2D_DESC td{};
                            tex0->GetDesc(&td);
                            if (td.Width == sceneW && td.Height == sceneH &&
                                td.SampleDesc.Count == 1) {
                                if (isLdrSceneColorFormat(td.Format)) {
                                    const bool alreadyHaveDifferentLdr =
                                        (scene_ldr_frame == current_frame && scene_ldr_source &&
                                         scene_ldr_source.Get() != tex0.Get());
                                    if (s_verified_ldr_tonemap_frame == current_frame || alreadyHaveDifferentLdr) {
                                        // Pass 53/54 post-tonemap output target on the scene device (in dual-device
                                        // setups where the scene device's final LDR output is not the swapchain
                                        // backbuffer): never overwrite the pre-UI Pass 52 scene_ldr_source.
                                        s_hud_mask_active_pass = false;
                                        hud_first_match_seen = false;
                                        s_ldr_tonemap_candidate_bound = false;
                                        s_pass53_blit_seen_frame = current_frame;
                                        s_post_ldr_backbuffer_bound = true;
                                        s_post_ldr_backbuffer_no_dsv = true;
                                    } else {
                                        s_ldr_tonemap_candidate_bound = true;
                                        if (!scene_ldr_source || scene_ldr_frame != current_frame) {
                                            scene_ldr_source = tex0;
                                            scene_ldr_frame = current_frame;
                                            s_recent_scene_ldr_source = tex0;
                                            s_recent_scene_ldr_frame = current_frame;
                                            ++s_diag_ldr_updates;
                                        }
                                    }
                                } else if (td.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
                                           td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                                           td.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS) {
                                    s_last_full_hdr_post_source = tex0;
                                    s_last_full_hdr_post_frame = current_frame;
                                }
                            }
                        }
                    } else if (isBackbuffer) {
                        s_hud_mask_active_pass = false;
                        hud_first_match_seen = false;
                        if (scene_ldr_frame == current_frame && scene_ldr_source) {
                            s_pass53_blit_seen_frame = current_frame;
                            s_post_ldr_backbuffer_bound = true;
                            s_post_ldr_backbuffer_no_dsv = (dsv == nullptr);
                        }
                    } else if (!isGbuffer && dsv != nullptr && !s_upscale_evaluated_this_frame &&
                               scene_ldr_frame == current_frame && scene_ldr_source) {
                        // Pass 54 Station Services & ESC Menu may bind an offscreen post-tonemap LDR screen
                        // target with a dedicated UI depth-stencil buffer (D24_UNORM_S8_UINT) before the final
                        // blit to the swapchain backbuffer.
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex0;
                        if (SUCCEEDED(res0->QueryInterface(__uuidof(ID3D11Texture2D),
                                reinterpret_cast<void**>(tex0.GetAddressOf())))) {
                            D3D11_TEXTURE2D_DESC td{};
                            tex0->GetDesc(&td);
                            if (td.SampleDesc.Count == 1 && isLdrSceneColorFormat(td.Format)) {
                                s_hud_mask_active_pass = false;
                                hud_first_match_seen = false;
                                s_post_ldr_backbuffer_bound = true;
                                s_post_ldr_backbuffer_no_dsv = false;
                            }
                        }
                    }
                    res0->Release();
                }
            }
        }
    }
    }
    PerContextData* ctxData = getOrCreateContextData(context);
    if (!ctxData) return;

    if (!dsv) {
        if (sequence_active.load(std::memory_order_acquire)) {
            std::lock_guard lock(seen_mutex);
            recordBind(ctxData, -1);
        }
        return;
    }
    dsv_binds.fetch_add(1, std::memory_order_relaxed);
    ctxData->dsv_binds.fetch_add(1, std::memory_order_relaxed);

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
            ID3D11Device* sceneDevice = nullptr;
            context->GetDevice(&sceneDevice);
            if (sceneDevice) {
                s_scene_device.store(sceneDevice, std::memory_order_release);
                SetPrimaryD3D11Device(sceneDevice, context);
            }
            ID3D11Device* presentDev = s_present_device.load(std::memory_order_acquire);
            if (sceneDevice && presentDev && sceneDevice != presentDev) {
                static std::atomic<bool> s_warned_scene_dev{false};
                if (!s_warned_scene_dev.exchange(true)) {
                    wchar_t warnMsg[256];
                    swprintf_s(warnMsg, L"EDPE: WARNING: scene device differs from present device (scene device=%p, present device=%p)",
                        sceneDevice, presentDev);
                    EdpeLog(warnMsg);
                }
            }
            if (sceneDevice) sceneDevice->Release();
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
                if (!hud_draw_hook_mask) {
                    beginHudDrawProbe();
                }
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
        auto& ctx_seen = ctxData->seen;
        size_t& ctx_seen_count = ctxData->seen_count;
        while (index < ctx_seen_count && ctx_seen[index].view != dsv) ++index;
        if (index == ctx_seen.size()) {
            if (sequence_active.load(std::memory_order_acquire)) recordBind(ctxData, -3);
            return;
        }
        if (index == ctx_seen_count) {
            ctx_seen[ctx_seen_count].view = dsv;
            ID3D11Device* dev = nullptr;
            dsv->GetDevice(&dev);
            if (!dev) context->GetDevice(&dev);
            ctx_seen[ctx_seen_count].owning_device = dev;
            if (dev) dev->Release();
            ID3D11Resource* init_res = nullptr;
            dsv->GetResource(&init_res);
            if (init_res) {
                ID3D11Texture2D* init_tex = nullptr;
                if (SUCCEEDED(init_res->QueryInterface(__uuidof(ID3D11Texture2D),
                        reinterpret_cast<void**>(&init_tex)))) {
                    D3D11_TEXTURE2D_DESC init_desc{};
                    init_tex->GetDesc(&init_desc);
                    ctx_seen[ctx_seen_count].texture_format = init_desc.Format;
                    ctx_seen[ctx_seen_count].samples = init_desc.SampleDesc.Count;
                    ctx_seen[ctx_seen_count].width = init_desc.Width;
                    ctx_seen[ctx_seen_count].height = init_desc.Height;
                    init_tex->Release();
                }
                init_res->Release();
            }
            ctx_seen_count++;
            first_bind = true;
        }
        ++ctx_seen[index].interval_binds;
        if (ctx_seen[index].texture_format == DXGI_FORMAT_R32G8X24_TYPELESS &&
            ctx_seen[index].samples == 1) {
            const auto frame = current_frame;
            if (ctx_seen[index].mrt_frame != frame) {
                ctx_seen[index].mrt_hdr_view = nullptr;
                ctx_seen[index].mrt_frame = frame;
            }
            ID3D11RenderTargetView* hdrRtv = nullptr;
            if (is_main_scene || scene_mrt) {
                ctx_seen[index].mrt_hdr_view = targets[3];
                if (is_main_scene) hdrRtv = targets[3];
            } else if (ctx_seen[index].mrt_hdr_view && count && targets &&
                targets[0] == ctx_seen[index].mrt_hdr_view) {
                ctx_seen[index].scene_match_frame = frame;
                if (motion_color_source && sameRenderTargetResource(targets[0], motion_color_source.Get())) {
                    hdrRtv = targets[0];
                }
            } else if (count && targets && targets[0] &&
                       motion_color_source && sameRenderTargetResource(targets[0], motion_color_source.Get())) {
                hdrRtv = targets[0];
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
        if (sequence_active.load(std::memory_order_acquire)) recordBind(ctxData, static_cast<int>(index));
        const bool is_snapshot_ctx = (!snapshot_target_context || snapshot_target_context == context);
        if (is_snapshot_ctx && snapshot_active == static_cast<int>(index) && !snapshot_view) {
            dsv->AddRef();
            snapshot_view = dsv;
            snapshot_index = snapshot_active;
            snapshot_active = -1;
            snapshot_waiting.store(false, std::memory_order_release);
        }
        if (is_snapshot_ctx && snapshot_view == dsv && snapshot_index == static_cast<int>(index)) {
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
        const bool is_pair_ctx = (!camera_pair_context || camera_pair_context == context);
        if (is_pair_ctx && camera_pair_target == static_cast<int>(index)) {
            const auto frame = last_present_frame.load(std::memory_order_relaxed);
            if (frame >= camera_pair_armed_after && camera_pair_sample_frames[1] == ~0ull) {
                if (camera_pair_bind_frame != frame) {
                    camera_pair_bind_frame = frame;
                    camera_pair_bind_count = 0;
                }
                if (++camera_pair_bind_count == 3 && is_main_scene &&
                    ctx_seen[index].mrt_frame == frame && ctx_seen[index].mrt_hdr_view) {
                    if (camera_pair_sample_frames[0] == ~0ull) {
                        camera_pair_sample_frames[0] = frame;
                        camera_pair_sample = kPairFirstSample;
                    } else if (frame > camera_pair_sample_frames[0]) {
                        camera_pair_sample_frames[1] = frame;
                        camera_pair_sample = kPairSecondSample;
                    }
                }
                if (camera_pair_sample) {
                    auto& depth = camera_pair_depth[camera_pair_sample - kPairFirstSample];
                    if (!depth.view && !depth.staging) {
                        dsv->AddRef();
                        depth.view = dsv;
                        depth.context = context;
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
        if (count && targets && targets[0] && !ctx_seen[index].color_logged) {
            ctx_seen[index].color_logged = true;
            first_color = true;
        }
    }

    if (context != observed_context.load(std::memory_order_acquire)) {
        findSceneContextData(true);
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
        ctxData->seen[index].texture_format = texture_desc.Format;
        ctxData->seen[index].samples = texture_desc.SampleDesc.Count;
        ctxData->seen[index].width = texture_desc.Width;
        ctxData->seen[index].height = texture_desc.Height;
        const UINT scW = s_swapchain_width.load(std::memory_order_relaxed);
        const UINT scH = s_swapchain_height.load(std::memory_order_relaxed);
        const UINT sceneW = s_observed_scene_width.load(std::memory_order_relaxed);
        const UINT sceneH = s_observed_scene_height.load(std::memory_order_relaxed);
        const UINT expectedW = sceneW ? sceneW : scW;
        const UINT expectedH = sceneH ? sceneH : scH;
        const bool has_rtv0 = (count >= 1 && targets && targets[0] != nullptr);
        const bool is_dsv_format = (texture_desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
                                    texture_desc.Format == DXGI_FORMAT_R24G8_TYPELESS);
        const bool is_square_shadow = (scW > scH && texture_desc.Width == texture_desc.Height);

        UINT curMaxW = ctxData->maxDsvWidth.load(std::memory_order_relaxed);
        UINT curMaxH = ctxData->maxDsvHeight.load(std::memory_order_relaxed);
        if (has_rtv0 && is_dsv_format && !is_square_shadow &&
            (texture_desc.Width > curMaxW || (texture_desc.Width == curMaxW && texture_desc.Height > curMaxH))) {
            ctxData->maxDsvWidth.store(texture_desc.Width, std::memory_order_relaxed);
            ctxData->maxDsvHeight.store(texture_desc.Height, std::memory_order_relaxed);
        }
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

thread_local bool s_in_om_detour = false;

OMSetRenderTargetsFn resolveOriginalOM(ID3D11DeviceContext* context) {
    if (!context) return original.load(std::memory_order_acquire);
    void** table = *reinterpret_cast<void***>(context);
    const size_t cloneCount = s_cloned_context_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < cloneCount; ++i) {
        if (s_cloned_contexts[i] &&
            (s_cloned_contexts[i]->context == context || s_cloned_contexts[i]->methods == table)) {
            if (s_cloned_contexts[i]->orig_om && s_cloned_contexts[i]->orig_om != &observedOMSetRenderTargets)
                return s_cloned_contexts[i]->orig_om;
            table = s_cloned_contexts[i]->original_table;
            break;
        }
    }
    const size_t tblCount = s_hooked_table_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < tblCount; ++i) {
        if (s_hooked_tables[i].table == table && s_hooked_tables[i].orig_om &&
            s_hooked_tables[i].orig_om != &observedOMSetRenderTargets) {
            return s_hooked_tables[i].orig_om;
        }
    }
    const bool is_deferred = (context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED);
    OMSetRenderTargetsFn forward = is_deferred
        ? original_deferred_om.load(std::memory_order_acquire)
        : original.load(std::memory_order_acquire);
    if (!forward) forward = original.load(std::memory_order_acquire);
    return forward;
}

OMSetRenderTargetsAndUnorderedAccessViewsFn resolveOriginalOMUav(ID3D11DeviceContext* context) {
    if (!context) return original_om_uav.load(std::memory_order_acquire);
    void** table = *reinterpret_cast<void***>(context);
    const size_t cloneCount = s_cloned_context_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < cloneCount; ++i) {
        if (s_cloned_contexts[i] &&
            (s_cloned_contexts[i]->context == context || s_cloned_contexts[i]->methods == table)) {
            if (s_cloned_contexts[i]->orig_om_uav &&
                s_cloned_contexts[i]->orig_om_uav != &observedOMSetRenderTargetsAndUnorderedAccessViews)
                return s_cloned_contexts[i]->orig_om_uav;
            table = s_cloned_contexts[i]->original_table;
            break;
        }
    }
    const size_t tblCount = s_hooked_table_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < tblCount; ++i) {
        if (s_hooked_tables[i].table == table && s_hooked_tables[i].orig_om_uav &&
            s_hooked_tables[i].orig_om_uav != &observedOMSetRenderTargetsAndUnorderedAccessViews) {
            return s_hooked_tables[i].orig_om_uav;
        }
    }
    const bool is_deferred = (context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED);
    OMSetRenderTargetsAndUnorderedAccessViewsFn forward = is_deferred
        ? original_deferred_om_uav.load(std::memory_order_acquire)
        : original_om_uav.load(std::memory_order_acquire);
    if (!forward) forward = original_om_uav.load(std::memory_order_acquire);
    return forward;
}



void STDMETHODCALLTYPE observedOMSetRenderTargets(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* dsv) {
    OMSetRenderTargetsFn forward = resolveOriginalOM(context);
    if (g_is_evaluating_upscaler.load(std::memory_order_acquire)) {
        if (forward) forward(context, count, targets, dsv);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_scene_ctx_mutex);
    if (s_in_om_detour) {
        if (forward) forward(context, count, targets, dsv);
        if (isObservedImmediateContext(context)) rearmHudDrawProbe(context);
        return;
    }
    struct DetourGuard {
        DetourGuard() { s_in_om_detour = true; }
        ~DetourGuard() { s_in_om_detour = false; }
    } detour_guard;

    s_count_om_set_render_targets.fetch_add(1, std::memory_order_relaxed);
    logContextHookOnce(context, kOMSetRenderTargets, L"OMSetRenderTargets", s_log_om_imm, s_log_om_def);

    PerContextData* ctxData = getOrCreateContextData(context);
    const bool in_edpe = s_in_edpe_present.load(std::memory_order_acquire);
    if (ctxData) {
        ctxData->omCalls.fetch_add(1, std::memory_order_relaxed);
        if (in_edpe) {
            ctxData->edpeOmCalls.fetch_add(1, std::memory_order_relaxed);
        } else {
            ctxData->gameOmCalls.fetch_add(1, std::memory_order_relaxed);
        }
        if (dsv) {
            ctxData->nonNullDsvCalls.fetch_add(1, std::memory_order_relaxed);
            if (in_edpe) {
                ctxData->edpeNonNullDsvCalls.fetch_add(1, std::memory_order_relaxed);
            } else {
                ctxData->gameNonNullDsvCalls.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    if (in_edpe) {
        if (forward) forward(context, count, targets, dsv);
        return;
    }
    if (isObservedImmediateContext(context)) {
        endVSBufferProbe();
        endPipelineSample(context);
    }
    if (forward) forward(context, count, targets, dsv);

    observeDepthStencilView(context, count, targets, dsv);
}

void STDMETHODCALLTYPE observedOMSetRenderTargetsAndUnorderedAccessViews(
    ID3D11DeviceContext* context,
    UINT numRTVs,
    ID3D11RenderTargetView* const* targets,
    ID3D11DepthStencilView* dsv,
    UINT uavStartSlot,
    UINT numUAVs,
    ID3D11UnorderedAccessView* const* uavs,
    const UINT* initialCounts) {
    OMSetRenderTargetsAndUnorderedAccessViewsFn forward = resolveOriginalOMUav(context);
    if (g_is_evaluating_upscaler.load(std::memory_order_acquire)) {
        if (forward) forward(context, numRTVs, targets, dsv, uavStartSlot, numUAVs, uavs, initialCounts);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_scene_ctx_mutex);
    if (s_in_om_detour) {
        if (forward) forward(context, numRTVs, targets, dsv, uavStartSlot, numUAVs, uavs, initialCounts);
        if (isObservedImmediateContext(context)) rearmHudDrawProbe(context);
        return;
    }
    struct DetourGuard {
        DetourGuard() { s_in_om_detour = true; }
        ~DetourGuard() { s_in_om_detour = false; }
    } detour_guard;

    s_count_om_set_rt_uav.fetch_add(1, std::memory_order_relaxed);
    logContextHookOnce(context, kOMSetRenderTargetsAndUnorderedAccessViews,
        L"OMSetRenderTargetsAndUnorderedAccessViews", s_log_om_uav_imm, s_log_om_uav_def);

    PerContextData* ctxData = getOrCreateContextData(context);
    const bool in_edpe = s_in_edpe_present.load(std::memory_order_acquire);
    if (ctxData) {
        ctxData->omUavCalls.fetch_add(1, std::memory_order_relaxed);
        if (in_edpe) {
            ctxData->edpeOmUavCalls.fetch_add(1, std::memory_order_relaxed);
        } else {
            ctxData->gameOmUavCalls.fetch_add(1, std::memory_order_relaxed);
        }
        if (dsv) {
            ctxData->nonNullDsvCalls.fetch_add(1, std::memory_order_relaxed);
            if (in_edpe) {
                ctxData->edpeNonNullDsvCalls.fetch_add(1, std::memory_order_relaxed);
            } else {
                ctxData->gameNonNullDsvCalls.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    if (in_edpe) {
        if (forward) forward(context, numRTVs, targets, dsv, uavStartSlot, numUAVs, uavs, initialCounts);
        return;
    }
    if (isObservedImmediateContext(context)) {
        endVSBufferProbe();
        endPipelineSample(context);
    }
    if (forward) forward(context, numRTVs, targets, dsv, uavStartSlot, numUAVs, uavs, initialCounts);

    if (numRTVs == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        if (isObservedImmediateContext(context)) {
            rearmHudDrawProbe(context);
        }
        return;
    }
    observeDepthStencilView(context, numRTVs, targets, dsv);
}

void STDMETHODCALLTYPE observedExecuteCommandList(
    ID3D11DeviceContext* context,
    ID3D11CommandList* pCommandList,
    BOOL restoreContextState) {
    s_count_execute_command_list.fetch_add(1, std::memory_order_relaxed);
    logContextHookOnce(context, kExecuteCommandList, L"ExecuteCommandList", s_log_exec_imm, s_log_exec_def);

    void** table = *reinterpret_cast<void***>(context);
    ExecuteCommandListFn forward = nullptr;
    const size_t tblCount = s_hooked_table_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < tblCount; ++i) {
        if (s_hooked_tables[i].table == table) {
            forward = s_hooked_tables[i].orig_exec;
            break;
        }
    }
    if (!forward) {
        const bool is_deferred = (context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED);
        forward = is_deferred ? original_deferred_execute_command_list.load(std::memory_order_acquire)
                              : original_execute_command_list.load(std::memory_order_acquire);
        if (!forward) forward = original_execute_command_list.load(std::memory_order_acquire);
    }
    if (forward) forward(context, pCommandList, restoreContextState);
}

HRESULT STDMETHODCALLTYPE observedFinishCommandList(
    ID3D11DeviceContext* context,
    BOOL restoreDeferredContextState,
    ID3D11CommandList** ppCommandList) {
    s_count_finish_command_list.fetch_add(1, std::memory_order_relaxed);
    logContextHookOnce(context, kFinishCommandList, L"FinishCommandList", s_log_finish_imm, s_log_finish_def);

    void** table = *reinterpret_cast<void***>(context);
    FinishCommandListFn forward = nullptr;
    const size_t tblCount = s_hooked_table_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < tblCount; ++i) {
        if (s_hooked_tables[i].table == table) {
            forward = s_hooked_tables[i].orig_finish;
            break;
        }
    }
    if (!forward) {
        const bool is_deferred = (context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED);
        forward = is_deferred ? original_deferred_finish_command_list.load(std::memory_order_acquire)
                              : original_finish_command_list.load(std::memory_order_acquire);
        if (!forward) forward = original_finish_command_list.load(std::memory_order_acquire);
    }
    return forward ? forward(context, restoreDeferredContextState, ppCommandList) : E_FAIL;
}


bool patchSlot(void** table, size_t slot, void* expected, void* replacement) {
    if (!table) return false;
    DWORD old_protection = 0;
    if (!VirtualProtect(table + slot, sizeof(void*), PAGE_READWRITE, &old_protection)) {
        if (!VirtualProtect(table + slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protection)) {
            return false;
        }
    }
    void* replaced = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(table + slot), replacement, expected);
    DWORD ignored = 0;
    VirtualProtect(table + slot, sizeof(void*), old_protection, &ignored);
    if (replaced == expected) {
        const size_t count = s_hooked_table_count.load(std::memory_order_acquire);
        for (size_t i = 0; i < count; ++i) {
            if (s_context_vtable_hooks[i] && s_context_vtable_hooks[i]->isInitialized()) {
                if (s_context_vtable_hooks[i]->activeTable() == table ||
                    s_context_vtable_hooks[i]->originalTable() == table) {
                    s_context_vtable_hooks[i]->notifySlotPatched(slot, replacement);
                }
            }
        }
        return true;
    }
    return false;
}

void rearmContextHooks() {
    if (g_is_evaluating_upscaler.load(std::memory_order_acquire)) return;
    const size_t hookCount = s_hooked_table_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < hookCount; ++i) {
        if (s_context_vtable_hooks[i] && s_context_vtable_hooks[i]->isInitialized()) {
            s_context_vtable_hooks[i]->reclaim();
        }
    }
    static bool s_swap_logged = false;
    if (!s_swap_logged && motion_hook_watch > 0 && patched_table) {
        s_swap_logged = true;
        EdpeLog(L"EDPE: DSV observer restored after context-state swap");
        EdpeLog(L"EDPE: vtable slot 33 (OMSetRenderTargets) match");
        EdpeLog(L"EDPE: vtable slot 34 (OMSetRenderTargetsAndUAVs) match");
    }

    if (g_is_evaluating_upscaler.load(std::memory_order_acquire)) return;
    if (!patched_table && s_hooked_table_count.load(std::memory_order_acquire) == 0) return;
    if (patched_table) {
    const auto forward_om = original.load(std::memory_order_acquire);
    if (forward_om && patched_table[kOMSetRenderTargets] != reinterpret_cast<void*>(&observedOMSetRenderTargets)) {
        const bool restored = patchSlot(patched_table, kOMSetRenderTargets,
            patched_table[kOMSetRenderTargets],
            reinterpret_cast<void*>(&observedOMSetRenderTargets));
        if (restored && !s_swap_logged) {
            s_swap_logged = true;
            EdpeLog(L"EDPE: DSV observer restored after context-state swap");

            void* cur_om = patched_table[kOMSetRenderTargets];
            void* exp_om = reinterpret_cast<void*>(&observedOMSetRenderTargets);
            if (cur_om == exp_om) {
                EdpeLog(L"EDPE: vtable slot 33 (OMSetRenderTargets) match");
            } else {
                wchar_t msg[160];
                swprintf_s(msg, L"EDPE: vtable slot 33 (OMSetRenderTargets) mismatch current=%p expected=%p; re-patching", cur_om, exp_om);
                EdpeLog(msg);
                patchSlot(patched_table, kOMSetRenderTargets, cur_om, exp_om);
            }

            void* cur_uav = patched_table[kOMSetRenderTargetsAndUnorderedAccessViews];
            void* exp_uav = reinterpret_cast<void*>(&observedOMSetRenderTargetsAndUnorderedAccessViews);
            if (cur_uav == exp_uav) {
                EdpeLog(L"EDPE: vtable slot 34 (OMSetRenderTargetsAndUAVs) match");
            } else {
                wchar_t msg[160];
                swprintf_s(msg, L"EDPE: vtable slot 34 (OMSetRenderTargetsAndUAVs) mismatch current=%p expected=%p; re-patching", cur_uav, exp_uav);
                EdpeLog(msg);
                patchSlot(patched_table, kOMSetRenderTargetsAndUnorderedAccessViews, cur_uav, exp_uav);
            }
        }
    }
    const auto forward_uav = original_om_uav.load(std::memory_order_acquire);
    if (forward_uav && patched_table[kOMSetRenderTargetsAndUnorderedAccessViews] != reinterpret_cast<void*>(&observedOMSetRenderTargetsAndUnorderedAccessViews)) {
        patchSlot(patched_table, kOMSetRenderTargetsAndUnorderedAccessViews,
            patched_table[kOMSetRenderTargetsAndUnorderedAccessViews],
            reinterpret_cast<void*>(&observedOMSetRenderTargetsAndUnorderedAccessViews));
    }
    const auto forward_exec = original_execute_command_list.load(std::memory_order_acquire);
    if (forward_exec && patched_table[kExecuteCommandList] != reinterpret_cast<void*>(&observedExecuteCommandList)) {
        patchSlot(patched_table, kExecuteCommandList,
            patched_table[kExecuteCommandList],
            reinterpret_cast<void*>(&observedExecuteCommandList));
    }
    const auto forward_finish = original_finish_command_list.load(std::memory_order_acquire);
    if (forward_finish && patched_table[kFinishCommandList] != reinterpret_cast<void*>(&observedFinishCommandList)) {
        patchSlot(patched_table, kFinishCommandList,
            patched_table[kFinishCommandList],
            reinterpret_cast<void*>(&observedFinishCommandList));
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
    if (hud_draw_probe_active.load(std::memory_order_acquire)) {
        installHudStateRearmHooks();
    }
    rearmHudDrawProbe();

    const size_t tblCount = s_hooked_table_count.load(std::memory_order_acquire);
    const bool map_active = map_hook_active.load(std::memory_order_acquire);
    const bool unmap_active = unmap_hook_active.load(std::memory_order_acquire);
    for (size_t i = 0; i < tblCount; ++i) {
        auto& entry = s_hooked_tables[i];
        if (!entry.table) continue;
        if (entry.orig_om &&
            entry.table[kOMSetRenderTargets] != reinterpret_cast<void*>(&observedOMSetRenderTargets)) {
            patchSlot(entry.table, kOMSetRenderTargets, entry.table[kOMSetRenderTargets],
                reinterpret_cast<void*>(&observedOMSetRenderTargets));
        }
        if (entry.orig_om_uav &&
            entry.table[kOMSetRenderTargetsAndUnorderedAccessViews] !=
                reinterpret_cast<void*>(&observedOMSetRenderTargetsAndUnorderedAccessViews)) {
            patchSlot(entry.table, kOMSetRenderTargetsAndUnorderedAccessViews,
                entry.table[kOMSetRenderTargetsAndUnorderedAccessViews],
                reinterpret_cast<void*>(&observedOMSetRenderTargetsAndUnorderedAccessViews));
        }
        if (map_active && entry.orig_map &&
            entry.table[kMap] != reinterpret_cast<void*>(&observedMap)) {
            patchSlot(entry.table, kMap, entry.table[kMap],
                reinterpret_cast<void*>(&observedMap));
        }
        if (unmap_active && entry.orig_unmap &&
            entry.table[kUnmap] != reinterpret_cast<void*>(&observedUnmap)) {
            patchSlot(entry.table, kUnmap, entry.table[kUnmap],
                reinterpret_cast<void*>(&observedUnmap));
        }
    }
}
} // namespace

bool ContextCensusIsObservedSwapChain(IDXGISwapChain* swap_chain) {
    return swap_chain && observed_swap_chain == swap_chain;
}

bool ContextCensusSelectSwapChain(IDXGISwapChain* swap_chain, UINT flags) {
    if (!swap_chain) return false;
    if (!observed_swap_chain) {
        if ((flags & DXGI_PRESENT_TEST) && edpe::isRunningInEliteProcess()) {
            return false;
        }
        observed_swap_chain = swap_chain;
        ID3D11Device* dev = nullptr;
        if (SUCCEEDED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev) {
            s_present_device.store(dev, std::memory_order_release);
            dev->Release();
        }
        return true;
    }
    if (observed_swap_chain == swap_chain) {
        return true;
    }
    if (flags & DXGI_PRESENT_TEST) {
        return false;
    }

    ID3D11Device* candDev = nullptr;
    if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&candDev))) || !candDev) {
        return false;
    }
    ID3D11Device* curDev = nullptr;
    if (FAILED(observed_swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&curDev)))) {
        curDev = s_present_device.load(std::memory_order_acquire);
    }

    ID3D11Device* authDev = s_scene_device.load(std::memory_order_acquire);
    if (!authDev) {
        authDev = GetPrimaryD3D11Device();
    }

    const bool preferCandidate = (authDev != nullptr && candDev == authDev && curDev != authDev);
    if (preferCandidate) {
        wchar_t msg[256];
        swprintf_s(msg,
            L"EDPE: switching observed swapchain from %p (device=%p) to scene-device swapchain %p (device=%p)",
            observed_swap_chain, curDev, swap_chain, candDev);
        EdpeLog(msg);

        {
            std::lock_guard<std::recursive_mutex> sceneLock(g_scene_ctx_mutex);
            std::lock_guard lock(seen_mutex);
            s_backbuffer_rtv.Reset();
            s_cached_backbuffer_ptr = nullptr;
            s_debug_backbuffer_rtv.Reset();
            s_debug_cached_backbuffer = nullptr;
            s_hud_debug_backbuffer_rtv.Reset();
            s_hud_debug_cached_backbuffer = nullptr;
            s_swapchain_backbuffer.store(nullptr, std::memory_order_relaxed);
            resetTemporalPathDebounce();
            resetSharedBackbufferBridge();
            s_last_final_output_present.Reset();
            s_last_reconstructed_output.Reset();
        }
        if (auto* prevCtx = observed_context.exchange(nullptr, std::memory_order_acq_rel)) {
            prevCtx->Release();
        }
        observed_swap_chain = swap_chain;
        s_present_device.store(candDev, std::memory_order_release);
        attempted = false;
    }

    if (curDev) curDev->Release();
    candDev->Release();
    return preferCandidate;
}

void ContextCensusOnPresent(IDXGISwapChain* swap_chain, unsigned long long frame, UINT flags) {
    if (!s_config_loaded) {
        s_config_loaded = true;
        auto& cfg = edpe::Config::instance();
        cfg.load();
        jitter_enabled.store(cfg.jitterEnabled, std::memory_order_relaxed);
        s_jitter_invert_x.store(cfg.jitterInvertX, std::memory_order_relaxed);
        s_jitter_invert_y.store(cfg.jitterInvertY, std::memory_order_relaxed);
        s_jitter_swap_xy.store(cfg.jitterSwapXY, std::memory_order_relaxed);
        s_jitter_scale.store(cfg.jitterScale, std::memory_order_relaxed);
        s_jitter_phase_count.store(cfg.jitterPhaseCount, std::memory_order_relaxed);
        s_preserve_native_ui.store(cfg.preserveNativeUi, std::memory_order_relaxed);
        s_sharpness.store(cfg.sharpness, std::memory_order_relaxed);
        s_jitter_debug_view.store(false, std::memory_order_relaxed);
        s_jitter_debug_sensitivity.store(cfg.jitterDebugSensitivity, std::memory_order_relaxed);
        s_jitter_debug_threshold.store(cfg.jitterDebugThreshold, std::memory_order_relaxed);
        s_jitter_debug_mode.store(cfg.jitterDebugMode, std::memory_order_relaxed);
        s_hud_debug_view.store(false, std::memory_order_relaxed);
        s_hud_debug_opacity.store(cfg.hudDebugOpacity, std::memory_order_relaxed);
        s_hud_debug_threshold.store(cfg.hudDebugThreshold, std::memory_order_relaxed);
        s_hud_debug_mode.store(cfg.hudDebugMode, std::memory_order_relaxed);
        s_zero_motion_vectors.store(cfg.zeroMotionVectors, std::memory_order_relaxed);
        s_motion_invert_x.store(cfg.motionInvertX, std::memory_order_relaxed);
        s_motion_invert_y.store(cfg.motionInvertY, std::memory_order_relaxed);
        s_motion_scale.store(cfg.motionScale, std::memory_order_relaxed);
        edpe::NgxContext::set_preset(static_cast<uint32_t>(cfg.dlssPreset));
        edpe::JitterLogger::instance().setSampleInterval(cfg.jitterLogInterval);
        edpe::JitterLogger::instance().setFlushIntervalMs(cfg.jitterLogFlushMs);
        edpe::JitterLogger::instance().setLoggingEnabled(cfg.devMode && cfg.jitterLogEnabled);

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
        std::lock_guard<std::recursive_mutex> sceneLock(g_scene_ctx_mutex);
        auto* context = activeSceneContext();
        const auto cur_rendered = last_present_frame.load(std::memory_order_relaxed);
        finalizeHudMaskForFrame(context, cur_rendered);
        const bool rec_active = edpe::UpscalerManager::instance().isReconstructionActive() ||
                                s_jitter_debug_view.load(std::memory_order_relaxed) ||
                                s_hud_debug_view.load(std::memory_order_relaxed);
        if (!rec_active && hud_draw_hook_mask) {
            endHudDrawProbe();
        }
        endVSBufferProbe();
        endPipelineSample(context);
        pollPipelineSamples(context);
    }
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain)
        pollConstantBufferSamples(activeSceneContext());
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain) {
        auto* context = activeSceneContext();
        copyCameraDeltaSource(context);
        pollCameraDeltaRing(context);
    }
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain) {
        auto* context = activeSceneContext();
        queueCameraPairDepth(context, frame);
        pollCameraPairDepth(context);
        pollMotionGrid(context);
        tryMotionPair(context, frame);
    }
    const auto rendered_frame = last_present_frame.load(std::memory_order_relaxed);
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain) {
        last_present_frame.store(frame, std::memory_order_relaxed);
        edpe::setCrashFrameNumber(frame);
    }
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain) {
        int finished_pair = -1;
        {
            std::lock_guard lock(seen_mutex);
            if (camera_pair_target >= 0 &&
                ((camera_pair_sample_frames[1] != ~0ull && frame > camera_pair_sample_frames[1]) ||
                 frame >= camera_pair_armed_after + 120)) {
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
        const size_t ctxCount = s_tracked_context_count.load(std::memory_order_acquire);
        bool any_logged = false;
        for (size_t c = 0; c < ctxCount; ++c) {
            auto* data = s_tracked_contexts[c].get();
            if (!data) continue;
            std::array<int, 64> sequence{};
            size_t stored = 0;
            unsigned transitions = 0;
            {
                std::lock_guard lock(seen_mutex);
                sequence = data->bind_sequence;
                stored = data->sequence_stored;
                transitions = data->sequence_transitions;
            }
            if (transitions == 0 && (c + 1 < ctxCount || any_logged)) continue;
            any_logged = true;
            wchar_t message[160];
            swprintf_s(message, L"EDPE: DSV bind sequence frame=%llu transitions=%u stored=%zu ctx=%p",
                frame, transitions, stored, data->context);
            EdpeLog(message);
            for (size_t i = 0; i < stored; ++i) {
                swprintf_s(message, L"EDPE: DSV bind sequence %zu target=%d", i, sequence[i]);
                EdpeLog(message);
            }
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
        if (!table) {
            context->Release();
            return;
        }

        ContextCensusHookContext(context);
        table = *reinterpret_cast<void***>(context);
        observed_context.store(context, std::memory_order_release);
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
        if (map_forward && map_forward != &observedMap) original_map.store(map_forward, std::memory_order_release);
        if (unmap_forward && unmap_forward != &observedUnmap) original_unmap.store(unmap_forward, std::memory_order_release);

        bool map_ok = map_forward && (map_forward == &observedMap || patchSlot(table, kMap,
            reinterpret_cast<void*>(map_forward), reinterpret_cast<void*>(&observedMap)));
        bool unmap_ok = unmap_forward && (unmap_forward == &observedUnmap || patchSlot(table, kUnmap,
            reinterpret_cast<void*>(unmap_forward), reinterpret_cast<void*>(&observedUnmap)));
        map_hook_active.store(map_ok, std::memory_order_release);
        unmap_hook_active.store(unmap_ok, std::memory_order_release);
        wchar_t hook_msg[128];
        swprintf_s(hook_msg, L"EDPE: Map/Unmap hooks armed map=%u unmap=%u", map_ok, unmap_ok);
        EdpeLog(hook_msg);
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
                const bool hud_debug_active = s_hud_debug_view.load(std::memory_order_relaxed);
                advanceComparisonSequence();
                if (!s_upscale_evaluated_this_frame &&
                    (edpe::UpscalerManager::instance().isReconstructionActive() || debug_view_active || hud_debug_active)) {
                    tryTemporalUpscale(context, rendered_frame, backbuffer.Get());
                }
                if (debug_view_active) {
                    renderJitterDebugView(context, rendered_frame, backbuffer.Get());
                }
                if (hud_debug_active) {
                    renderHudDebugView(context, rendered_frame, backbuffer.Get());
                }
                s_hud_mask_latest_draws.store(s_hud_draws_replayed_frame, std::memory_order_relaxed);
                if (s_hud_draws_replayed_frame == 0) {
                    s_hud_mask_latest_coverage.store(0.0f, std::memory_order_relaxed);
                    s_hud_mask_coverage_pending_frame = ~0ull;
                } else if (hud_debug_active && s_hud_mask_texture && s_hud_mask_device) {
                    std::lock_guard<std::recursive_mutex> sceneLock(g_scene_ctx_mutex);
                    Microsoft::WRL::ComPtr<ID3D11DeviceContext> hudCtx;
                    s_hud_mask_device->GetImmediateContext(&hudCtx);
                    if (hudCtx) {
                        const unsigned check_interval = 30;
                        if (rendered_frame % check_interval == 0) {
                            ensureHudMaskStaging(s_hud_mask_device, s_hud_mask_width, s_hud_mask_height);
                            if (s_hud_mask_staging) {
                                hudCtx->CopyResource(s_hud_mask_staging.Get(), s_hud_mask_texture.Get());
                                s_hud_mask_coverage_pending_frame = rendered_frame;
                            }
                        }
                        if (s_hud_mask_staging && s_hud_mask_coverage_pending_frame != ~0ull &&
                            rendered_frame > s_hud_mask_coverage_pending_frame) {
                            D3D11_MAPPED_SUBRESOURCE mapped{};
                            const HRESULT hr = hudCtx->Map(s_hud_mask_staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                            if (SUCCEEDED(hr)) {
                                size_t nonZero = 0;
                                size_t totalPixels = 0;
                                const auto* ptr = static_cast<const uint8_t*>(mapped.pData);
                                for (UINT y = 0; y < s_hud_mask_staging_height; y += 16) {
                                    const auto* row = reinterpret_cast<const uint16_t*>(ptr + y * mapped.RowPitch);
                                    for (UINT x = 0; x < s_hud_mask_staging_width; x += 16) {
                                        ++totalPixels;
                                        const uint16_t r = row[x * 4 + 0];
                                        const uint16_t g = row[x * 4 + 1];
                                        const uint16_t b = row[x * 4 + 2];
                                        const uint16_t a = row[x * 4 + 3];
                                        // Non-zero color or transmittance < ~0.95 (half-float 0x3BC0 is ~0.94)
                                        if (r > 0x1000 || g > 0x1000 || b > 0x1000 || a < 0x3BC0) {
                                            ++nonZero;
                                        }
                                    }
                                }
                                hudCtx->Unmap(s_hud_mask_staging.Get(), 0);
                                const float coverage = (totalPixels > 0) ? (static_cast<float>(nonZero) * 100.0f / totalPixels) : 0.0f;
                                s_hud_mask_latest_coverage.store(coverage, std::memory_order_relaxed);
                                if (s_hud_mask_coverage_pending_frame % 300 == 0) {
                                    wchar_t msg[128];
                                    swprintf_s(msg, L"EDPE: HUD mask coverage=%.2f%% frame=%llu", coverage, s_hud_mask_coverage_pending_frame);
                                    EdpeLog(msg);
                                }
                                s_hud_mask_coverage_pending_frame = ~0ull;
                            } else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
                                s_hud_mask_coverage_pending_frame = ~0ull;
                            }
                        }
                    }
                }
                if (edpe::UpscalerManager::instance().isReconstructionActive() &&
                    jittered_writes_count.load(std::memory_order_relaxed) == 0 &&
                    motion_color_frame == rendered_frame) {
                    static unsigned long long s_last_zero_write_warn = 0;
                    if (rendered_frame > s_last_zero_write_warn + 60) {
                        s_last_zero_write_warn = rendered_frame;
                        wchar_t zMsg[256];
                        swprintf_s(zMsg,
                            L"EDPE: zero jitter writes on scene frame %llu (mrt_frame=%llu, hdr0_frame=%llu, has_staged=%d, staged_frame=%llu)",
                            rendered_frame, motion_color_frame, s_hdr_slot0_seen_frame, (int)s_has_staged_camera, s_staged_camera_frame);
                        EdpeLog(zMsg);
                    }
                }
            }
        }
        // Fail open (AGENTS.md "Projection jitter"): Elite must never keep rendering a
        // jittered image that reconstruction did not resolve this frame; the raw
        // sub-pixel offset shows up as the whole image constantly shaking.
        // Stand the jitter down with back-off until reconstruction works again.
        if (s_last_reconstructed_frame == rendered_frame) {
            s_jitter_standdown_until.store(0, std::memory_order_relaxed);
            s_jitter_standdown_len = 2;
            s_unreconstructed_jitter_streak = 0;
        } else if (s_jitter_injected_frame == rendered_frame &&
            !s_jitter_debug_view.load(std::memory_order_relaxed) &&
            !s_hud_debug_view.load(std::memory_order_relaxed) &&
            !jitter_test_override.load(std::memory_order_relaxed)) {
            if (s_probe_seen_this_frame && !s_has_staged_camera) {
                // Зондовий кадр без основної сцени: не накладаємо штраф standdown і не скидаємо фазу
            } else if (++s_unreconstructed_jitter_streak >= 8) {
                s_jitter_standdown_until.store(rendered_frame + 2ull, std::memory_order_relaxed);
                wchar_t standdown_msg[200];
                swprintf_s(standdown_msg, L"EDPE: jitter stood down for 2 frames (unreconstructed streak=%u)",
                    s_unreconstructed_jitter_streak);
                EdpeLog(standdown_msg);
            }
        }
        s_probe_seen_this_frame = false;
        s_ustat = UnmapFrameStats{};
        if (!s_upscale_evaluated_this_frame) {
            bool dummy_cut = false;
            if (s_has_staged_camera && s_staged_camera_frame == rendered_frame) {
                commitMainCamera(rendered_frame, dummy_cut);
            }
        }
        if (edpe::Config::instance().devMode && edpe::UpscalerManager::instance().isReconstructionActive()) {
            // HUD diagnostics: one summary line per 60 frames, plus detailed per-shader-pair
            // blink diagnostics ("HUD BLINK DIAG") on any frame where total or per-pair HUD draws
            // change, a draw is declined, hooks are overwritten, or evaluation stage changes.
            static unsigned s_diag_prev_replayed = 0;
            static int s_diag_prev_eval_stage = 0;
            static unsigned s_diag_anomaly_budget = 500;
            bool pair_changed = false;
            if (s_hud_pair_stats_prev_count > 0) {
                for (size_t i = 0; i < s_hud_pair_stats_prev_count; ++i) {
                    const auto& prev = s_hud_pair_stats_prev[i];
                    unsigned cur_rep = 0;
                    for (size_t j = 0; j < s_hud_pair_stats_cur_count; ++j) {
                        if (s_hud_pair_stats_cur[j].vs == prev.vs && s_hud_pair_stats_cur[j].ps == prev.ps) {
                            cur_rep = s_hud_pair_stats_cur[j].replayed;
                            break;
                        }
                    }
                    if (cur_rep != prev.replayed) {
                        pair_changed = true;
                        break;
                    }
                }
                if (!pair_changed && s_hud_pair_stats_cur_count != s_hud_pair_stats_prev_count) {
                    pair_changed = true;
                }
            }
            const bool count_changed = (s_diag_prev_replayed > 0 && s_diag_hud_replayed != s_diag_prev_replayed);
            const bool stage_changed = (s_diag_prev_eval_stage != 0 && s_diag_eval_stage != s_diag_prev_eval_stage);
            const bool anomaly = (count_changed || pair_changed || s_diag_hud_declined_draws > 0 || stage_changed) &&
                                 s_diag_anomaly_budget > 0;
            if (rendered_frame % 60 == 0 || anomaly) {
                if (anomaly) --s_diag_anomaly_budget;
                wchar_t dmsg[512];
                swprintf_s(dmsg,
                    L"EDPE: HUD diag frame=%llu%ls probe=%d hookMask=0x%X rearmed=0x%X unhooks=%u candidates=%u replayed=%u prevReplayed=%u declined=%u slot=%u evalStage=%d(prev=%d) ldrUpdates=%u composite=%d hudSrc=%d maskW=%u cleanCb=%d subs=%u jitter=%d",
                    rendered_frame, anomaly ? L" [BLINK/DELTA]" : L"",
                    hud_draw_probe_active.load(std::memory_order_relaxed) ? 1 : 0,
                    hud_draw_hook_mask, hud_draw_rearmed_mask,
                    s_diag_hud_unhook_events,
                    s_diag_hud_candidates, s_diag_hud_replayed, s_diag_prev_replayed,
                    s_diag_hud_declined_draws,
                    s_hud_draws_replayed_frame, s_diag_eval_stage, s_diag_prev_eval_stage,
                    s_diag_ldr_updates, s_diag_composite_path, s_diag_hud_src,
                    s_diag_hud_mask_w,
                    (s_has_clean_cb1_data && s_clean_camera_frame == rendered_frame) ? 1 : 0,
                    s_clean_camera_substitutions.load(std::memory_order_relaxed),
                    ContextCensusIsJitterActive() ? 1 : 0);
                EdpeLog(dmsg);
                if (anomaly) {
                    for (size_t i = 0; i < s_hud_pair_stats_prev_count; ++i) {
                        const auto& prev = s_hud_pair_stats_prev[i];
                        const HudPairFrameStat* cur = nullptr;
                        for (size_t j = 0; j < s_hud_pair_stats_cur_count; ++j) {
                            if (s_hud_pair_stats_cur[j].vs == prev.vs && s_hud_pair_stats_cur[j].ps == prev.ps) {
                                cur = &s_hud_pair_stats_cur[j];
                                break;
                            }
                        }
                        const unsigned cur_rep = cur ? cur->replayed : 0;
                        const unsigned cur_cand = cur ? cur->candidates : 0;
                        const unsigned cur_sub = cur ? cur->cb1_substituted : 0;
                        const unsigned cur_decl = cur ? cur->declined : 0;
                        const unsigned cur_sten = cur ? cur->stencil_only : 0;
                        if (cur_rep != prev.replayed || cur_decl > 0 || cur_sub != prev.cb1_substituted) {
                            wchar_t pmsg[320];
                            swprintf_s(pmsg,
                                L"EDPE: HUD BLINK DIAG frame=%llu VS=%016llX PS=%016llX replayed=%u->%u cand=%u->%u cb1Sub=%u->%u stencil=%u declined=%u",
                                rendered_frame, prev.vs, prev.ps,
                                prev.replayed, cur_rep, prev.candidates, cur_cand,
                                prev.cb1_substituted, cur_sub, cur_sten, cur_decl);
                            EdpeLog(pmsg);
                        }
                    }
                    for (size_t j = 0; j < s_hud_pair_stats_cur_count; ++j) {
                        const auto& cur = s_hud_pair_stats_cur[j];
                        bool existed = false;
                        for (size_t i = 0; i < s_hud_pair_stats_prev_count; ++i) {
                            if (s_hud_pair_stats_prev[i].vs == cur.vs && s_hud_pair_stats_prev[i].ps == cur.ps) {
                                existed = true;
                                break;
                            }
                        }
                        if (!existed) {
                            wchar_t pmsg[320];
                            swprintf_s(pmsg,
                                L"EDPE: HUD BLINK DIAG frame=%llu VS=%016llX PS=%016llX NEW pair replayed=0->%u cand=%u cb1Sub=%u stencil=%u declined=%u",
                                rendered_frame, cur.vs, cur.ps,
                                cur.replayed, cur.candidates, cur.cb1_substituted, cur.stencil_only, cur.declined);
                            EdpeLog(pmsg);
                        }
                    }
                }
            }
            s_diag_prev_replayed = s_diag_hud_replayed;
            s_diag_prev_eval_stage = s_diag_eval_stage;
            s_hud_pair_stats_prev = s_hud_pair_stats_cur;
            s_hud_pair_stats_prev_count = s_hud_pair_stats_cur_count;
        }
        s_hud_pair_stats_cur_count = 0;
        s_diag_hud_candidates = 0;
        s_diag_hud_replayed = 0;
        s_diag_hud_unhook_events = 0;
        s_diag_hud_declined_draws = 0;
        s_diag_eval_stage = 0;
        s_diag_ldr_updates = 0;
        s_diag_composite_path = 0;
        s_diag_hud_src = 0;
        s_diag_hud_mask_w = 0;
        s_upscale_evaluated_this_frame = false;
        motion_color_first_exit_logged = false;
        s_has_staged_camera = false;
        s_staged_from_in_scene = false;
        s_hdr_slot0_seen_frame = ~0ull;
        s_pass53_blit_seen_frame = ~0ull;
        s_jitter_injected_frame = ~0ull;
        s_hud_mask_cleared_for_frame = false;
        s_hud_mask_active_pass = false;
        hud_first_match_seen = false;
        s_hud_draws_replayed_frame = 0;
    s_clean_camera_substitutions.store(0, std::memory_order_relaxed);
    s_jittered_cb1_buffer.store(nullptr, std::memory_order_relaxed);
        clearTrackedMappedBuffers();
        clearRejectedResourceCache();
        static uint64_t s_watchdog_last_camera_seen = 0;
        static unsigned s_watchdog_no_camera_frames = 0;
        const uint64_t cur_camera_seen = s_valid_camera_seen_counter.load(std::memory_order_relaxed);
        if (edpe::UpscalerManager::instance().isReconstructionActive()) {
            if (cur_camera_seen == s_watchdog_last_camera_seen) {
                if (scene_map_target.load(std::memory_order_relaxed) != nullptr) {
                    if (++s_watchdog_no_camera_frames >= 120) {
                        scene_map_target.store(nullptr, std::memory_order_release);
                        clearRejectedResourceCache();
                        s_watchdog_no_camera_frames = 0;
                    }
                }
            } else {
                s_watchdog_last_camera_seen = cur_camera_seen;
                s_watchdog_no_camera_frames = 0;
            }
        } else {
            s_watchdog_last_camera_seen = cur_camera_seen;
            s_watchdog_no_camera_frames = 0;
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

        if (active_render_width && active_render_height && jitter_frame_index.load(std::memory_order_relaxed) == 0) {
            std::lock_guard lock(jitter_mutex);
            const unsigned base_phases = s_jitter_phase_count.load(std::memory_order_relaxed);
            unsigned phases = base_phases;
            if (outW > active_render_width && active_render_width > 0) {
                const float upscaleRatio = static_cast<float>(outW) / static_cast<float>(active_render_width);
                phases = static_cast<unsigned>(std::clamp(
                    std::lround(static_cast<float>(base_phases) * upscaleRatio * upscaleRatio), 8l, 256l));
            }
            s_active_jitter_phases.store(phases ? phases : 32, std::memory_order_relaxed);
            auto jitter = edpe::projectionJitter(0, active_render_width, active_render_height, phases);
            const float scale = s_jitter_scale.load(std::memory_order_relaxed);
            jitter.pixelX *= scale;
            jitter.pixelY *= scale;
            jitter.ndcX *= scale;
            jitter.ndcY *= scale;
            active_projection_jitter = jitter;
        }

        if (edpe::JitterLogger::instance().shouldSampleFrame(frame, false) &&
            !edpe::UpscalerManager::instance().isReconstructionActive() &&
            !s_jitter_debug_view.load(std::memory_order_relaxed) &&
            !s_hud_debug_view.load(std::memory_order_relaxed)) {
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
            const unsigned long long scene_seq = jitter_frame_index.load(std::memory_order_relaxed);
            edpe::JitterFrameLog jlog{};
            jlog.frame = frame;
            jlog.timeMs = getJitterLogTimeMs();
            jlog.deltaMs = dt_ms;
            jlog.phase = static_cast<unsigned>(scene_seq % (base_phases ? base_phases : 32));
            jlog.totalPhases = base_phases ? base_phases : 32;
            jlog.pixelX = s_injected_jitter.pixelX;
            jlog.pixelY = s_injected_jitter.pixelY;
            jlog.ndcX = s_injected_jitter.ndcX;
            jlog.ndcY = s_injected_jitter.ndcY;
            jlog.injectedWrites = writes_this_frame;
            jlog.rejections = jitter_rejection_count.load(std::memory_order_relaxed);
            jlog.evalJx = 0.0f;
            jlog.evalJy = 0.0f;
            jlog.syncOk = (s_jitter_injected_frame == frame && writes_this_frame > 0);
            jlog.cameraValid = (s_camera_now.scaleX > 0.0f);
            jlog.heldJitter = (writes_this_frame == 0);
            jlog.sceneCut = false;
            edpe::JitterLogger::instance().pushFrame(jlog);
        }
    }
    if ((edpe::Config::instance().devMode || !edpe::UpscalerManager::instance().isReconstructionActive()) &&
        frame % 1024 == 0 && observed_swap_chain == swap_chain) {
        wchar_t callMsg[256];
        swprintf_s(callMsg,
            L"EDPE: context calls frame=%llu OMSetRenderTargets=%llu OMSetRenderTargetsAndUnorderedAccessViews=%llu FinishCommandList=%llu ExecuteCommandList=%llu",
            frame,
            s_count_om_set_render_targets.load(std::memory_order_relaxed),
            s_count_om_set_rt_uav.load(std::memory_order_relaxed),
            s_count_finish_command_list.load(std::memory_order_relaxed),
            s_count_execute_command_list.load(std::memory_order_relaxed));
        EdpeLog(callMsg);

        const size_t ctxCount = s_tracked_context_count.load(std::memory_order_acquire);
        for (size_t i = 0; i < ctxCount; ++i) {
            auto* data = s_tracked_contexts[i].get();
            if (!data) continue;
            const unsigned long long omCalls = data->omCalls.load(std::memory_order_relaxed);
            const unsigned long long gameOm = data->gameOmCalls.load(std::memory_order_relaxed);
            const unsigned long long ownOm = data->edpeOmCalls.load(std::memory_order_relaxed);
            const unsigned long long uavCalls = data->omUavCalls.load(std::memory_order_relaxed);
            const unsigned long long gameUav = data->gameOmUavCalls.load(std::memory_order_relaxed);
            const unsigned long long ownUav = data->edpeOmUavCalls.load(std::memory_order_relaxed);
            const unsigned long long dsvCalls = data->nonNullDsvCalls.load(std::memory_order_relaxed);
            const unsigned long long gameDsv = data->gameNonNullDsvCalls.load(std::memory_order_relaxed);
            const unsigned long long ownDsv = data->edpeNonNullDsvCalls.load(std::memory_order_relaxed);
            const UINT maxW = data->maxDsvWidth.load(std::memory_order_relaxed);
            const UINT maxH = data->maxDsvHeight.load(std::memory_order_relaxed);

            wchar_t ctxMsg[512];
            swprintf_s(ctxMsg,
                L"EDPE: context census ctx=%p device=%p OMSetRenderTargets=%llu (game=%llu own=%llu) OMSetRenderTargetsAndUnorderedAccessViews=%llu (game=%llu own=%llu) omCalls=%llu dsvCalls=%llu (game=%llu own=%llu) maxDsv=%ux%u",
                data->context, data->owningDevice,
                omCalls, gameOm, ownOm,
                uavCalls, gameUav, ownUav,
                omCalls + uavCalls,
                dsvCalls, gameDsv, ownDsv,
                maxW, maxH);
            EdpeLog(ctxMsg);
        }
        findSceneContextData(true);
    }
    if (frame % 1024 == 0 && observed_swap_chain == swap_chain &&
        !edpe::UpscalerManager::instance().isReconstructionActive()) {
        const size_t ctxCount = s_tracked_context_count.load(std::memory_order_acquire);
        const unsigned slotActive = patched_table && patched_table[kOMSetRenderTargets] ==
            reinterpret_cast<void*>(&observedOMSetRenderTargets);
        for (size_t c = 0; c < ctxCount; ++c) {
            auto* data = s_tracked_contexts[c].get();
            if (!data) continue;
            size_t distinct = 0;
            std::array<unsigned long long, 32> counts{};
            {
                std::lock_guard lock(seen_mutex);
                distinct = data->seen_count;
                for (size_t i = 0; i < distinct; ++i) {
                    counts[i] = data->seen[i].interval_binds;
                    data->seen[i].interval_binds = 0;
                }
            }
            const unsigned long long ctxBinds = data->dsv_binds.load(std::memory_order_relaxed);
            if (distinct == 0 && ctxBinds == 0 &&
                data->context != observed_context.load(std::memory_order_acquire))
                continue;
            wchar_t message[192];
            swprintf_s(message, L"EDPE: DSV census frame=%llu binds=%llu unique=%zu slotActive=%u ctx=%p",
                frame, ctxBinds, distinct, slotActive, data->context);
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
            swprintf_s(message, L"EDPE: DSV interval frame=%llu top=%d:%llu,%d:%llu,%d:%llu,%d:%llu ctx=%p",
                frame, busiest[0], binds[0], busiest[1], binds[1],
                busiest[2], binds[2], busiest[3], binds[3], data->context);
            EdpeLog(message);
        }
    }
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain) {
        edpe::Profiler::instance().onFrameEnd();
    }
}

void ContextCensusAfterOverlay(IDXGISwapChain* swap_chain, UINT flags) {
    if ((flags & DXGI_PRESENT_TEST) || !patched_table || observed_swap_chain != swap_chain) return;
    const bool arm_sequence = sequence_requested.exchange(false, std::memory_order_acq_rel);
    int armed_pair = -1;
    {
        std::lock_guard lock(seen_mutex);
        if (arm_sequence) {
            const size_t ctxCount = s_tracked_context_count.load(std::memory_order_acquire);
            for (size_t i = 0; i < ctxCount; ++i) {
                if (auto* data = s_tracked_contexts[i].get()) {
                    data->sequence_stored = 0;
                    data->sequence_transitions = 0;
                    data->sequence_last = -2;
                }
            }
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
            camera_pair_sample_frames = {~0ull, ~0ull};
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
bool depthSnapshotAvailableForContextLocked(const PerContextData* ctxData, unsigned index) {
    if (!patched_table || !ctxData || index >= ctxData->seen_count || snapshot_queued >= 0 ||
        snapshot_active >= 0 || snapshot_view || camera_pair_queued >= 0 ||
        camera_pair_target >= 0 || camera_pair_depth[0].staging ||
        camera_pair_depth[1].staging || motion_pair_active ||
        motion_grid_readback) return false;
    const auto& candidate = ctxData->seen[index];
    const UINT scW = s_swapchain_width.load(std::memory_order_relaxed);
    const UINT scH = s_swapchain_height.load(std::memory_order_relaxed);
    if (scW > scH && candidate.width == candidate.height) {
        return false;
    }
    return candidate.samples == 1 &&
        (candidate.texture_format == DXGI_FORMAT_R32G8X24_TYPELESS ||
         candidate.texture_format == DXGI_FORMAT_R24G8_TYPELESS);
}
}

bool ContextCensusRequestDepthSnapshot(unsigned index) {
    PerContextData* sceneCtx = findSceneContextData(false);
    std::lock_guard lock(seen_mutex);
    if (!depthSnapshotAvailableForContextLocked(sceneCtx, index)) return false;
    snapshot_target_context = sceneCtx->context;
    snapshot_queued = static_cast<int>(index);
    return true;
}

bool ContextCensusDepthSnapshotAvailable(unsigned index) {
    PerContextData* sceneCtx = findSceneContextData(false);
    std::lock_guard lock(seen_mutex);
    return depthSnapshotAvailableForContextLocked(sceneCtx, index);
}

bool ContextCensusRequestCameraPair(unsigned index) {
    PerContextData* sceneCtx = findSceneContextData(false);
    std::lock_guard lock(seen_mutex);
    if (!depthSnapshotAvailableForContextLocked(sceneCtx, index)) return false;
    camera_pair_context = sceneCtx->context;
    camera_pair_queued = static_cast<int>(index);
    return true;
}

bool ContextCensusRequestMotionPair(unsigned index) {
    PerContextData* sceneCtx = findSceneContextData(false);
    std::lock_guard lock(seen_mutex);
    if (!depthSnapshotAvailableForContextLocked(sceneCtx, index) ||
        sceneCtx->seen[index].texture_format != DXGI_FORMAT_R32G8X24_TYPELESS) return false;
    camera_pair_context = sceneCtx->context;
    camera_pair_queued = static_cast<int>(index);
    motion_pair_queued = true;
    s_hud_mask_snapshot.Reset();
    s_hud_mask_snapshot_queued = true;
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

ID3D11ShaderResourceView* ContextCensusTakeHudMaskSnapshot(UINT* width, UINT* height) {
    if (s_hud_mask_snapshot_queued && s_hud_mask_texture && s_hud_mask_device) {
        s_hud_mask_snapshot.Reset();
        s_hud_mask_snapshot_queued = false;
        D3D11_TEXTURE2D_DESC desc{};
        s_hud_mask_texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
        if (SUCCEEDED(s_hud_mask_device->CreateTexture2D(&desc, nullptr, &copy)) && copy) {
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
            s_hud_mask_device->GetImmediateContext(&ctx);
            if (ctx) {
                ctx->CopyResource(copy.Get(), s_hud_mask_texture.Get());
                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Format = desc.Format;
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MostDetailedMip = 0;
                srvDesc.Texture2D.MipLevels = 1;
                s_hud_mask_device->CreateShaderResourceView(copy.Get(), &srvDesc, &s_hud_mask_snapshot);
                s_hud_mask_snapshot_width = desc.Width;
                s_hud_mask_snapshot_height = desc.Height;
            }
        }
    }
    if (!s_hud_mask_snapshot) return nullptr;
    if (width) *width = s_hud_mask_snapshot_width;
    if (height) *height = s_hud_mask_snapshot_height;
    return s_hud_mask_snapshot.Detach();
}

int ContextCensusSceneDepthCandidate() {
    PerContextData* sceneCtx = findSceneContextData(true);
    std::lock_guard lock(seen_mutex);
    if (!sceneCtx) return -1;
    const auto frame = last_present_frame.load(std::memory_order_relaxed);

    int bestCandidate = -1;

    for (size_t index = 0; index < sceneCtx->seen_count; ++index) {
        const auto match = sceneCtx->seen[index].scene_match_frame;
        // Present can run between scene signature binds; keep a recent match stable.
        if (match == ~0ull || match > frame || frame - match > 32 ||
            !depthSnapshotAvailableForContextLocked(sceneCtx, static_cast<unsigned>(index))) continue;

        if (bestCandidate < 0) {
            bestCandidate = static_cast<int>(index);
            continue;
        }

        const auto& cand = sceneCtx->seen[index];
        const auto& best = sceneCtx->seen[bestCandidate];
        const int candPrio = (cand.texture_format == DXGI_FORMAT_R32G8X24_TYPELESS) ? 1 : 0;
        const int bestPrio = (best.texture_format == DXGI_FORMAT_R32G8X24_TYPELESS) ? 1 : 0;

        if (candPrio > bestPrio) {
            bestCandidate = static_cast<int>(index);
        } else if (candPrio == bestPrio) {
            if (cand.scene_match_frame > best.scene_match_frame) {
                bestCandidate = static_cast<int>(index);
            } else if (cand.scene_match_frame == best.scene_match_frame) {
                if (index > static_cast<size_t>(bestCandidate)) {
                    bestCandidate = static_cast<int>(index);
                }
            }
        }
    }
    return bestCandidate;
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

void ContextCensusOnResize(IDXGISwapChain* swap_chain) {
    if (swap_chain != observed_swap_chain) return;
    std::lock_guard<std::recursive_mutex> sceneLock(g_scene_ctx_mutex);
    std::lock_guard lock(seen_mutex);
    s_backbuffer_rtv.Reset();
    s_cached_backbuffer_ptr = nullptr;
    s_debug_backbuffer_rtv.Reset();
    s_debug_cached_backbuffer = nullptr;
    s_hud_debug_backbuffer_rtv.Reset();
    s_hud_debug_cached_backbuffer = nullptr;
    s_swapchain_backbuffer.store(nullptr, std::memory_order_relaxed);
    resetTemporalPathDebounce();
    resetSharedBackbufferBridge();
    s_last_final_output_present.Reset();
    s_last_reconstructed_output.Reset();
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
        s_motion_prev_depth.Reset();
        s_motion_prev_depth_view.Reset();
        s_has_motion_prev_depth = false;
        s_cached_hdr_rtv0 = nullptr;
        scene_depth_source.Reset();
        scene_ldr_source.Reset();
        scene_ldr_frame = ~0ull;
        s_recent_scene_ldr_source.Reset();
        s_recent_scene_ldr_frame = ~0ull;
        resetTemporalPathDebounce();
        scene_hdr_color_source.Reset();
        scene_hdr_color_frame = ~0ull;
        s_clean_hdr_scene_texture.Reset();
        s_clean_hdr_scene_srv.Reset();
        s_clean_hdr_scene_rtv.Reset();
        s_clean_hdr_scene_width = s_clean_hdr_scene_height = 0;
        s_clean_hdr_scene_frame = ~0ull;
        s_dlaa_input.Reset();
        s_dlaa_input_srv.Reset();
        s_direct_color_srv.Reset();
        s_direct_color_tex = nullptr;
        s_direct_color_fmt = DXGI_FORMAT_UNKNOWN;
        s_dlss_output_texture.Reset();
        s_dlss_output_srv.Reset();
        s_backbuffer_copy.Reset();
        s_backbuffer_copy_srv.Reset();
        s_backbuffer_copy_rtv.Reset();
        s_comp_eval_tex.Reset();
        s_comp_eval_srv.Reset();
        s_comp_eval_rtv.Reset();
        s_comp_final_tex.Reset();
        s_comp_final_rtv.Reset();
        s_backbuffer_rtv.Reset();
        s_cached_backbuffer_ptr = nullptr;
        s_debug_backbuffer_rtv.Reset();
        s_debug_cached_backbuffer = nullptr;
        s_hud_debug_backbuffer_rtv.Reset();
        s_hud_debug_cached_backbuffer = nullptr;
        s_first_hud_draw_logged = false;
        resetHudMaskResources(nullptr);
        ui_composite_pass.reset();
        resetSharedBackbufferBridge();
        s_last_final_output_present.Reset();
        s_last_reconstructed_output.Reset();
        s_temporal_resources_device = nullptr;
        s_last_reconstructed_frame = ~0ull;
        s_last_temporal_inputs = {};
        s_last_temporal_inputs_frame = ~0ull;
        s_hdr_slot0_seen_frame = ~0ull;
        s_consecutive_skipped_frames = 0;
        s_has_staged_camera = false;
        s_staged_from_in_scene = false;
        s_staged_camera_frame = ~0ull;
        s_last_valid_velocity = {0.0f, 0.0f, 0.0f};
        s_prev_valid_velocity = {0.0f, 0.0f, 0.0f};
        s_teleport_rejection_streak = 0;
        s_cut_rejection_streak = 0;
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
        motion_grid_context = nullptr;
        motion_snapshot.Reset();
        s_hud_mask_snapshot.Reset();
        motion_pass.reset();
        motion_camera_frames = {~0ull, ~0ull};
        camera_pair_sample_frames = {~0ull, ~0ull};
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
        sample.context = nullptr;
        sample.ready.store(false, std::memory_order_release);
        sample.claimed.store(false, std::memory_order_release);
    }
    {
        std::lock_guard lock(seen_mutex);
        for (auto& sample : pipeline_samples) {
            auto* end_ctx = sample.context ? sample.context : observed_context.load(std::memory_order_acquire);
            if (sample.active && end_ctx) end_ctx->End(sample.query);
            if (sample.query) sample.query->Release();
            sample = {};
        }
    }
    endVSBufferProbe();
    resetCameraDelta();
    endHudDrawProbe();
    hud_draw_hook_mask = 0;
    hud_draw_probe_active.store(false, std::memory_order_release);
    for (size_t i = 0; i < s_cloned_context_count.load(std::memory_order_acquire); ++i) {
        if (s_cloned_contexts[i] && s_cloned_contexts[i]->context && s_cloned_contexts[i]->original_table) {
            *reinterpret_cast<void***>(s_cloned_contexts[i]->context) = s_cloned_contexts[i]->original_table;
        }
    }
    for (size_t i = 0; i < s_hooked_table_count.load(std::memory_order_acquire); ++i) {
        auto& entry = s_hooked_tables[i];
        if (entry.table) {
            if (entry.orig_om) {
                patchSlot(entry.table, kOMSetRenderTargets,
                    reinterpret_cast<void*>(&observedOMSetRenderTargets),
                    reinterpret_cast<void*>(entry.orig_om));
            }
            if (entry.orig_om_uav) {
                patchSlot(entry.table, kOMSetRenderTargetsAndUnorderedAccessViews,
                    reinterpret_cast<void*>(&observedOMSetRenderTargetsAndUnorderedAccessViews),
                    reinterpret_cast<void*>(entry.orig_om_uav));
            }
            if (entry.orig_exec) {
                patchSlot(entry.table, kExecuteCommandList,
                    reinterpret_cast<void*>(&observedExecuteCommandList),
                    reinterpret_cast<void*>(entry.orig_exec));
            }
            if (entry.orig_finish) {
                patchSlot(entry.table, kFinishCommandList,
                    reinterpret_cast<void*>(&observedFinishCommandList),
                    reinterpret_cast<void*>(entry.orig_finish));
            }
            if (entry.orig_map) {
                patchSlot(entry.table, kMap,
                    reinterpret_cast<void*>(&observedMap),
                    reinterpret_cast<void*>(entry.orig_map));
            }
            if (entry.orig_unmap) {
                patchSlot(entry.table, kUnmap,
                    reinterpret_cast<void*>(&observedUnmap),
                    reinterpret_cast<void*>(entry.orig_unmap));
            }
        }
    }
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
    }
    for (size_t i = 0; i < s_hooked_table_count.load(std::memory_order_acquire); ++i) {
        if (s_context_vtable_hooks[i] && s_context_vtable_hooks[i]->isInitialized()) {
            s_context_vtable_hooks[i]->unhookAll();
        }
        s_hooked_tables[i] = {};
    }
    s_hooked_table_count.store(0, std::memory_order_release);
    scene_map_target.store(nullptr, std::memory_order_release);
    scene_mapped_data.store(nullptr, std::memory_order_release);
    s_hud_probe_armed_once.store(false, std::memory_order_release);
    clearTrackedMappedBuffers();
    if (auto* prevCtx = observed_context.exchange(nullptr, std::memory_order_acq_rel)) {
        prevCtx->Release();
    }
    patched_table = nullptr;
    observed_swap_chain = nullptr;
    attempted = false;
    s_scene_device.store(nullptr, std::memory_order_release);
    s_present_device.store(nullptr, std::memory_order_release);
    last_present_frame.store(0, std::memory_order_relaxed);
    ResetPrimaryD3D11Device();
}

void ContextCensusSetJitterEnabled(bool enabled) {
    jitter_enabled.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        s_jitter_standdown_until.store(0, std::memory_order_relaxed);
        s_jitter_standdown_len = 60;
    }
    auto& cfg = edpe::Config::instance();
    cfg.jitterEnabled = enabled;
    cfg.save();
}

bool ContextCensusGetJitterEnabled() {
    return jitter_enabled.load(std::memory_order_relaxed);
}

void ContextCensusSetJitterTestOverride(bool override) {
    if (override) {
        clearRejectedResourceCache();
    }
    jitter_test_override.store(override, std::memory_order_relaxed);
}

bool ContextCensusGetJitterTestOverride() {
    return jitter_test_override.load(std::memory_order_relaxed);
}

bool ContextCensusIsJitterActive() {
    if (!jitter_enabled.load(std::memory_order_relaxed)) return false;
    if (jitter_test_override.load(std::memory_order_relaxed)) return true;
    if (s_jitter_debug_view.load(std::memory_order_relaxed)) return true;
    if (last_present_frame.load(std::memory_order_relaxed) < s_jitter_standdown_until.load(std::memory_order_relaxed)) return false;
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

void ContextCensusSetSharpness(float sharpness) {
    sharpness = std::clamp(sharpness, 0.0f, 1.0f);
    s_sharpness.store(sharpness, std::memory_order_relaxed);
    edpe::Config::instance().sharpness = sharpness;
}

float ContextCensusGetSharpness() {
    if (!s_config_loaded) {
        return edpe::Config::instance().sharpness;
    }
    return s_sharpness.load(std::memory_order_relaxed);
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

const wchar_t* ContextCensusComparisonProgress() {
    return s_cmp_progress;
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

void ContextCensusSetHudDebugView(bool enable) {
    s_hud_debug_view.store(enable, std::memory_order_relaxed);
    edpe::Config::instance().hudDebugView = enable;
}

bool ContextCensusGetHudDebugView() {
    return s_hud_debug_view.load(std::memory_order_relaxed);
}

void ContextCensusToggleHudDebugView() {
    const bool current = s_hud_debug_view.load(std::memory_order_relaxed);
    ContextCensusSetHudDebugView(!current);
}

void ContextCensusSetHudDebugOpacity(float opacity) {
    s_hud_debug_opacity.store(opacity, std::memory_order_relaxed);
    edpe::Config::instance().hudDebugOpacity = opacity;
}

float ContextCensusGetHudDebugOpacity() {
    return s_hud_debug_opacity.load(std::memory_order_relaxed);
}

void ContextCensusSetHudDebugThreshold(float threshold) {
    s_hud_debug_threshold.store(threshold, std::memory_order_relaxed);
    edpe::Config::instance().hudDebugThreshold = threshold;
}

float ContextCensusGetHudDebugThreshold() {
    return s_hud_debug_threshold.load(std::memory_order_relaxed);
}

void ContextCensusSetHudDebugMode(int mode) {
    s_hud_debug_mode.store(mode, std::memory_order_relaxed);
    edpe::Config::instance().hudDebugMode = mode;
}

int ContextCensusGetHudDebugMode() {
    return s_hud_debug_mode.load(std::memory_order_relaxed);
}

float ContextCensusGetHudMaskCoverage() {
    return s_hud_mask_latest_coverage.load(std::memory_order_relaxed);
}

unsigned ContextCensusGetHudDrawsReplayed() {
    const unsigned subs = s_clean_camera_substitutions.load(std::memory_order_relaxed);
    return subs > 0 ? subs : s_hud_mask_latest_draws.load(std::memory_order_relaxed);
}



void ContextCensusHookContext(ID3D11DeviceContext* context) {
    if (!context) return;
    void** rawTable = *reinterpret_cast<void***>(context);
    if (!rawTable) return;

    getOrCreateContextData(context);
    const bool is_immediate = (context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE);

    std::lock_guard lock(s_hooked_tables_mutex);
    const size_t count = s_hooked_table_count.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
        if (s_hooked_tables[i].context == context &&
            s_context_vtable_hooks[i] && s_context_vtable_hooks[i]->isInitialized()) {
            s_context_vtable_hooks[i]->reclaim();
            return;
        }
    }

    if (count >= kMaxHookedTables) return;

    const edpe::HookMode mode = edpe::contextHookModeFor(context);
    auto vtHook = std::make_unique<edpe::VTableHook>();
    if (!vtHook->initialize(context, mode)) return;

    HookedTableEntry entry{};
    entry.context = context;
    entry.original_table = vtHook->originalTable();
    entry.table = vtHook->activeTable();
    entry.is_immediate = is_immediate;

    void* fwdOm = nullptr;
    vtHook->hook(context, kOMSetRenderTargets,
        reinterpret_cast<void*>(&observedOMSetRenderTargets), &fwdOm, mode);
    entry.orig_om = reinterpret_cast<OMSetRenderTargetsFn>(fwdOm);
    if (is_immediate || !original.load(std::memory_order_relaxed)) {
        original.store(entry.orig_om, std::memory_order_release);
    } else {
        original_deferred_om.store(entry.orig_om, std::memory_order_release);
    }

    void* fwdOmUav = nullptr;
    vtHook->hook(context, kOMSetRenderTargetsAndUnorderedAccessViews,
        reinterpret_cast<void*>(&observedOMSetRenderTargetsAndUnorderedAccessViews), &fwdOmUav, mode);
    entry.orig_om_uav = reinterpret_cast<OMSetRenderTargetsAndUnorderedAccessViewsFn>(fwdOmUav);
    if (is_immediate || !original_om_uav.load(std::memory_order_relaxed)) {
        original_om_uav.store(entry.orig_om_uav, std::memory_order_release);
    } else {
        original_deferred_om_uav.store(entry.orig_om_uav, std::memory_order_release);
    }

    void* fwdExec = nullptr;
    vtHook->hook(context, kExecuteCommandList,
        reinterpret_cast<void*>(&observedExecuteCommandList), &fwdExec, mode);
    entry.orig_exec = reinterpret_cast<ExecuteCommandListFn>(fwdExec);
    if (is_immediate || !original_execute_command_list.load(std::memory_order_relaxed)) {
        original_execute_command_list.store(entry.orig_exec, std::memory_order_release);
    } else {
        original_deferred_execute_command_list.store(entry.orig_exec, std::memory_order_release);
    }

    void* fwdFinish = nullptr;
    vtHook->hook(context, kFinishCommandList,
        reinterpret_cast<void*>(&observedFinishCommandList), &fwdFinish, mode);
    entry.orig_finish = reinterpret_cast<FinishCommandListFn>(fwdFinish);
    if (is_immediate || !original_finish_command_list.load(std::memory_order_relaxed)) {
        original_finish_command_list.store(entry.orig_finish, std::memory_order_release);
    } else {
        original_deferred_finish_command_list.store(entry.orig_finish, std::memory_order_release);
    }

    if (is_immediate) {
        entry.orig_map = reinterpret_cast<MapFn>(vtHook->liveStubForSlot(kMap));
        entry.orig_unmap = reinterpret_cast<UnmapFn>(vtHook->liveStubForSlot(kUnmap));
        entry.orig_vs_cb = reinterpret_cast<VSSetConstantBuffersFn>(vtHook->liveStubForSlot(kVSSetConstantBuffers));
        entry.orig_draw_indexed = reinterpret_cast<DrawIndexedFn>(vtHook->liveStubForSlot(kDrawIndexed));
        entry.orig_draw = reinterpret_cast<DrawFn>(vtHook->liveStubForSlot(kDraw));
        entry.orig_draw_indexed_instanced = reinterpret_cast<DrawIndexedInstancedFn>(vtHook->liveStubForSlot(kDrawIndexedInstanced));
        entry.orig_draw_instanced = reinterpret_cast<DrawInstancedFn>(vtHook->liveStubForSlot(kDrawInstanced));
        if (!original_map.load(std::memory_order_relaxed)) {
            original_map.store(entry.orig_map, std::memory_order_release);
        }
        if (!original_unmap.load(std::memory_order_relaxed)) {
            original_unmap.store(entry.orig_unmap, std::memory_order_release);
        }
        if (!original_vs_set_buffers.load(std::memory_order_relaxed)) {
            original_vs_set_buffers.store(entry.orig_vs_cb, std::memory_order_release);
        }
        if (!original_draw_indexed.load(std::memory_order_relaxed)) {
            original_draw_indexed.store(entry.orig_draw_indexed, std::memory_order_release);
        }
        if (!original_draw.load(std::memory_order_relaxed)) {
            original_draw.store(entry.orig_draw, std::memory_order_release);
        }
        if (!original_draw_indexed_instanced.load(std::memory_order_relaxed)) {
            original_draw_indexed_instanced.store(entry.orig_draw_indexed_instanced, std::memory_order_release);
        }
        if (!original_draw_instanced.load(std::memory_order_relaxed)) {
            original_draw_instanced.store(entry.orig_draw_instanced, std::memory_order_release);
        }
        if (map_hook_active.load(std::memory_order_acquire)) {
            void* ignoredOut = nullptr;
            vtHook->hook(context, kMap, reinterpret_cast<void*>(&observedMap), &ignoredOut, mode);
        }
        if (unmap_hook_active.load(std::memory_order_acquire)) {
            void* ignoredOut = nullptr;
            vtHook->hook(context, kUnmap, reinterpret_cast<void*>(&observedUnmap), &ignoredOut, mode);
        }
        if (hud_draw_probe_active.load(std::memory_order_acquire)) {
            void* ignoredOut = nullptr;
            vtHook->hook(context, kVSSetConstantBuffers, reinterpret_cast<void*>(&observedVSSetConstantBuffers), &ignoredOut, mode);
            vtHook->hook(context, kDrawIndexed, reinterpret_cast<void*>(&observedDrawIndexed), &ignoredOut, mode);
            vtHook->hook(context, kDraw, reinterpret_cast<void*>(&observedDraw), &ignoredOut, mode);
            vtHook->hook(context, kDrawIndexedInstanced, reinterpret_cast<void*>(&observedDrawIndexedInstanced), &ignoredOut, mode);
            vtHook->hook(context, kDrawInstanced, reinterpret_cast<void*>(&observedDrawInstanced), &ignoredOut, mode);
        }
    }

    s_hooked_tables[count] = entry;
    s_context_vtable_hooks[count] = std::move(vtHook);
    s_hooked_table_count.store(count + 1, std::memory_order_release);
}

void ContextCensusHookDeferredContext(ID3D11DeviceContext* context) {
    if (!context) return;
    ContextCensusHookContext(context);
    void** table = *reinterpret_cast<void***>(context);
    if (!table) return;

    wchar_t msg[192];
    swprintf_s(msg, L"EDPE: deferred context vtable hooked context=%p table=%p slot33=%p slot34=%p slot114=%p",
        context, table, table[kOMSetRenderTargets], table[kOMSetRenderTargetsAndUnorderedAccessViews], table[kFinishCommandList]);
    EdpeLog(msg);
}

void ContextCensusRegisterDevice(ID3D11Device* device, ID3D11DeviceContext* immContext) {
    if (!device) return;
    std::lock_guard lock(s_device_registry_mutex);
    for (const auto& info : s_known_devices) {
        if (info.device == device) return;
    }
    s_known_devices.push_back({device, immContext});
}

bool ContextCensusIsDeviceRegistered(ID3D11Device* device) {
    if (!device) return false;
    std::lock_guard lock(s_device_registry_mutex);
    for (const auto& info : s_known_devices) {
        if (info.device == device) return true;
    }
    return false;
}

void ContextCensusLogPresentDevice(IDXGISwapChain* swapChain, ID3D11Device* presentDevice) {
    if (!presentDevice) return;
    if (!observed_swap_chain || observed_swap_chain == swapChain) {
        s_present_device.store(presentDevice, std::memory_order_release);
    }
    if (s_present_device_logs.fetch_add(1, std::memory_order_acq_rel) >= 8) return;

    std::lock_guard lock(s_device_registry_mutex);
    bool matched = false;
    for (const auto& info : s_known_devices) {
        if (info.device == presentDevice) {
            matched = true;
            break;
        }
    }
    wchar_t msg[256];
    if (matched) {
        swprintf_s(msg, L"EDPE: Present swapchain=%p device=%p matched known device (total known: %zu)",
            swapChain, presentDevice, s_known_devices.size());
    } else {
        swprintf_s(msg, L"EDPE: Present swapchain=%p device=%p not in known devices (total known: %zu)",
            swapChain, presentDevice, s_known_devices.size());
    }
    EdpeLog(msg);
}

unsigned long long ContextCensusGetDsvBinds() {
    return dsv_binds.load(std::memory_order_relaxed);
}
