#pragma once

#include "jitter_view_report.h"
#include "jitter_debug_pass.h"

#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace edpe {

struct JitterViewRingSlot {
    unsigned long long renderedFrameId = 0;
    unsigned long long logFrameId = 0;
    int64_t qpcTicks = 0;
    float tMs = 0.0f;
    float deltaMs = 0.0f;
    float jx = 0.0f;
    float jy = 0.0f;
    unsigned phase = 0;
    unsigned totalPhases = 0;
    float camMove = 0.0f;
    float camRot = 0.0f;
    bool cameraValid = false;
    bool heldJitter = false;
    bool sceneCut = false;
    bool reset = false;
    bool inFlight = false;
    DXGI_FORMAT inputFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t texW = 0;
    uint32_t texH = 0;
    uint32_t inW = 0;
    uint32_t inH = 0;
    bool historyRecreated = false;
    bool performReset = false;
    bool motionNull = false;
    bool depthNull = false;
    float sensitivity = 0.0f;
    float threshold = 0.0f;
    int mode = 0;
    uint32_t sceneW = 0;
    uint32_t sceneH = 0;
    uint64_t lumaSumQ10 = 0;
    bool hasLuma = false;
};

class JitterViewRecorder {
public:
    enum class State {
        Idle,
        Recording,
        Draining,
        Writing
    };

    static JitterViewRecorder& instance();

    void startRecording();
    void shutdown();

    bool isIdle() const;
    bool isRecording() const;
    bool isDraining() const;
    bool isWriting() const;
    bool isActive() const;

    float getRecordingElapsedSeconds() const;
    size_t getLastSavedFrameCount() const;
    bool hasSavedLog() const;

    void onFrame(ID3D11DeviceContext* context,
                 ID3D11Texture2D* backbuffer,
                 unsigned long long rendered,
                 UINT width, UINT height,
                 UINT renderWidth, UINT renderHeight,
                 float jx, float jy,
                 bool isHdr, bool reset,
                 const JitterDebugPass::FrameInfo& passInfo,
                 bool motionNull, bool depthNull,
                 float sensitivity, float threshold, int mode,
                 ID3D11ShaderResourceView* inputColorSrv);

private:
    JitterViewRecorder();
    ~JitterViewRecorder();

    bool ensureResources(ID3D11Device* device, ID3D11DeviceContext* context,
                         UINT width, UINT height, DXGI_FORMAT format);
    void releaseResources();
    void triggerBackgroundWrite(UINT renderWidth, UINT renderHeight,
                                UINT outputWidth, UINT outputHeight,
                                bool isHdr, uint64_t droppedFrames);
    std::wstring getLogFilePath() const;

    mutable std::mutex mutex_;
    std::atomic<State> state_{State::Idle};
    std::atomic<float> elapsedSeconds_{0.0f};
    std::atomic<size_t> lastSavedCount_{0};
    std::atomic<bool> hasSavedLog_{false};

    bool restoreDebugView_ = false;
    LARGE_INTEGER qpcStart_{};
    LARGE_INTEGER qpcFreq_{};
    uint32_t drainingFramesLeft_ = 0;

    static constexpr size_t kRingSize = 4;
    std::array<JitterViewRingSlot, kRingSize> slots_{};
    uint32_t writeSlot_ = 0;
    uint32_t readSlot_ = 0;
    uint64_t droppedFrames_ = 0;

    std::vector<JitterViewFrameData> recordedFrames_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> stateObject_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> computeShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> gpuBuffer_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> gpuUav_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, kRingSize> stagingBuffers_{};

    Microsoft::WRL::ComPtr<ID3D11Texture2D> inputCopy_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> inputSrv_;
    UINT inputCopyWidth_ = 0;
    UINT inputCopyHeight_ = 0;
    DXGI_FORMAT inputCopyFormat_ = DXGI_FORMAT_UNKNOWN;

    std::thread writerThread_;
};

}
