#pragma once

#include <cstdint>

#ifdef _WIN32
#include <d3d11_1.h>
#include <wrl/client.h>
#endif

namespace edpe {

struct MotionClassStats {
    uint32_t counts[8]{};
    uint32_t totalSamples = 0;
    bool valid = false;
};

#ifdef _WIN32
class MotionClassDebugPass {
public:
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void shutdown();

    bool render(ID3D11DeviceContext* context,
                ID3D11ShaderResourceView* inputColorSrv,
                ID3D11ShaderResourceView* classMapSrv,
                ID3D11RenderTargetView* targetRtv,
                UINT outputWidth, UINT outputHeight,
                UINT classWidth, UINT classHeight,
                float opacity = 0.65f,
                bool isHdr = false);

    MotionClassStats lastStats() const { return lastStats_; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> histShader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearSampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> histConstants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> histBuffer_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> histUav_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> histStaging_[2];
    uint32_t histSlot_ = 0;
    bool histStagingFilled_[2]{false, false};
    MotionClassStats lastStats_{};
};
#endif

} // namespace edpe
