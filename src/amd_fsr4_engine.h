#pragma once

#include "temporal_inputs.h"

#include <d3d11.h>
#include <memory>

namespace edpe {

// FSR 4 (ML upscaler) engine using the official signed FidelityFX DX12 DLLs through the D3D11/D3D12
// interop bridge.
//
// It is not an IUpscalerBackend of its own: AmdUpscalerBackend owns one and, while FSR 4 is selected (menu or
// [Upscaler] Fsr4=1 in edpe.ini), tries it first and falls back to the D3D11 FSR 3.1.2 path on any failure.
//
// Per frame: copy color and motion vectors into shared textures, convert the depth copy to R32_FLOAT with a small
// compute pass, signal a shared fence, run ffxDispatch on the D3D12 queue, make D3D11 wait on the GPU, and copy the
// shared output into the destination. Resources are persistent; they are recreated only when size or format
// changes.
class Fsr4Engine {
public:
    Fsr4Engine();
    ~Fsr4Engine();

    Fsr4Engine(const Fsr4Engine&) = delete;
    Fsr4Engine& operator=(const Fsr4Engine&) = delete;

    // True when [Upscaler] Fsr4=1 is set in the edpe.ini next to the game executable.
    static bool selectedByConfig();

    // True when this build includes the FSR 4 engine and the signed FidelityFX loader and upscaler DLLs
    // sit beside the proxy DLL. Cheap (two file checks); does not prove the GPU or driver offers FSR 4.
    static bool filesPresent();

    // Loads the FidelityFX DLLs and creates the D3D12 bridge on the adapter backing `device`.
    // The FFX upscaler context itself is created lazily on the first evaluate (sizes are not known yet).
    bool initialize(ID3D11Device* device);
    void shutdown();

    bool isReady() const;
    const wchar_t* lastError() const;
    const char* providerVersion() const;

    // Returns false on any failure without touching `destination` (the caller falls back).
    bool evaluate(ID3D11DeviceContext* context,
                  const TemporalFrameInputs& inputs,
                  ID3D11Texture2D* destination);

    void resetHistory();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace edpe
