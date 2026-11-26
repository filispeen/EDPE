#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

#include <cstdint>

namespace edpe {

// Packs EDPE's 3 native-resolution UiLayer textures (hdrLayerTex_ transmittance,
// ldrLayerTex_ emission, multLayerTex_ backdrop darkening) into a single
// DXGI_FORMAT_R8G8B8A8_UNORM premultiplied-alpha texture required by both
// AMD FSR FG (RegisterUiResourceDX12) and NVIDIA DLSS-G (kBufferTypeUIColorAndAlpha).
class FgUiPackPass {
public:
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void shutdown();

    bool pack(ID3D11ShaderResourceView* hdrUiSrv,
              ID3D11ShaderResourceView* ldrUiSrv,
              ID3D11ShaderResourceView* multUiSrv,
              bool hasMultiply,
              uint32_t width,
              uint32_t height,
              ID3D11Texture2D* externalTargetTex = nullptr,
              ID3D11UnorderedAccessView* externalTargetUav = nullptr);

    ID3D11Texture2D* outputTexture() const { return outputTex_.Get(); }
    ID3D11ShaderResourceView* outputSrv() const { return outputSrv_.Get(); }
    ID3D11UnorderedAccessView* outputUav() const { return outputUav_.Get(); }

private:
    bool ensureOutput(uint32_t width, uint32_t height);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> compute_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> blackFloorCs_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> blackFloorTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> blackFloorSrv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> blackFloorUav_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> outputTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> outputSrv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outputUav_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace edpe
