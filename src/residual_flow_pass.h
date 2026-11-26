#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>

namespace edpe {

// Backend-neutral sparse coarse-to-fine residual optical flow pass.
// Takes camera+depth base motion vectors + render-res color/depth + optional
// MotionClass mask, and refines only confidence-gated moving unvectored world
// objects across all temporal upscalers (TAAv2, DLAA, DLSS, FSR, XeSS).
class ResidualFlowPass {
public:
    bool initialize(ID3D11Device* device);
    void shutdown();

    bool render(ID3D11DeviceContext* context,
                ID3D11ShaderResourceView* currColorSrv,
                ID3D11ShaderResourceView* baseMotionSrv,
                ID3D11ShaderResourceView* currDepthSrv,
                ID3D11ShaderResourceView* motionClassSrv,
                uint32_t width, uint32_t height,
                float jitterPxX, float jitterPxY,
                float prevJitterPxX, float prevJitterPxY,
                bool reset);

    ID3D11ShaderResourceView* output() const { return outMotionSrv_.Get(); }
    ID3D11Texture2D* outputTexture() const { return outMotionTex_.Get(); }
    bool isInitialized() const { return initialized_; }

private:
    bool ensureTargets(uint32_t width, uint32_t height);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> pyr0Cs_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> pyr1Cs_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> coarseCs_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> refineCs_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> applyCs_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearSampler_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> lumaQTex_[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lumaQSrv_[2];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> lumaQUav_[2];

    Microsoft::WRL::ComPtr<ID3D11Texture2D> lumaETex_[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lumaESrv_[2];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> lumaEUav_[2];

    Microsoft::WRL::ComPtr<ID3D11Texture2D> coarseTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> coarseSrv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> coarseUav_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> refineTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> refineSrv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> refineUav_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> outMotionTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> outMotionSrv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outMotionUav_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t qWidth_ = 0;
    uint32_t qHeight_ = 0;
    uint32_t eWidth_ = 0;
    uint32_t eHeight_ = 0;
    uint32_t curSlot_ = 0;
    bool hasPrev_ = false;
    bool initialized_ = false;
};

} // namespace edpe
