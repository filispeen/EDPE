#pragma once

#include "temporal_math.h"

#include <d3d11_1.h>
#include <wrl/client.h>

namespace edpe {

// GPU-only camera/depth motion. The caller owns depth/frame association.
class MotionPass {
public:
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    bool render(ID3D11ShaderResourceView* depth, const CameraProjection& now,
        const CameraProjection& previous, UINT width, UINT height);
    bool renderGpuCameras(ID3D11ShaderResourceView* depth, ID3D11Buffer* now,
        ID3D11Buffer* previous, UINT width, UINT height);
    ID3D11ShaderResourceView* output() const { return output_srv_.Get(); }

private:
    bool renderInternal(ID3D11ShaderResourceView* depth, const float (&values)[32],
        ID3D11Buffer* now, ID3D11Buffer* previous, UINT width, UINT height);
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_gpu_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_texture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output_rtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> output_srv_;
    UINT width_ = 0;
    UINT height_ = 0;
};

} // namespace edpe
