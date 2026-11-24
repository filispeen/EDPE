#pragma once

#include "temporal_math.h"

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace edpe {

enum class MotionPixelClass : uint8_t {
    Sky           = 0,
    World         = 1,
    CockpitStatic = 2,
    Disoccluded   = 3,
    Weapon        = 4,
    Celestial     = 5,
    OffscreenFG   = 6,
    Reset         = 7,
};

// GPU compute camera/depth motion pass outputting:
//   - OutMotion (R16G16_FLOAT): prev - cur in render pixels, +Y down, no jitter
//   - OutDepth  (R32_FLOAT):    canonical reversed-Z depth in world near scale
//   - OutClass  (R8_UINT):      optional diagnostic pixel class (when DEV=1)
class MotionPass {
public:
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    bool render(ID3D11ShaderResourceView* depth, const CameraProjection& now,
        const CameraProjection& previous, UINT width, UINT height,
        float jitterX = 0.0f, float jitterY = 0.0f,
        float prevJitterX = 0.0f, float prevJitterY = 0.0f,
        bool reset = false,
        ID3D11ShaderResourceView* prevDepth = nullptr,
        bool preserveContextState = true,
        bool zeroTranslation = false,
        bool enableClassOutput = false,
        ID3D11ShaderResourceView* weaponMotion = nullptr,
        ID3D11ShaderResourceView* sceneStencil = nullptr,
        ID3D11ShaderResourceView* celestialRecords = nullptr,
        uint32_t celestialCount = 0,
        bool clampCockpit = true);
    bool renderGpuCameras(ID3D11ShaderResourceView* depth, ID3D11Buffer* now,
        ID3D11Buffer* previous, UINT width, UINT height,
        float jitterX = 0.0f, float jitterY = 0.0f,
        float prevJitterX = 0.0f, float prevJitterY = 0.0f,
        bool reset = false,
        ID3D11ShaderResourceView* prevDepth = nullptr,
        bool enableClassOutput = false);

    ID3D11ShaderResourceView* output() const { return output_srv_.Get(); }
    ID3D11Texture2D* outputTexture() const { return output_texture_.Get(); }
    ID3D11ShaderResourceView* canonicalDepth() const { return depth_output_srv_.Get(); }
    ID3D11Texture2D* canonicalDepthTexture() const { return depth_output_texture_.Get(); }
    ID3D11ShaderResourceView* pixelClass() const { return class_output_srv_.Get(); }
    ID3D11Texture2D* pixelClassTexture() const { return class_output_texture_.Get(); }
    void clear(ID3D11DeviceContext* context);

private:
    bool ensureOutputs(UINT width, UINT height, bool needClassOutput);
    bool ensureCockpitTileTexture(UINT width, UINT height);
    bool renderInternal(ID3D11ShaderResourceView* depth,
        ID3D11ShaderResourceView* prevDepth,
        ID3D11ShaderResourceView* weaponMotion,
        ID3D11ShaderResourceView* sceneStencil,
        ID3D11ShaderResourceView* celestialRecords,
        const float (&values)[36],
        ID3D11Buffer* now, ID3D11Buffer* previous, UINT width, UINT height,
        bool preserveContextState, bool enableClassOutput);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> compute_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> compute_full_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> compute_gpu_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> cockpit_tile_cs_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> point_sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> cockpit_tile_tex_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> cockpit_tile_uav_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cockpit_tile_srv_;
    UINT cockpit_tile_w_ = 0;
    UINT cockpit_tile_h_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_texture_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> output_uav_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> output_srv_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_output_texture_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> depth_output_uav_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depth_output_srv_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> class_output_texture_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> class_output_uav_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> class_output_srv_;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cached_stencil_srv_[2];
    int cached_stencil_next_ = 0;

    UINT width_ = 0;
    UINT height_ = 0;
};

} // namespace edpe
