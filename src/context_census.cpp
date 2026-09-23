#include "context_census.h"
#include "log.h"

#include <array>
#include <atomic>
#include <d3d11.h>
#include <mutex>
#include <windows.h>

namespace {
constexpr size_t kOMSetRenderTargets = 33;
constexpr size_t kClearDepthStencilView = 53;
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using ClearDepthStencilViewFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
    ID3D11DepthStencilView*, UINT, FLOAT, UINT8);

std::atomic<OMSetRenderTargetsFn> original{nullptr};
std::atomic<ClearDepthStencilViewFn> original_clear{nullptr};
std::atomic<ID3D11DeviceContext*> observed_context{nullptr};
std::atomic<bool> clear_probe_requested{false};
std::atomic<bool> clear_probe_active{false};
std::atomic<bool> clear_probe_capture_enabled{false};
std::atomic<bool> clear_probe_disabled{false};
std::atomic<unsigned long long> clear_probe_calls{0};
IDXGISwapChain* observed_swap_chain = nullptr; // Weak; released by the game.
void** patched_table = nullptr;
bool attempted = false;
std::mutex seen_mutex;
struct SeenDepthView {
    ID3D11DepthStencilView* view = nullptr; // Identity only; never dereferenced later.
    bool color_logged = false;
    unsigned long long interval_binds = 0;
    unsigned clear_count = 0;
    UINT clear_flags = 0;
    FLOAT clear_min = 0;
    FLOAT clear_max = 0;
};
std::array<SeenDepthView, 32> seen{};
size_t seen_count = 0;
std::atomic<unsigned long long> dsv_binds{0};

void STDMETHODCALLTYPE observedClearDepthStencilView(ID3D11DeviceContext* context,
    ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil) {
    original_clear.load(std::memory_order_acquire)(context, dsv, flags, depth, stencil);
    if (!clear_probe_capture_enabled.load(std::memory_order_acquire) ||
        context != observed_context.load(std::memory_order_acquire)) return;
    clear_probe_calls.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(seen_mutex);
    for (size_t i = 0; i < seen_count; ++i) {
        if (seen[i].view != dsv) continue;
        if (!(flags & D3D11_CLEAR_DEPTH)) break;
        if (!seen[i].clear_count) seen[i].clear_min = seen[i].clear_max = depth;
        if (depth < seen[i].clear_min) seen[i].clear_min = depth;
        if (depth > seen[i].clear_max) seen[i].clear_max = depth;
        ++seen[i].clear_count;
        seen[i].clear_flags |= flags;
        break;
    }
}

void STDMETHODCALLTYPE observedOMSetRenderTargets(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* dsv) {
    const auto forward = original.load(std::memory_order_acquire);
    forward(context, count, targets, dsv);
    if (context != observed_context.load(std::memory_order_acquire) || !dsv) return;
    dsv_binds.fetch_add(1, std::memory_order_relaxed);

    size_t index = 0;
    bool first_bind = false;
    bool first_color = false;
    {
        std::lock_guard lock(seen_mutex);
        while (index < seen_count && seen[index].view != dsv) ++index;
        if (index == seen.size()) return;
        if (index == seen_count) {
            seen[seen_count++].view = dsv;
            first_bind = true;
        }
        ++seen[index].interval_binds;
        if (count && targets && targets[0] && !seen[index].color_logged) {
            seen[index].color_logged = true;
            first_color = true;
        }
    }

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

void finishClearProbe(unsigned long long frame) {
    clear_probe_capture_enabled.store(false, std::memory_order_release);
    const bool restored = patchSlot(patched_table, kClearDepthStencilView,
        reinterpret_cast<void*>(&observedClearDepthStencilView),
        reinterpret_cast<void*>(original_clear.load(std::memory_order_acquire)));
    clear_probe_active.store(false, std::memory_order_release);
    if (!restored) clear_probe_disabled.store(true, std::memory_order_release);
    wchar_t message[160];
    swprintf_s(message, L"EDPE: depth clear probe frame=%llu calls=%llu slotRestored=%u",
        frame, clear_probe_calls.load(std::memory_order_relaxed), restored);
    EdpeLog(message);
    struct ClearRecord { unsigned count; UINT flags; FLOAT min; FLOAT max; };
    std::array<ClearRecord, 32> records{};
    size_t distinct = 0;
    {
        std::lock_guard lock(seen_mutex);
        distinct = seen_count;
        for (size_t i = 0; i < distinct; ++i) {
            records[i] = {seen[i].clear_count, seen[i].clear_flags,
                seen[i].clear_min, seen[i].clear_max};
            seen[i].clear_count = 0;
            seen[i].clear_flags = 0;
        }
    }
    for (size_t i = 0; i < distinct; ++i) {
        if (!records[i].count) continue;
        swprintf_s(message, L"EDPE: depth clear #%zu count=%u flags=0x%X depth=%.6f..%.6f",
            i, records[i].count, records[i].flags, records[i].min, records[i].max);
        EdpeLog(message);
    }
}
} // namespace

