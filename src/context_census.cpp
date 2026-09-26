#include "context_census.h"
#include "elite_camera.h"
#include "log.h"
#include "motion_pass.h"

#include <DirectXPackedVector.h>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <memory>
#include <mutex>
#include <wrl/client.h>
#include <windows.h>

namespace {
constexpr size_t kVSSetConstantBuffers = 7;
constexpr size_t kOMSetRenderTargets = 33;
constexpr unsigned kPairFirstSample = 100;
constexpr unsigned kPairSecondSample = 101;
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using VSSetConstantBuffersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    UINT, ID3D11Buffer* const*);

bool patchSlot(void** table, size_t slot, void* expected, void* replacement);
void STDMETHODCALLTYPE observedOMSetRenderTargets(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* dsv);

std::atomic<OMSetRenderTargetsFn> original{nullptr};
std::atomic<VSSetConstantBuffersFn> original_vs_set_buffers{nullptr};
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
unsigned long long motion_depth_frame = ~0ull;
Microsoft::WRL::ComPtr<ID3D11Texture2D> motion_depth;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motion_depth_view;
Microsoft::WRL::ComPtr<ID3D11Texture2D> motion_center_readback;
std::unique_ptr<edpe::MotionPass> motion_pass;
unsigned motion_center_wait = 0;
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
std::array<ConstantBufferSample, 8> constant_buffer_samples{}; // Six snapshot probes and two adjacent-frame probes.
struct PipelineSample {
    ID3D11Query* query = nullptr;
    unsigned bind = 0;
    unsigned long long after_present = 0;
    unsigned wait = 0;
    bool active = false;
};
std::array<PipelineSample, 4> pipeline_samples{}; // Requested bind intervals 1, 2, 3, and 6.
std::atomic<bool> vs_buffer_probe_active{false};
std::atomic<unsigned> vs_buffer_calls{0};
std::atomic<unsigned> vs_scene_buffer_calls{0};
std::atomic<unsigned> vs_scene_buffer_switches{0};
std::atomic<ID3D11Buffer*> vs_first_scene_buffer{nullptr}; // Identity only.
std::atomic<ID3D11Buffer*> vs_scene_buffer{nullptr};

void queueConstantBufferSample(ID3D11DeviceContext* context, ID3D11Buffer* source,
    const D3D11_BUFFER_DESC& source_desc, unsigned bind_ordinal);

void STDMETHODCALLTYPE observedVSSetConstantBuffers(ID3D11DeviceContext* context,
    UINT start, UINT count, ID3D11Buffer* const* buffers) {
    const auto forward = original_vs_set_buffers.load(std::memory_order_acquire);
    forward(context, start, count, buffers);
    if (context != observed_context.load(std::memory_order_acquire) ||
        !vs_buffer_probe_active.load(std::memory_order_acquire)) return;
    vs_buffer_calls.fetch_add(1, std::memory_order_relaxed);
    if (start > 1 || count <= 1 - start || !buffers || !buffers[1 - start]) return;
    ID3D11Buffer* buffer = buffers[1 - start];
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    if (desc.ByteWidth == 5376 && desc.Usage == D3D11_USAGE_DYNAMIC) {
        const unsigned scene_bind = vs_scene_buffer_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        ID3D11Buffer* expected = nullptr;
        if (vs_first_scene_buffer.compare_exchange_strong(expected, buffer, std::memory_order_relaxed))
            queueConstantBufferSample(context, buffer, desc, 0); // 0 labels the first bind inside DSV interval 2.
        if (scene_bind == 51) queueConstantBufferSample(context, buffer, desc, 51);
        auto* previous = vs_scene_buffer.exchange(buffer, std::memory_order_relaxed);
        if (previous && previous != buffer)
            vs_scene_buffer_switches.fetch_add(1, std::memory_order_relaxed);
    }
}

void endVSBufferProbe() {
    if (!vs_buffer_probe_active.exchange(false, std::memory_order_acq_rel)) return;
    const bool restored = patched_table && patchSlot(patched_table,
        kVSSetConstantBuffers, reinterpret_cast<void*>(&observedVSSetConstantBuffers),
        reinterpret_cast<void*>(original_vs_set_buffers.load(std::memory_order_acquire)));
    wchar_t message[256];
    swprintf_s(message,
        L"EDPE: depth-pass VS bindings calls=%u sceneBufferCalls=%u first=%p last=%p switches=%u slotRestored=%u",
        vs_buffer_calls.load(std::memory_order_relaxed),
        vs_scene_buffer_calls.load(std::memory_order_relaxed),
        vs_first_scene_buffer.load(std::memory_order_relaxed),
        vs_scene_buffer.load(std::memory_order_relaxed),
        vs_scene_buffer_switches.load(std::memory_order_relaxed), restored);
    EdpeLog(message);
}

