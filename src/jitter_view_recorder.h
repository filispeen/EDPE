#pragma once

#include "jitter_view_report.h"

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
                 UINT width, UINT height,
                 UINT renderWidth, UINT renderHeight,
                 bool isHdr, bool reset);

private:
    JitterViewRecorder();
    ~JitterViewRecorder();

    bool ensureResources(ID3D11Device* device, ID3D11DeviceContext* context,
                         UINT width, UINT height, DXGI_FORMAT format);
    void releaseResources();
    void triggerBackgroundWrite(UINT renderWidth, UINT renderHeight,
                                UINT outputWidth, UINT outputHeight,
                                bool isHdr);
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
    uint32_t writeIndex_ = 0;
    uint32_t readIndex_ = 0;
    std::array<JitterViewFrameMeta, kRingSize> slotMeta_{};

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
