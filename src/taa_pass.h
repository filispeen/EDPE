#pragma once

#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>

namespace edpe {

class TaaPass {
public:
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
        bool reset);

    void resetHistory() { resetRequested_ = true; }
    bool isInitialized() const { return device_ != nullptr; }

private:
    bool ensureResources(uint32_t width, uint32_t height, DXGI_FORMAT format);

    ID3D11Device* device_ = nullptr;
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
