#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace edpe {

constexpr uint32_t kLoadingMeshMagic = 0x4D504445u; // "EDPM"
constexpr uint32_t kLoadingMeshVersion = 1u;
constexpr uint32_t kMaxLoadingMeshBones = 48u;
constexpr uint32_t kLoadingScreenHoloIndexCount = 231870u;

struct LoadingMeshHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t boneCount;
    uint32_t frameCount;
    float durationSec;
    uint32_t flags;
};
static_assert(sizeof(LoadingMeshHeader) == 32, "LoadingMeshHeader size mismatch");

struct LoadingMeshVertex {
    float pos[3];
    float normal[3];
    float tangent[4];
    float uv[2];
    uint8_t bones[4];
    uint8_t weights[4];
};
static_assert(sizeof(LoadingMeshVertex) == 56, "LoadingMeshVertex size mismatch");

struct ParsedLoadingMesh {
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t boneCount = 0;
    uint32_t frameCount = 0;
    float durationSec = 0.0f;
    std::vector<LoadingMeshVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<float> boneFrames; // frameCount * boneCount * 12 floats (row-major 3x4)
};

bool parseLoadingMeshBlob(const uint8_t* data, size_t size, ParsedLoadingMesh& out,
                          std::string* err = nullptr);

void sampleLoadingMeshBones(const ParsedLoadingMesh& mesh, float timeSec,
                            float outMatrices[kMaxLoadingMeshBones * 12]);

class LoadingMeshReplacer {
public:
    using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);

    static LoadingMeshReplacer& instance();

    static const char* getVertexShaderHlsl();
    static bool compileVertexShader(ID3DBlob** outBytecode, std::string* err = nullptr);
    static const D3D11_INPUT_ELEMENT_DESC* getInputElements(UINT* outCount);

    void setEnabled(bool v) { enabled_.store(v, std::memory_order_relaxed); }
    bool isEnabled() const { return enabled_.load(std::memory_order_relaxed); }

    void setUseGameSpin(bool v) { useGameSpin_.store(v, std::memory_order_relaxed); }
    bool useGameSpin() const { return useGameSpin_.load(std::memory_order_relaxed); }

    void setReplaceAllHolograms(bool v) { replaceAllHolograms_.store(v, std::memory_order_relaxed); }
    bool replaceAllHolograms() const { return replaceAllHolograms_.load(std::memory_order_relaxed); }

    void setAnimSpeed(float s) { animSpeed_.store(s, std::memory_order_relaxed); }
    float animSpeed() const { return animSpeed_.load(std::memory_order_relaxed); }

    uint64_t replacedDrawCount() const { return replacedDrawCount_.load(std::memory_order_relaxed); }
    uint32_t lastMatchedIndexCount() const { return lastMatchedIndexCount_.load(std::memory_order_relaxed); }
    bool isAssetReady() const { return assetLoaded_.load(std::memory_order_relaxed); }

    bool tryReplaceDrawIndexedInstanced(ID3D11DeviceContext* context,
                                        UINT indexCount,
                                        UINT instanceCount,
                                        UINT startIndex,
                                        INT baseVertex,
                                        UINT startInstance,
                                        DrawIndexedInstancedFn origDraw);

    void shutdown();

private:
    LoadingMeshReplacer() = default;

    bool ensureInitialized(ID3D11Device* device);
    bool loadMeshAsset();
    float currentElapsedSeconds();

    std::mutex mutex_;
    ID3D11Device* device_ = nullptr;
    bool ready_ = false;
    bool failed_ = false;
    bool meshParsed_ = false;
    ParsedLoadingMesh mesh_;

    Microsoft::WRL::ComPtr<ID3D11Buffer> vb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> ib_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb2_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> il_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;

    int64_t qpcFreq_ = 0;
    int64_t qpcStart_ = 0;

    std::atomic<bool> enabled_{true};
    std::atomic<bool> useGameSpin_{true};
    std::atomic<bool> replaceAllHolograms_{false};
    std::atomic<float> animSpeed_{1.0f};
    std::atomic<uint64_t> replacedDrawCount_{0};
    std::atomic<uint32_t> lastMatchedIndexCount_{0};
    std::atomic<bool> assetLoaded_{false};
    std::atomic<bool> loggedMismatch_{false};
};

} // namespace edpe
