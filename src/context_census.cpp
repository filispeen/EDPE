#include "context_census.h"
#include "log.h"

#include <array>
#include <atomic>
#include <d3d11.h>
#include <mutex>
#include <windows.h>

namespace {
constexpr size_t kOMSetRenderTargets = 33;
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);

std::atomic<OMSetRenderTargetsFn> original{nullptr};
std::atomic<ID3D11DeviceContext*> observed_context{nullptr};
IDXGISwapChain* observed_swap_chain = nullptr; // Weak; released by the game.
void** patched_table = nullptr;
bool attempted = false;
std::mutex seen_mutex;
struct SeenDepthView {
    ID3D11DepthStencilView* view = nullptr; // Identity only; never dereferenced later.
    bool color_logged = false;
    unsigned long long interval_binds = 0;
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

void recordBind(int index) {
    if (index == sequence_last) return;
    sequence_last = index;
    ++sequence_transitions;
    if (sequence_stored < bind_sequence.size()) bind_sequence[sequence_stored++] = index;
}

void STDMETHODCALLTYPE observedOMSetRenderTargets(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* dsv) {
    const auto forward = original.load(std::memory_order_acquire);
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

    size_t index = 0;
    bool first_bind = false;
    bool first_color = false;
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
        if (sequence_active.load(std::memory_order_acquire)) recordBind(static_cast<int>(index));
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

bool patchSlot(void** table, void* expected, void* replacement) {
    DWORD old_protection = 0;
    if (!VirtualProtect(table + kOMSetRenderTargets, sizeof(void*), PAGE_READWRITE,
            &old_protection)) return false;
    void* replaced = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(table + kOMSetRenderTargets), replacement, expected);
    DWORD ignored = 0;
    VirtualProtect(table + kOMSetRenderTargets, sizeof(void*), old_protection, &ignored);
    return replaced == expected;
}
} // namespace

void ContextCensusOnPresent(IDXGISwapChain* swap_chain, unsigned long long frame, UINT flags) {
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
        if (patchSlot(table, reinterpret_cast<void*>(forward),
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
    if ((flags & DXGI_PRESENT_TEST) || !patched_table || observed_swap_chain != swap_chain ||
        !sequence_requested.exchange(false, std::memory_order_acq_rel)) return;
    {
        std::lock_guard lock(seen_mutex);
        sequence_stored = 0;
        sequence_transitions = 0;
        sequence_last = -2;
        sequence_active.store(true, std::memory_order_release);
    }
    EdpeLog(L"EDPE: one-frame DSV bind sequence armed after overlay");
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

extern "C" BOOL WINAPI EdpeRequestBindSequence() {
    return ContextCensusRequestBindSequence();
}

void ContextCensusOnSwapChainRelease(IUnknown* object) {
    if (object != observed_swap_chain) return;
    sequence_active.store(false, std::memory_order_release);
    sequence_requested.store(false, std::memory_order_release);
    if (patched_table) {
        patchSlot(patched_table, reinterpret_cast<void*>(&observedOMSetRenderTargets),
            reinterpret_cast<void*>(original.load(std::memory_order_acquire)));
    }
    observed_context.exchange(nullptr, std::memory_order_acq_rel)->Release();
    patched_table = nullptr;
    observed_swap_chain = nullptr;
}
