#pragma once

#include "d3d12_bridge.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <shared_mutex>
#include <vector>

namespace edpe {

// Sanitizes a game swapchain description for D3D12 Flip-model presentation (Phase 13B).
// Enforces BufferCount >= 2, DXGI_SWAP_EFFECT_FLIP_DISCARD, non-sRGB swapchain format,
// strips GDI_COMPATIBLE, and sets ALLOW_TEARING only when supported.
inline DXGI_SWAP_CHAIN_DESC1 sanitizeFlipSwapChainDesc(uint32_t width,
                                                      uint32_t height,
                                                      DXGI_FORMAT requestedFormat,
                                                      uint32_t requestedBufferCount,
                                                      UINT requestedFlags,
                                                      bool tearingSupported) {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width > 0 ? width : 1u;
    desc.Height = height > 0 ? height : 1u;
    switch (requestedFormat) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        break;
    case DXGI_FORMAT_UNKNOWN:
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    default:
        desc.Format = requestedFormat;
        break;
    }
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = (requestedBufferCount >= 2u && requestedBufferCount <= 4u)
                           ? requestedBufferCount
                           : (requestedBufferCount > 4u ? 4u : 3u);
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    UINT cleanFlags = requestedFlags & ~DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
    if (tearingSupported) {
        cleanFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    } else {
        cleanFlags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    desc.Flags = cleanFlags;
    return desc;
}

// Hidden 1x1 non-activating dummy HWND so a D3D11 swapchain can stay valid offscreen
// while the visible D3D12 Flip-model swapchain owns presentation on the game's HWND.
class HiddenDummyWindow {
public:
    HiddenDummyWindow() = default;
    ~HiddenDummyWindow();

    HiddenDummyWindow(const HiddenDummyWindow&) = delete;
    HiddenDummyWindow& operator=(const HiddenDummyWindow&) = delete;

    bool create();
    void destroy();
    HWND hwnd() const { return hwnd_; }

private:
    HWND hwnd_ = nullptr;
    ATOM classAtom_ = 0;
    HINSTANCE hInstance_ = nullptr;
};

// Isolated D3D12 Flip-model proxy swapchain presenter fed from a D3D11 texture via
// D3D11D3D12Bridge (Option A2 / Phase 13B). Foundation for Phases 14 (FSR FG) & 15 (DLSS-G).
class D3D12ProxyPresenter {
public:
    D3D12ProxyPresenter() = default;
    ~D3D12ProxyPresenter();

    D3D12ProxyPresenter(const D3D12ProxyPresenter&) = delete;
    D3D12ProxyPresenter& operator=(const D3D12ProxyPresenter&) = delete;

    // Initializes the bridge, system DXGI factory (bypassing EDPE proxy recursion),
    // and D3D12 Flip-model swapchain bound to targetHwnd.
    bool initialize(ID3D11Device* d3d11Device,
                    HWND targetHwnd,
                    uint32_t width,
                    uint32_t height,
                    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM,
                    uint32_t bufferCount = 3,
                    UINT flags = 0);

    void shutdown();

    bool isInitialized() const { return initialized_; }
    bool isTearingSupported() const { return tearingSupported_; }

    // Thread-safe resize synchronized with Present via exclusive mutex + waitGpuIdle().
    bool resizeBuffers(uint32_t bufferCount,
                       uint32_t width,
                       uint32_t height,
                       DXGI_FORMAT format,
                       UINT flags);

    bool setColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace);

    // Copies srcD3D11 -> shared NT texture, signals fence11, copies into current D3D12
    // backbuffer, and presents via the D3D12 Flip-model swapchain.
    bool presentFromD3D11Texture(ID3D11DeviceContext4* ctx4,
                                 ID3D11Texture2D* srcD3D11,
                                 UINT syncInterval,
                                 UINT flags,
                                 HRESULT* outPresentHr = nullptr,
                                 double* outGpuCopyMs = nullptr);

    // Returns true when Windows renderdoc.dll is in-process, where cross-API shared-handle
    // swapchains are unsupported by RenderDoc and EDPE should fail-open to D3D11 Present.
    static bool isCrossApiPresentBlockedByRenderDoc();

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    DXGI_FORMAT format() const { return format_; }
    uint32_t bufferCount() const { return bufferCount_; }
    HWND targetHwnd() const { return targetHwnd_; }

    D3D11D3D12Bridge& bridge() { return bridge_; }
    const D3D11D3D12Bridge& bridge() const { return bridge_; }
    IDXGIFactory4* dxgiFactory() const { return factory_.Get(); }
    IDXGISwapChain4* swapChain() const { return swapChain_.Get(); }
    ID3D12Resource* currentBackBuffer() const;

private:
    bool acquireBackBuffers();
    void releaseBackBuffers();
    bool ensureSharedInput(uint32_t width, uint32_t height, DXGI_FORMAT format);
    bool ensureTimestampQueries();

    mutable std::shared_mutex presentResizeMutex_;
    bool initialized_ = false;
    bool tearingSupported_ = false;
    HWND targetHwnd_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    uint32_t bufferCount_ = 0;
    UINT swapChainFlags_ = 0;
    DXGI_COLOR_SPACE_TYPE colorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

    D3D11D3D12Bridge bridge_;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory_;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> swapChain_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> backBuffers_;
    SharedBridgeTexture sharedInput_;

    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queryHeap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> queryReadback_;
    std::array<bool, D3D11D3D12Bridge::kAllocatorCount> slotHasTimestamp_{};
};

} // namespace edpe