void beginVSBufferProbe() {
    if (!patched_table) return;
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
    vs_buffer_probe_active.store(true, std::memory_order_release);
    if (!patchSlot(patched_table, kVSSetConstantBuffers,
            reinterpret_cast<void*>(forward),
            reinterpret_cast<void*>(&observedVSSetConstantBuffers))) {
        vs_buffer_probe_active.store(false, std::memory_order_release);
        EdpeLog(L"EDPE: depth-pass VS bindings unavailable (slot changed)");
    }
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
    if (device) device->Release();
    if (FAILED(result) || !staging) {
        sample.claimed.store(false, std::memory_order_release);
        EdpeLog(L"EDPE: scene CB sample unavailable (staging creation failed)");
        return;
    }
    context->CopyResource(staging, source);
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
            if (sample.bind == 2) {
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
                swprintf_s(message,
                    L"EDPE: scene camera candidate afterPresent=%llu bind=%u valid=%u scale=(%.9g,%.9g) depthB=%.9g",
                    sample.after_present, sample.bind, parsed, camera.scaleX, camera.scaleY, camera.depthB);
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

void pollMotionCenter(ID3D11DeviceContext* context) {
    if (!motion_center_readback) return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT result = context->Map(motion_center_readback.Get(), 0,
        D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (result == DXGI_ERROR_WAS_STILL_DRAWING && ++motion_center_wait < 120) return;
    if (SUCCEEDED(result) && mapped.pData) {
        const auto* halves = static_cast<const uint16_t*>(mapped.pData);
        const float x = DirectX::PackedVector::XMConvertHalfToFloat(halves[0]);
        const float y = DirectX::PackedVector::XMConvertHalfToFloat(halves[1]);
        context->Unmap(motion_center_readback.Get(), 0);
        wchar_t message[192];
        swprintf_s(message,
            L"EDPE: motion candidate currentAfterPresent=%llu center=(%.6g,%.6g) pixels finite=%u",
            motion_depth_frame, x, y, std::isfinite(x) && std::isfinite(y));
        EdpeLog(message);
    } else {
        EdpeLog(L"EDPE: motion candidate center unavailable (nonblocking readback failed)");
    }
    motion_center_readback.Reset();
}

void tryMotionPair(ID3D11DeviceContext* context, unsigned long long frame) {
    if (!motion_pair_active) return;
    if (frame > camera_pair_armed_after + 120) {
        EdpeLog(L"EDPE: motion candidate unavailable (camera/depth pair timed out)");
        motion_pair_active = false;
        motion_depth.Reset();
        motion_depth_view.Reset();
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
            return;
        }
        motion_pass = std::move(candidate);
    }
    if (!motion_pass->render(motion_depth_view.Get(), motion_cameras[1],
            motion_cameras[0], desc.Width, desc.Height)) {
        EdpeLog(L"EDPE: motion candidate unavailable (GPU motion draw rejected inputs)");
        motion_pair_active = false;
        motion_depth.Reset();
        motion_depth_view.Reset();
        return;
    }
    Microsoft::WRL::ComPtr<ID3D11Resource> output;
    motion_pass->output()->GetResource(&output);
    D3D11_TEXTURE2D_DESC read_desc{};
    read_desc.Width = read_desc.Height = 1;
    read_desc.MipLevels = read_desc.ArraySize = 1;
    read_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    read_desc.SampleDesc.Count = 1;
    read_desc.Usage = D3D11_USAGE_STAGING;
    read_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&read_desc, nullptr, &motion_center_readback))) {
        EdpeLog(L"EDPE: motion candidate unavailable (center readback creation failed)");
    } else {
        const UINT x = desc.Width / 2, y = desc.Height / 2;
        const D3D11_BOX center{x, y, 0, x + 1, y + 1, 1};
        context->CopySubresourceRegion(motion_center_readback.Get(), 0, 0, 0, 0,
            output.Get(), 0, &center);
        motion_center_wait = 0;
        EdpeLog(L"EDPE: motion candidate GPU pass completed; center readback queued");
    }
    motion_pair_active = false;
    motion_depth.Reset();
    motion_depth_view.Reset();
    motion_hook_watch = 8;
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

