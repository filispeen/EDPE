#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>

namespace edpe {

class JitterDebugPass {
public:
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void shutdown();

    bool render(ID3D11DeviceContext* context,
                ID3D11ShaderResourceView* inputColorSrv,
                ID3D11RenderTargetView* targetRtv,
                UINT width, UINT height,
                float jitterPixelX, float jitterPixelY,
                float sensitivity = 1.5f,
                float threshold = 0.02f,
                int mode = 0,
                ID3D11ShaderResourceView* motionSrv = nullptr);

    void resetHistory() { resetRequested_ = true; }

private:
    bool ensureHistory(uint32_t width, uint32_t height, DXGI_FORMAT format);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearSampler_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> pointSampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> historyTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> historySrv_;
    uint32_t historyWidth_ = 0;
    uint32_t historyHeight_ = 0;
    DXGI_FORMAT historyFormat_ = DXGI_FORMAT_UNKNOWN;
    bool resetRequested_ = true;
};

} // namespace edpe
