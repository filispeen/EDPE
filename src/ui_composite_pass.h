#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

namespace edpe {

class UiCompositePass {
public:
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    bool composite(ID3D11DeviceContext* context,
                   ID3D11ShaderResourceView* dlssReconstruction,
                   ID3D11ShaderResourceView* gameBackbufferWithUi,
                   ID3D11ShaderResourceView* lowResWorld,
                   ID3D11RenderTargetView* targetBackbufferRtv,
                   UINT outputWidth, UINT outputHeight,
                   UINT renderWidth, UINT renderHeight);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearSampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;
};

} // namespace edpe
