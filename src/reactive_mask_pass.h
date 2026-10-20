#pragma once

#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>

namespace edpe {

class ReactiveMaskPass {
public:
    ReactiveMaskPass() = default;
    ~ReactiveMaskPass() = default;

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void shutdown();

    bool render(ID3D11DeviceContext* context,
        ID3D11ShaderResourceView* emissiveSrv,
        ID3D11ShaderResourceView* preHudSrv,
        ID3D11ShaderResourceView* postHudSrv,
        uint32_t width, uint32_t height,
        float emissiveWeight, float hudWeight,
        float emissiveThreshold, float hudThreshold);

    void clear(ID3D11DeviceContext* context);

    ID3D11Texture2D* outputTexture() const { return outputTexture_.Get(); }
    ID3D11ShaderResourceView* outputSrv() const { return outputSrv_.Get(); }
    ID3D11RenderTargetView* outputRtv() const { return outputRtv_.Get(); }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    bool ensureOutputTexture(ID3D11Device* device, uint32_t width, uint32_t height);

    ID3D11Device* device_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> outputTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outputRtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> outputSrv_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace edpe
