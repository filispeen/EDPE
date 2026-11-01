#pragma once

#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>

namespace edpe {

class SharpenPass {
public:
    SharpenPass() = default;
    ~SharpenPass() = default;

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void shutdown();

    bool apply(ID3D11DeviceContext* context, ID3D11Texture2D* target, float sharpness, bool hdr);

    bool isInitialized() const { return device_ != nullptr; }

private:
    bool ensureResources(const D3D11_TEXTURE2D_DESC& desc, bool directTarget);

    ID3D11Device* device_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> sourceTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sourceSrv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTarget_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> stagingRtv_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    bool direct_ = false;
};

} // namespace edpe