void STDMETHODCALLTYPE observedOMSetRenderTargets(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* dsv) {
    const auto forward = original.load(std::memory_order_acquire);
    if (context == observed_context.load(std::memory_order_acquire)) {
        endVSBufferProbe();
        endPipelineSample(context);
    }
    forward(context, count, targets, dsv);
    if (context != observed_context.load(std::memory_order_acquire)) return;
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

    size_t index = 0;
    bool first_bind = false;
    bool first_color = false;
    unsigned snapshot_probe_bind = 0;
    unsigned camera_pair_sample = 0;
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
            const auto frame = last_present_frame.load(std::memory_order_relaxed);
            if (seen[index].mrt_frame != frame) {
                seen[index].mrt_hdr_view = nullptr;
                seen[index].mrt_frame = frame;
            }
            if (scene_mrt) seen[index].mrt_hdr_view = targets[3];
            else if (seen[index].mrt_hdr_view && count && targets &&
                targets[0] == seen[index].mrt_hdr_view)
                seen[index].scene_match_frame = frame;
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
                if (++camera_pair_bind_count == 3 && scene_mrt &&
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
                }
            }
        }
        if (count && targets && targets[0] && !seen[index].color_logged) {
            seen[index].color_logged = true;
            first_color = true;
        }
    }

    if (snapshot_probe_bind) {
        beginPipelineSample(context, snapshot_probe_bind);
        logSnapshotColorTarget(index, snapshot_probe_bind, count, targets);
        logBoundConstantBuffers(context, index, snapshot_probe_bind);
        if (snapshot_probe_bind == 2) beginVSBufferProbe();
    }
    if (camera_pair_sample) queueCameraPairSample(context, camera_pair_sample);

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
} // namespace

void ContextCensusOnPresent(IDXGISwapChain* swap_chain, unsigned long long frame, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain) {
        auto* context = observed_context.load(std::memory_order_acquire);
        endVSBufferProbe();
        endPipelineSample(context);
        pollPipelineSamples(context);
    }
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain)
        pollConstantBufferSamples(observed_context.load(std::memory_order_acquire));
    if (!(flags & DXGI_PRESENT_TEST) && observed_swap_chain == swap_chain) {
        auto* context = observed_context.load(std::memory_order_acquire);
        queueCameraPairDepth(context, frame);
        pollCameraPairDepth(context);
        pollMotionCenter(context);
        tryMotionPair(context, frame);
    }
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
            EdpeLog(L"EDPE: OMSetRenderTargets DSV census armed");
        } else {
            observed_context.store(nullptr, std::memory_order_release);
            context->Release();
            EdpeLog(L"EDPE: OMSetRenderTargets DSV census unavailable");
        }
    }
    if (frame % 1024 == 0 && observed_swap_chain == swap_chain) {
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
            motion_depth_frame = ~0ull;
            motion_depth.Reset();
            motion_depth_view.Reset();
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
    if ((flags & DXGI_PRESENT_TEST) || observed_swap_chain != swap_chain ||
        !motion_hook_watch || !patched_table) return;
    // A WARP and Elite run observed this slot reverting after the motion pass.
    // Restore only our known original slot; leave any other hook untouched.
    --motion_hook_watch;
    const auto forward = original.load(std::memory_order_acquire);
    auto* current = patched_table[kOMSetRenderTargets];
    if (current == reinterpret_cast<void*>(forward)) {
        const bool restored = patchSlot(patched_table, kOMSetRenderTargets,
            reinterpret_cast<void*>(forward),
            reinterpret_cast<void*>(&observedOMSetRenderTargets));
        EdpeLog(restored ? L"EDPE: DSV observer restored after context-state swap"
                         : L"EDPE: DSV observer unavailable after context-state swap");
        motion_hook_watch = 0;
    } else if (current != reinterpret_cast<void*>(&observedOMSetRenderTargets)) {
        EdpeLog(L"EDPE: DSV observer changed unexpectedly after motion pass");
        motion_hook_watch = 0;
    }
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
        motion_center_readback) return false;
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

int ContextCensusSceneDepthCandidate() {
    std::lock_guard lock(seen_mutex);
    const auto frame = last_present_frame.load(std::memory_order_relaxed);
    int candidate = -1;
    for (size_t index = 0; index < seen_count; ++index) {
        if (seen[index].scene_match_frame == ~0ull ||
            seen[index].scene_match_frame + 1 != frame ||
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
        motion_center_readback.Reset();
        motion_pass.reset();
        motion_camera_frames = {~0ull, ~0ull};
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
    if (patched_table) {
        patchSlot(patched_table, kOMSetRenderTargets,
            reinterpret_cast<void*>(&observedOMSetRenderTargets),
            reinterpret_cast<void*>(original.load(std::memory_order_acquire)));
    }
    observed_context.exchange(nullptr, std::memory_order_acq_rel)->Release();
    patched_table = nullptr;
    observed_swap_chain = nullptr;
    last_present_frame.store(0, std::memory_order_relaxed);
}