void ContextCensusOnPresent(IDXGISwapChain* swap_chain, unsigned long long frame, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST) && clear_probe_active.load(std::memory_order_acquire) &&
        observed_swap_chain == swap_chain) finishClearProbe(frame);
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

void ContextCensusAfterPresent(IDXGISwapChain* swap_chain, UINT flags, HRESULT result) {
    if ((flags & DXGI_PRESENT_TEST) || FAILED(result) ||
        observed_swap_chain != swap_chain ||
        clear_probe_disabled.load(std::memory_order_acquire) ||
        !clear_probe_requested.exchange(false, std::memory_order_acq_rel)) return;
    auto forward = reinterpret_cast<ClearDepthStencilViewFn>(patched_table[kClearDepthStencilView]);
    if (!forward || forward == &observedClearDepthStencilView) {
        clear_probe_disabled.store(true, std::memory_order_release);
        EdpeLog(L"EDPE: depth clear probe unavailable (invalid forward target)");
        return;
    }
    original_clear.store(forward, std::memory_order_release);
    clear_probe_calls.store(0, std::memory_order_relaxed);
    clear_probe_capture_enabled.store(true, std::memory_order_release);
    if (!patchSlot(patched_table, kClearDepthStencilView, reinterpret_cast<void*>(forward),
            reinterpret_cast<void*>(&observedClearDepthStencilView))) {
        clear_probe_capture_enabled.store(false, std::memory_order_release);
        clear_probe_disabled.store(true, std::memory_order_release);
        EdpeLog(L"EDPE: depth clear probe unavailable (slot changed before install)");
        return;
    }
    clear_probe_active.store(true, std::memory_order_release);
    EdpeLog(L"EDPE: one-frame depth clear probe armed after Present");
}

bool ContextCensusDepthClearProbeAvailable() {
    if (!patched_table || !observed_context.load(std::memory_order_acquire) ||
        clear_probe_active.load(std::memory_order_acquire) ||
        clear_probe_requested.load(std::memory_order_acquire) ||
        clear_probe_disabled.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(seen_mutex);
    return seen_count != 0;
}

bool ContextCensusRequestDepthClearProbe() {
    if (!ContextCensusDepthClearProbeAvailable()) return false;
    clear_probe_requested.store(true, std::memory_order_release);
    return true;
}

const char* ContextCensusDepthClearProbeStatus() {
    if (!patched_table) return "D3D11 context not observed";
    if (clear_probe_disabled.load(std::memory_order_acquire)) return "Hook changed; probe disabled until restart";
    if (clear_probe_active.load(std::memory_order_acquire)) return "Capturing one frame";
    if (clear_probe_requested.load(std::memory_order_acquire)) return "Capture queued";
    std::lock_guard lock(seen_mutex);
    return seen_count ? "Ready" : "Waiting for depth targets";
}

extern "C" BOOL WINAPI EdpeRequestDepthClearProbe() {
    return ContextCensusRequestDepthClearProbe();
}

void ContextCensusOnSwapChainRelease(IUnknown* object) {
    if (object != observed_swap_chain) return;
    if (clear_probe_active.load(std::memory_order_acquire)) finishClearProbe(0);
    if (patched_table) {
        patchSlot(patched_table, kOMSetRenderTargets, reinterpret_cast<void*>(&observedOMSetRenderTargets),
            reinterpret_cast<void*>(original.load(std::memory_order_acquire)));
    }
    observed_context.exchange(nullptr, std::memory_order_acq_rel)->Release();
    patched_table = nullptr;
    observed_swap_chain = nullptr;
}
