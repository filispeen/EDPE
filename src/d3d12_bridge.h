#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>

namespace edpe {

// Shared cross-API texture owned by D3D11 and opened on D3D12 via NT handle.
struct SharedBridgeTexture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11;
    Microsoft::WRL::ComPtr<ID3D12Resource> d3d12;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    void reset() {
        d3d12.Reset();
        d3d11.Reset();
        width = 0;
        height = 0;
        format = DXGI_FORMAT_UNKNOWN;
    }

    bool isValid() const { return d3d11 != nullptr && d3d12 != nullptr; }
};

// Isolated D3D11 <-> D3D12 interop bridge using shared NT handles and shared timeline fences.
// Keeps Elite's native D3D11 device untouched while allowing secondary D3D12 compute/FG work.
class D3D11D3D12Bridge {
public:
    static constexpr uint32_t kAllocatorCount = 4;

    D3D11D3D12Bridge() = default;
    ~D3D11D3D12Bridge();

    D3D11D3D12Bridge(const D3D11D3D12Bridge&) = delete;
    D3D11D3D12Bridge& operator=(const D3D11D3D12Bridge&) = delete;

    // Initializes secondary D3D12 device, high-priority queue, command allocators, and shared fence
    // on the exact DXGI adapter backing d3d11Device.
    // When agilitySdkDir is non-null and holds D3D12Core.dll, the device is created through
    // ID3D12DeviceFactory from that Agility SDK runtime (the inbox runtime on Windows 10 did not offer
    // FSR 4.1.2); if that fails the bridge falls back to the system D3D12 runtime.
    bool initialize(ID3D11Device* d3d11Device, const wchar_t* agilitySdkDir = nullptr);

    // True when the D3D12 device was created from a side-loaded Agility SDK runtime.
    bool usingAgilitySdk() const { return usingAgilitySdk_; }

    // Drains pending GPU work and releases all D3D12/shared resources.
    void shutdown();

    bool isInitialized() const { return initialized_; }

    // Allocates a D3D11 texture with SHARED_NTHANDLE and opens the matching ID3D12Resource.
    bool createSharedTexture(uint32_t width, uint32_t height, DXGI_FORMAT format,
                             UINT d3d11BindFlags, SharedBridgeTexture& out) const;

    // Emits a D3D12 resource state transition barrier.
    static void transitionResource(ID3D12GraphicsCommandList* cmdList,
                                   ID3D12Resource* resource,
                                   D3D12_RESOURCE_STATES before,
                                   D3D12_RESOURCE_STATES after);

    // Opens a fresh command list for the next allocator ring slot (waits only if slot is still in flight).
    ID3D12GraphicsCommandList* beginCommandList(uint32_t& outSlot, bool* outDidCpuWait = nullptr);

    // Signals D3D11 fence, submits D3D12 command list after GPU wait on that fence, and signals completion.
    uint64_t submitAfterD3D11Signal(ID3D11DeviceContext4* ctx4, uint32_t slot, bool flushD11 = true);

    // Signals the shared fence on the D3D12 command queue and returns the signaled value (0 on failure).
    uint64_t signalD3D12Queue();

    // Enqueues a pure GPU-side wait on D3D11 for a prior D3D12 submission fence value.
    bool waitOnD3D11(ID3D11DeviceContext4* ctx4, uint64_t d12DoneFence) const;

    // Blocks CPU until all previously submitted D3D12 work completes (used on resize/shutdown).
    void waitGpuIdle();

    // Queries completed D3D12 fence value without blocking.
    uint64_t completedFenceValue() const;

    // Overrides the active D3D12 command queue (e.g., with a Streamline-wrapped queue).
    // Passing nullptr restores the native queue created by initialize().
    bool setOverrideCommandQueue(ID3D12CommandQueue* overrideQueue);

    ID3D11Device* d3d11Device() const { return device11_; }
    ID3D11Device5* d3d11Device5() const { return device11_5_.Get(); }
    ID3D12Device* d3d12Device() const { return device12_.Get(); }
    ID3D12CommandQueue* commandQueue() const { return activeQueue(); }
    ID3D12Fence* d3d12Fence() const { return fence12_.Get(); }
    ID3D11Fence* d3d11Fence() const { return fence11_.Get(); }
    uint64_t timestampFrequency() const { return timestampFrequency_; }

    // Helper to dynamically serialize a D3D12 root signature without hard-linking d3d12.dll.
    static HRESULT serializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc,
                                          D3D_ROOT_SIGNATURE_VERSION version,
                                          ID3DBlob** blob,
                                          ID3DBlob** errorBlob);

private:
    ID3D12CommandQueue* activeQueue() const {
        return overrideQueue_ ? overrideQueue_.Get() : queue_.Get();
    }

    bool initialized_ = false;
    HMODULE d3d12Module_ = nullptr;
    ID3D11Device* device11_ = nullptr; // Non-owning
    Microsoft::WRL::ComPtr<ID3D11Device5> device11_5_;
    Microsoft::WRL::ComPtr<ID3D12DeviceFactory> deviceFactory_; // keeps the Agility runtime alive
    bool usingAgilitySdk_ = false;
    Microsoft::WRL::ComPtr<ID3D12Device> device12_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> overrideQueue_;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kAllocatorCount> allocators_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmdList_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence12_;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence11_;
    HANDLE fenceEvent_ = nullptr;
    uint64_t fenceValue_ = 0;
    std::array<uint64_t, kAllocatorCount> allocatorFence_{};
    uint32_t frameCounter_ = 0;
    uint64_t timestampFrequency_ = 0;
};

} // namespace edpe
