#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <d3d11_1.h>
#include <wrl/client.h>
#endif

namespace edpe {

// Content-based identity for a first-person viewmodel draw call.
// Masks out the volatile per-frame pool slot byte (0x00FF0000) from signature
// so draws match reliably across frames without RVA hooks.
struct WeaponDrawIdentity {
    uint32_t boneBase = 0;
    uint32_t signature = 0;
    uint32_t indexCount = 0;
    uint32_t instanceCount = 1;
};

inline constexpr uint32_t kWeaponSignatureMask = 0xFF00FFFFu;
inline constexpr uint32_t kWeaponStencilBit = 0x10u;
inline constexpr float kDefaultWorldNearM = 0.025f;
inline constexpr float kDefaultWeaponNearM = 0.0675f;
inline constexpr uint32_t kWeaponSoPoolBytes = 24u * 1024u * 1024u; // 24 MiB bounded pool (16-32 MiB range)

inline bool sameWeaponIdentity(const WeaponDrawIdentity& a, const WeaponDrawIdentity& b) {
    return a.boneBase == b.boneBase &&
           (a.signature & kWeaponSignatureMask) == (b.signature & kWeaponSignatureMask) &&
           a.indexCount == b.indexCount &&
           a.instanceCount == b.instanceCount;
}

// Rescales reversed-Z weapon depth from weaponNear (0.0675 m) into worldNear (0.025 m) scale.
inline float computeCanonicalWeaponDepth(float rawDepth, float worldNear = kDefaultWorldNearM,
                                         float weaponNear = kDefaultWeaponNearM) {
    if (!std::isfinite(rawDepth) || rawDepth <= 0.0f) return 0.0f;
    if (!(worldNear > 0.0f) || !(weaponNear > 0.0f)) return std::clamp(rawDepth, 0.0f, 1.0f);
    return std::clamp(rawDepth * (worldNear / weaponNear), 0.0f, 1.0f);
}

// Strips jitter phase delta from jittered viewmodel motion: mv + (jitterNow - jitterPrev).
inline void unjitterWeaponMotion(float& mvX, float& mvY,
                                 float jitterNowX, float jitterNowY,
                                 float jitterPrevX, float jitterPrevY,
                                 bool viewmodelJittered) {
    if (viewmodelJittered) {
        mvX += (jitterNowX - jitterPrevX);
        mvY += (jitterNowY - jitterPrevY);
    }
    if (!std::isfinite(mvX) || !std::isfinite(mvY)) {
        mvX = 0.0f;
        mvY = 0.0f;
    } else {
        mvX = std::clamp(mvX, -65504.0f, 65504.0f);
        mvY = std::clamp(mvY, -65504.0f, 65504.0f);
    }
}

#ifdef _WIN32
// Pure D3D11 Stream-Output first-person weapon motion & canonical depth tracker.
// Captures post-VS clip positions for stencil-0x10 depth-writing draws into a bounded
// double-buffered SO pool, matches draws across frames via WeaponDrawIdentity, and
// rasterizes WeaponMotion (R16G16B16A16_FLOAT) for MotionPass consumption.
class WeaponMotionTracker {
public:
    struct CapturedDraw {
        WeaponDrawIdentity identity{};
        uint32_t vertexOffsetBytes = 0;
        uint32_t vertexCount = 0;
    };

    bool initialize(ID3D11Device* device);
    void shutdown();

    // Gate check: true when OM depth-stencil state has StencilEnable && (ref & 0x10)
    // && DepthWriteMask == ALL.
    static bool isFirstPersonWeaponState(ID3D11DeviceContext* context);

    // Begin a new frame: swap current/previous SO pools and clear draw records.
    void beginFrame(ID3D11DeviceContext* context, UINT width, UINT height,
                    float worldNear = kDefaultWorldNearM,
                    float weaponNear = kDefaultWeaponNearM);

    // Record a captured post-VS draw into the current frame's bounded SO pool.
    // Returns false if the pool budget (24 MiB) is exhausted.
    bool recordCapturedDraw(const WeaponDrawIdentity& identity,
                            uint32_t vertexCount,
                            uint32_t* outByteOffset);

    // Captures a live stencil-0x10 DrawIndexedInstanced call into the current SO pool using a
    // lazily cached D3D11_SO_NO_RASTERIZED_STREAM Geometry Shader (leaves G-buffer untouched).
    using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
    bool captureDrawIndexedInstanced(ID3D11DeviceContext* context,
                                     UINT indexCount,
                                     UINT instanceCount,
                                     UINT startIndex,
                                     INT baseVertex,
                                     UINT startInstance,
                                     DrawIndexedInstancedFn origDraw);

    // Rasterize all matched current/previous draws into WeaponMotion (R16G16B16A16_FLOAT).
    bool resolve(ID3D11DeviceContext* context,
                 float jitterNowX, float jitterNowY,
                 float jitterPrevX, float jitterPrevY,
                 bool viewmodelJittered);

    ID3D11Buffer* currentSoBuffer() const { return soBuffers_[curSlot_].Get(); }
    ID3D11ShaderResourceView* weaponMotionSrv() const { return motionSrv_.Get(); }
    ID3D11Texture2D* weaponMotionTexture() const { return motionTex_.Get(); }
    bool hasResolvedMotion() const { return hasResolvedMotion_; }
    uint32_t matchedDrawCount() const { return matchedDraws_; }
    uint32_t capturedDrawCount() const { return static_cast<uint32_t>(draws_[curSlot_].size()); }
    bool isInitialized() const { return device_ != nullptr; }

private:
    bool ensureTargets(UINT width, UINT height);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> soBuffers_[2];
    Microsoft::WRL::ComPtr<ID3D11VertexShader> rasterVs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> rasterPs_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> motionTex_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> motionRtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> motionSrv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depthTex_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthDsv_;

    ID3D11GeometryShader* getOrCreateStreamOutGs(ID3D11VertexShader* vs, uint64_t vsHash);
    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11GeometryShader>> soGsCache_;
    std::vector<CapturedDraw> draws_[2];
    uint32_t curSlot_ = 0;
    uint32_t writeCursorBytes_ = 0;
    UINT width_ = 0;
    UINT height_ = 0;
    float worldNear_ = kDefaultWorldNearM;
    float weaponNear_ = kDefaultWeaponNearM;
    bool hasResolvedMotion_ = false;
    uint32_t matchedDraws_ = 0;
};
#endif

} // namespace edpe
