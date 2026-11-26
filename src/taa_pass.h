#pragma once

#include "temporal_math.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>

namespace edpe {

struct TaaSettings {
    float minAlpha = 0.045f;
    float maxAlpha = 0.25f;
    float baseGamma = 1.25f;
    float lockMaxGamma = 2.25f;
    float rcasSharpness = 0.30f;
    int debugView = 0; // 0=Off, 1=Disocclusion, 2=ThinFeatureLock, 3=LumaInstability, 4=SampleCount
};

class TaaPass {
public:
    static constexpr DXGI_FORMAT kHistoryFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    TaaPass() = default;
    ~TaaPass() = default;

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void shutdown();

    bool render(ID3D11DeviceContext* context,
        ID3D11ShaderResourceView* currentFrameSrv,
        ID3D11ShaderResourceView* motionVectorsSrv,
        ID3D11ShaderResourceView* depthSrv,
        ID3D11RenderTargetView* destinationRtv,
        ID3D11Texture2D* destinationTexture,
        uint32_t width,
        uint32_t height,
        float jitterX,
        float jitterY,
        bool reset,
        uint32_t renderWidth = 0,
        uint32_t renderHeight = 0,
        float prevJitterX = 0.0f,
        float prevJitterY = 0.0f,
        float depthB = 0.025f,
        ID3D11ShaderResourceView* prevDepthSrv = nullptr,
        DXGI_FORMAT preferredFormat = DXGI_FORMAT_UNKNOWN,
        ID3D11ShaderResourceView* reactiveMaskSrv = nullptr,
        ID3D11ShaderResourceView* motionClassSrv = nullptr,
        const TaaSettings* settings = nullptr,
        bool colorIsHdr = false,
        const CameraProjection* cameraNow = nullptr,
        const CameraProjection* cameraPrev = nullptr);

    void resetHistory() {
        resetRequested_ = true;
        hasPrevDepthCopy_ = false;
        hasPrevPrepass_ = false;
        framesSinceReset_ = 0;
    }
    bool isInitialized() const { return device_ != nullptr; }

    // Always returns the untouched temporal history buffer (never mutated by PostRCAS or DebugView).
    ID3D11Texture2D* latestOutputTexture() const { return historyTexture_[historyReadIdx_].Get(); }
    ID3D11ShaderResourceView* latestOutputSrv() const { return historySrv_[historyReadIdx_].Get(); }

    // Returns the display texture/SRV (DebugView or PostRCAS output when active, else latestOutput).
    ID3D11Texture2D* latestDisplayTexture() const {
        return displayTexture_ ? displayTexture_ : historyTexture_[historyReadIdx_].Get();
    }
    ID3D11ShaderResourceView* latestDisplaySrv() const {
        return displaySrv_ ? displaySrv_ : historySrv_[historyReadIdx_].Get();
    }

    DXGI_FORMAT historyFormat() const { return historyFormat_; }

private:
    bool ensureRenderResources(uint32_t renderWidth, uint32_t renderHeight);
    bool ensureOutputResources(uint32_t width, uint32_t height, DXGI_FORMAT format);
    bool ensurePostTexture(bool needPost);
    bool ensurePrevDepthTexture(uint32_t renderWidth, uint32_t renderHeight);

    ID3D11Device* device_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> prepassCs_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> patchDivCs_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> resolveCs_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> postRcasCs_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> debugCs_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> blitVs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> blitPs_;

    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearSampler_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> pointSampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> prepassCb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> patchDivCb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> resolveCb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> postRcasCb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> blitCb_;

    // Render-resolution buffers (ping-ponged prepass + half-res patch divergence)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> prepassTex_[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> prepassSrv_[2];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> prepassUav_[2];
    Microsoft::WRL::ComPtr<ID3D11Texture2D> patchDivTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> patchDivSrv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> patchDivUav_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> prevDepthTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> prevDepthSrv_;
    uint32_t prepassReadIdx_ = 0;
    bool hasPrevPrepass_ = false;
    uint32_t renderWidth_ = 0;
    uint32_t renderHeight_ = 0;
    uint32_t patchWidth_ = 0;
    uint32_t patchHeight_ = 0;
    uint32_t prevDepthWidth_ = 0;
    uint32_t prevDepthHeight_ = 0;
    bool hasPrevDepthCopy_ = false;

    // Output-resolution ping-pong history + auxiliary temporal state
    Microsoft::WRL::ComPtr<ID3D11Texture2D> historyTexture_[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> historySrv_[2];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> historyUav_[2];

    Microsoft::WRL::ComPtr<ID3D11Texture2D> historyAuxTex_[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> historyAuxSrv_[2];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> historyAuxUav_[2];

    // Non-feedback post-pass buffer (PostRCAS or DebugView in zero-copy mode)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> postTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> postSrv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> postUav_;

    ID3D11Texture2D* displayTexture_ = nullptr;
    ID3D11ShaderResourceView* displaySrv_ = nullptr;

    uint32_t historyReadIdx_ = 0;
    uint32_t historyWidth_ = 0;
    uint32_t historyHeight_ = 0;
    DXGI_FORMAT historyFormat_ = DXGI_FORMAT_UNKNOWN;

    ID3D11Texture2D* cachedDestTexture_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> cachedDestRtv_;

    bool resetRequested_ = true;
    uint32_t framesSinceReset_ = 0;
};

} // namespace edpe
