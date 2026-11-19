#pragma once

#include <cstdint>
#include <cstring>
#include <d3d11_1.h>
#include <guiddef.h>
#include <string>
#include <wrl/client.h>

struct RENDERDOC_API_1_7_0;

namespace edpe {

// Standard D3D WKPDID_D3DDebugObjectName GUID (avoids requiring dxguid in standalone tests).
inline constexpr GUID kD3DDebugObjectNameGuid = {
    0x429b8c22, 0x9188, 0x4b0c, {0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00}
};

class RenderDocBridge {
public:
    static RenderDocBridge& instance();

    /// Probes for renderdoc.dll and binds the In-Application API if loaded.
    bool refresh();

    bool isAvailable() const;
    bool areAnnotationsEnabled() const;
    void setAnnotationsEnabled(bool enabled);
    bool areAnnotationsActive() const;

    int apiMajor() const { return apiMajor_; }
    int apiMinor() const { return apiMinor_; }
    int apiPatch() const { return apiPatch_; }

    void setActiveWindow(ID3D11Device* device, void* hwnd);
    bool triggerCapture(uint32_t numFrames = 1);
    bool isFrameCapturing() const;
    bool isTargetControlConnected() const;
    uint32_t getNumCaptures() const;
    bool getCapturePath(uint32_t index, std::string& outPath, uint64_t* outTimestamp = nullptr) const;
    void setCaptureComments(const char* filePath, const char* comments);
    void setCaptureFilePathTemplate(const char* pathTemplate);
    uint32_t launchOrShowReplayUI();

    bool isOverlayVisible() const;
    void setOverlayVisible(bool visible);

    /// Attaches EDPE diagnostic comments whenever a new RenderDoc capture is recorded.
    void onFramePresented(const char* edpeVersion,
                          int upscalerBackend,
                          int upscaleMode,
                          uint32_t renderWidth,
                          uint32_t renderHeight,
                          uint32_t outputWidth,
                          uint32_t outputHeight);

    /// Test helper: clears cached state (does not unload renderdoc.dll).
    void resetForTest();

private:
    RenderDocBridge() = default;

    RENDERDOC_API_1_7_0* api_ = nullptr;
    int apiMajor_ = 0;
    int apiMinor_ = 0;
    int apiPatch_ = 0;
    uint32_t lastAnnotatedCaptureCount_ = 0;
};

inline void SetD3D11DebugName(ID3D11DeviceChild* obj, const char* name) {
    if (!obj || !name || !name[0]) return;
    if (!RenderDocBridge::instance().areAnnotationsActive()) return;
    obj->SetPrivateData(kD3DDebugObjectNameGuid, static_cast<UINT>(std::strlen(name)), name);
}

inline void EmitRenderDocMarker(ID3D11DeviceContext* context, const wchar_t* name) {
    if (!context || !name) return;
    if (!RenderDocBridge::instance().areAnnotationsActive()) return;
    Microsoft::WRL::ComPtr<ID3DUserDefinedAnnotation> annot;
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3DUserDefinedAnnotation),
            reinterpret_cast<void**>(annot.GetAddressOf()))) && annot) {
        annot->SetMarker(name);
    }
}

class ScopedRenderDocEvent {
public:
    ScopedRenderDocEvent(ID3D11DeviceContext* context, const wchar_t* name) {
        if (!context || !name) return;
        if (!RenderDocBridge::instance().areAnnotationsActive()) return;
        Microsoft::WRL::ComPtr<ID3DUserDefinedAnnotation> annot;
        if (SUCCEEDED(context->QueryInterface(__uuidof(ID3DUserDefinedAnnotation),
                reinterpret_cast<void**>(annot.GetAddressOf()))) && annot) {
            annot->BeginEvent(name);
            annot_ = std::move(annot);
        }
    }

    ~ScopedRenderDocEvent() {
        if (annot_) {
            annot_->EndEvent();
        }
    }

    ScopedRenderDocEvent(const ScopedRenderDocEvent&) = delete;
    ScopedRenderDocEvent& operator=(const ScopedRenderDocEvent&) = delete;

private:
    Microsoft::WRL::ComPtr<ID3DUserDefinedAnnotation> annot_;
};

} // namespace edpe
