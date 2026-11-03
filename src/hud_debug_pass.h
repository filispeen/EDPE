#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>

namespace edpe {

class HudDebugPass {
public:
    struct FrameInfo {
        uint32_t outputW = 0;
        uint32_t outputH = 0;
        uint32_t renderW = 0;
        uint32_t renderH = 0;
        int mode = 0;
    };

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void shutdown();

    bool render(ID3D11DeviceContext* context,
                ID3D11ShaderResourceView* currentFrameSrv,
                ID3D11ShaderResourceView* hudMaskSrv,
                ID3D11ShaderResourceView* lowResWorldSrv,
                ID3D11ShaderResourceView* gameBackbufferSrv,
                ID3D11RenderTargetView* targetRtv,
                UINT outputWidth, UINT outputHeight,
                UINT renderWidth, UINT renderHeight,
                float opacity = 1.0f,
                float threshold = 0.02f,
                int mode = 0,
                bool isHdr = false);

    const FrameInfo& getLastFrameInfo() const { return lastFrameInfo_; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearSampler_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> pointSampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> dummyTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> dummySrv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> dummyMaskTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> dummyMaskSrv_;

    FrameInfo lastFrameInfo_;
};

} // namespace edpe
