#include "jitter_view_recorder.h"
#include "jitter_view_stats_shader.h"
#include "jitter_logger.h"
#include "context_census.h"
#include "upscaler_backend.h"
#include "config.h"
#include "log.h"

#include <d3dcompiler.h>
#include <ctime>
#include <cwchar>
#include <cstring>

namespace edpe {

namespace {

DXGI_FORMAT toConcreteLdrFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
        return fmt;
    }
}

struct StatsConstantBufferData {
    uint32_t width;
    uint32_t height;
    uint32_t pad[2];
};

std::string wideToUtf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) return "";
    const int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "";
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, s.data(), len, nullptr, nullptr);
    return s;
}

}

JitterViewRecorder& JitterViewRecorder::instance() {
    static JitterViewRecorder s_recorder;
    return s_recorder;
}

JitterViewRecorder::JitterViewRecorder() {
    QueryPerformanceFrequency(&qpcFreq_);
}

JitterViewRecorder::~JitterViewRecorder() {
    shutdown();
}

void JitterViewRecorder::shutdown() {
    if (writerThread_.joinable()) {
        writerThread_.join();
    }
    releaseResources();
}

bool JitterViewRecorder::isIdle() const {
    return state_.load(std::memory_order_relaxed) == State::Idle;
}

bool JitterViewRecorder::isRecording() const {
    return state_.load(std::memory_order_relaxed) == State::Recording;
}

bool JitterViewRecorder::isDraining() const {
    return state_.load(std::memory_order_relaxed) == State::Draining;
}

bool JitterViewRecorder::isWriting() const {
    return state_.load(std::memory_order_relaxed) == State::Writing;
}

bool JitterViewRecorder::isActive() const {
    const State s = state_.load(std::memory_order_relaxed);
    return s == State::Recording || s == State::Draining;
}

float JitterViewRecorder::getRecordingElapsedSeconds() const {
    return elapsedSeconds_.load(std::memory_order_relaxed);
}

size_t JitterViewRecorder::getLastSavedFrameCount() const {
    return lastSavedCount_.load(std::memory_order_relaxed);
}

bool JitterViewRecorder::hasSavedLog() const {
    return hasSavedLog_.load(std::memory_order_relaxed);
}

void JitterViewRecorder::startRecording() {
    std::lock_guard lock(mutex_);
    if (state_.load(std::memory_order_relaxed) != State::Idle) return;

    restoreDebugView_ = !ContextCensusGetJitterDebugView();
    if (restoreDebugView_) {
        ContextCensusSetJitterDebugView(true);
    }

    recordedFrames_.clear();
    for (size_t i = 0; i < kRingSize; ++i) {
        slots_[i] = JitterViewRingSlot{};
    }
    writeSlot_ = 0;
    readSlot_ = 0;
    droppedFrames_ = 0;
    drainingFramesLeft_ = 8;
    elapsedSeconds_.store(0.0f, std::memory_order_relaxed);
    hasSavedLog_.store(false, std::memory_order_relaxed);

    QueryPerformanceFrequency(&qpcFreq_);
    QueryPerformanceCounter(&qpcStart_);

    state_.store(State::Recording, std::memory_order_release);
    EdpeLog(L"EDPE: Jitter View recording started");
}

void JitterViewRecorder::releaseResources() {
    stagingBuffers_ = {};
    gpuUav_.Reset();
    gpuBuffer_.Reset();
    constantBuffer_.Reset();
    computeShader_.Reset();
    stateObject_.Reset();
    context1_.Reset();
    device_.Reset();

    inputSrv_.Reset();
    inputCopy_.Reset();
    inputCopyWidth_ = 0;
    inputCopyHeight_ = 0;
    inputCopyFormat_ = DXGI_FORMAT_UNKNOWN;
}

bool JitterViewRecorder::ensureResources(ID3D11Device* device, ID3D11DeviceContext* context,
                                         UINT width, UINT height, DXGI_FORMAT format) {
    if (!device || !context || width == 0 || height == 0) return false;

    if (device_.Get() != device) {
        releaseResources();
        device_ = device;

        Microsoft::WRL::ComPtr<ID3D11Device1> device1;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) ||
            FAILED(context->QueryInterface(IID_PPV_ARGS(&context1_)))) {
            releaseResources();
            return false;
        }

        const auto level = device->GetFeatureLevel();
        const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
            ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        if (FAILED(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION,
                __uuidof(ID3D11Device1), nullptr, &stateObject_))) {
            stateObject_.Reset();
        }

        Microsoft::WRL::ComPtr<ID3DBlob> csBlob, errBlob;
        if (FAILED(D3DCompile(kJitterViewStatsShader, sizeof(kJitterViewStatsShader) - 1,
                nullptr, nullptr, nullptr, "cs_main", "cs_5_0", 0, 0, &csBlob, &errBlob))) {
            releaseResources();
            return false;
        }

        if (FAILED(device->CreateComputeShader(csBlob->GetBufferPointer(),
                csBlob->GetBufferSize(), nullptr, &computeShader_))) {
            releaseResources();
            return false;
        }

        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = sizeof(StatsConstantBufferData);
        cbDesc.Usage = D3D11_USAGE_DEFAULT;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &constantBuffer_))) {
            releaseResources();
            return false;
        }

        D3D11_BUFFER_DESC gpuBufDesc{};
        gpuBufDesc.ByteWidth = static_cast<UINT>(kJitterViewGridTiles * sizeof(uint32_t));
        gpuBufDesc.Usage = D3D11_USAGE_DEFAULT;
        gpuBufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        gpuBufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        gpuBufDesc.StructureByteStride = sizeof(uint32_t);
        if (FAILED(device->CreateBuffer(&gpuBufDesc, nullptr, &gpuBuffer_))) {
            releaseResources();
            return false;
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = kJitterViewGridTiles;
        if (FAILED(device->CreateUnorderedAccessView(gpuBuffer_.Get(), &uavDesc, &gpuUav_))) {
            releaseResources();
            return false;
        }

        D3D11_BUFFER_DESC stgDesc{};
        stgDesc.ByteWidth = static_cast<UINT>(kJitterViewGridTiles * sizeof(uint32_t));
        stgDesc.Usage = D3D11_USAGE_STAGING;
        stgDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (size_t i = 0; i < kRingSize; ++i) {
            if (FAILED(device->CreateBuffer(&stgDesc, nullptr, &stagingBuffers_[i]))) {
                releaseResources();
                return false;
            }
        }
    }

    const DXGI_FORMAT concreteFormat = toConcreteLdrFormat(format);
    if (inputCopy_) {
        if (inputCopyWidth_ != width || inputCopyHeight_ != height || inputCopyFormat_ != concreteFormat) {
            inputCopy_.Reset();
            inputSrv_.Reset();
        }
    }

    if (!inputCopy_) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = concreteFormat;
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(device_->CreateTexture2D(&td, nullptr, &inputCopy_))) {
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = concreteFormat;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;

        if (FAILED(device_->CreateShaderResourceView(inputCopy_.Get(), &srvDesc, &inputSrv_))) {
            inputCopy_.Reset();
            return false;
        }

        inputCopyWidth_ = width;
        inputCopyHeight_ = height;
        inputCopyFormat_ = concreteFormat;
    }

    return true;
}

std::wstring JitterViewRecorder::getLogFilePath() const {
    wchar_t path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!len || len >= MAX_PATH) return L"";
    wchar_t* name = wcsrchr(path, L'\\');
    if (!name) return L"";
    const size_t prefix = name + 1 - path;
    if (prefix + sizeof(L"jitter_view.log") / sizeof(wchar_t) > MAX_PATH) return L"";
    wcscpy_s(name + 1, MAX_PATH - prefix, L"jitter_view.log");
    return path;
}

void JitterViewRecorder::triggerBackgroundWrite(UINT renderWidth, UINT renderHeight,
                                                UINT outputWidth, UINT outputHeight,
                                                bool isHdr, uint64_t droppedFrames) {
    JitterViewHeaderInfo header{};
    header.edpeVersion = kVersion;

    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char dateBuf[64]{};
    std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d %H:%M:%S", &tm);
    header.date = dateBuf;

    header.mode = ContextCensusGetJitterDebugMode();
    const char* const modes[] = {
        "Hybrid",
        "Spatial Shift",
        "Temporal Pulse",
        "Binary Mask"
    };
    if (header.mode >= 0 && header.mode < 4) {
        header.modeName = modes[header.mode];
    } else {
        header.modeName = "Unknown";
    }

    header.sensitivity = ContextCensusGetJitterDebugSensitivity();
    header.threshold = ContextCensusGetJitterDebugThreshold();
    header.renderWidth = renderWidth ? renderWidth : outputWidth;
    header.renderHeight = renderHeight ? renderHeight : outputHeight;
    header.outputWidth = outputWidth;
    header.outputHeight = outputHeight;

    auto& upscaler = UpscalerManager::instance();
    header.upscalerBackend = wideToUtf8(getUpscalerBackendName(upscaler.getBackend()));
    header.upscaleMode = wideToUtf8(getUpscaleModeName(upscaler.getMode()));
    header.isHdr = isHdr;
    header.droppedFrames = droppedFrames;

    auto framesCopy = std::move(recordedFrames_);
    const std::wstring logPath = getLogFilePath();

    if (writerThread_.joinable()) {
        writerThread_.join();
    }

    writerThread_ = std::thread([this, header = std::move(header), frames = std::move(framesCopy), logPath]() {
        const std::string report = generateJitterViewReport(header, frames);
        if (!logPath.empty()) {
            FILE* f = nullptr;
            if (_wfopen_s(&f, logPath.c_str(), L"wb") == 0 && f) {
                std::fwrite(report.data(), 1, report.size(), f);
                std::fclose(f);
            }
        }
        lastSavedCount_.store(frames.size(), std::memory_order_release);
        hasSavedLog_.store(true, std::memory_order_release);
        state_.store(State::Idle, std::memory_order_release);

        wchar_t msg[128];
        swprintf_s(msg, L"EDPE: Saved jitter_view.log (%zu frames)", frames.size());
        EdpeLog(msg);
    });
}

void JitterViewRecorder::onFrame(ID3D11DeviceContext* context,
                                 ID3D11Texture2D* backbuffer,
                                 unsigned long long rendered,
                                 UINT width, UINT height,
                                 UINT renderWidth, UINT renderHeight,
                                 float jx, float jy,
                                 bool isHdr, bool reset) {
    if (!context || !backbuffer || width == 0 || height == 0) return;

    const State curState = state_.load(std::memory_order_relaxed);
    if (curState == State::Idle || curState == State::Writing) return;

    LARGE_INTEGER qpcNow;
    QueryPerformanceCounter(&qpcNow);
    const double elapsedSec = (qpcFreq_.QuadPart > 0)
        ? static_cast<double>(qpcNow.QuadPart - qpcStart_.QuadPart) / static_cast<double>(qpcFreq_.QuadPart)
        : 0.0;
    elapsedSeconds_.store(static_cast<float>(elapsedSec), std::memory_order_relaxed);

    const double tMs = (qpcFreq_.QuadPart > 0)
        ? static_cast<double>(qpcNow.QuadPart - qpcStart_.QuadPart) * 1000.0 / static_cast<double>(qpcFreq_.QuadPart)
        : 0.0;

    if (curState == State::Recording) {
        if (elapsedSec >= 5.0) {
            state_.store(State::Draining, std::memory_order_release);
            drainingFramesLeft_ = 8;
        } else {
            if (slots_[writeSlot_].inFlight) {
                droppedFrames_++;
            } else {
                Microsoft::WRL::ComPtr<ID3D11Device> device;
                context->GetDevice(&device);
                if (!device) return;

                D3D11_TEXTURE2D_DESC bbDesc{};
                backbuffer->GetDesc(&bbDesc);
                const DXGI_FORMAT concreteFormat = toConcreteLdrFormat(bbDesc.Format);

                if (!ensureResources(device.Get(), context, width, height, concreteFormat)) {
                    EdpeLog(L"EDPE: JitterViewRecorder failed to allocate GPU resources - recording stopped");
                    state_.store(State::Idle, std::memory_order_release);
                    if (restoreDebugView_) {
                        ContextCensusSetJitterDebugView(false);
                        restoreDebugView_ = false;
                    }
                    return;
                }

                context->CopyResource(inputCopy_.Get(), backbuffer);

                const StatsConstantBufferData cbData{ width, height, {0, 0} };
                context->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &cbData, 0, 0);

                const UINT clearVals[4] = { 0, 0, 0, 0 };
                context->ClearUnorderedAccessViewUint(gpuUav_.Get(), clearVals);

                Microsoft::WRL::ComPtr<ID3DDeviceContextState> prior;
                if (stateObject_) {
                    context1_->SwapDeviceContextState(stateObject_.Get(), &prior);
                }

                ID3D11ShaderResourceView* srv = inputSrv_.Get();
                ID3D11UnorderedAccessView* uav = gpuUav_.Get();
                ID3D11Buffer* cb = constantBuffer_.Get();

                context1_->CSSetShader(computeShader_.Get(), nullptr, 0);
                context1_->CSSetConstantBuffers(0, 1, &cb);
                context1_->CSSetShaderResources(0, 1, &srv);
                context1_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

                context1_->Dispatch(64, 36, 1);

                ID3D11ShaderResourceView* nullSrv = nullptr;
                context1_->CSSetShaderResources(0, 1, &nullSrv);
                ID3D11UnorderedAccessView* nullUav = nullptr;
                context1_->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);

                if (stateObject_ && prior) {
                    context1_->SwapDeviceContextState(prior.Get(), nullptr);
                }

                context->CopyResource(stagingBuffers_[writeSlot_].Get(), gpuBuffer_.Get());

                const auto jl = JitterLogger::instance().getLatest();
                unsigned totalPhases = ContextCensusGetJitterPhaseCount();
                if (totalPhases == 0) totalPhases = 32;
                unsigned phase = 0;
                float camMove = jl.camMoveDist;
                float camRot = jl.camRotDeg;
                bool cameraValid = jl.cameraValid;
                bool heldJitter = jl.heldJitter;
                bool sceneCut = jl.sceneCut;

                if (jl.frame == rendered) {
                    phase = jl.phase;
                    if (jl.totalPhases > 0) totalPhases = jl.totalPhases;
                } else {
                    phase = static_cast<unsigned>(rendered % totalPhases);
                }

                auto& slot = slots_[writeSlot_];
                slot.renderedFrameId = rendered;
                slot.logFrameId = jl.frame;
                slot.qpcTicks = qpcNow.QuadPart;
                slot.tMs = static_cast<float>(tMs);
                slot.deltaMs = 0.0f;
                slot.jx = jx;
                slot.jy = jy;
                slot.phase = phase;
                slot.totalPhases = totalPhases;
                slot.camMove = camMove;
                slot.camRot = camRot;
                slot.cameraValid = cameraValid;
                slot.heldJitter = heldJitter;
                slot.sceneCut = sceneCut;
                slot.reset = reset;
                slot.inFlight = true;

                writeSlot_ = (writeSlot_ + 1) % kRingSize;
            }

            while (slots_[readSlot_].inFlight) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                const HRESULT hr = context->Map(stagingBuffers_[readSlot_].Get(), 0, D3D11_MAP_READ,
                                                D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                    break;
                }
                if (SUCCEEDED(hr)) {
                    JitterViewFrameData frameData{};
                    const auto& s = slots_[readSlot_];
                    frameData.meta.frame = s.logFrameId;
                    frameData.meta.renderedFrameId = s.renderedFrameId;
                    frameData.meta.timeMs = s.tMs;
                    frameData.meta.deltaMs = (!recordedFrames_.empty()) ? (s.tMs - recordedFrames_.back().meta.timeMs) : 0.0f;
                    frameData.meta.phase = s.phase;
                    frameData.meta.totalPhases = s.totalPhases;
                    frameData.meta.pixelX = s.jx;
                    frameData.meta.pixelY = s.jy;
                    frameData.meta.camMoveDist = s.camMove;
                    frameData.meta.camRotDeg = s.camRot;
                    frameData.meta.cameraValid = s.cameraValid;
                    frameData.meta.heldJitter = s.heldJitter;
                    frameData.meta.sceneCut = s.sceneCut;
                    frameData.meta.reset = s.reset;

                    std::memcpy(frameData.tiles.data(), mapped.pData, kJitterViewGridTiles * sizeof(uint32_t));
                    context->Unmap(stagingBuffers_[readSlot_].Get(), 0);
                    slots_[readSlot_].inFlight = false;
                    recordedFrames_.push_back(std::move(frameData));
                    readSlot_ = (readSlot_ + 1) % kRingSize;
                } else {
                    slots_[readSlot_].inFlight = false;
                    readSlot_ = (readSlot_ + 1) % kRingSize;
                }
            }
        }
    }

    if (state_.load(std::memory_order_relaxed) == State::Draining) {
        while (slots_[readSlot_].inFlight) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT hr = context->Map(stagingBuffers_[readSlot_].Get(), 0, D3D11_MAP_READ,
                                            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                break;
            }
            if (SUCCEEDED(hr)) {
                JitterViewFrameData frameData{};
                const auto& s = slots_[readSlot_];
                frameData.meta.frame = s.logFrameId;
                frameData.meta.renderedFrameId = s.renderedFrameId;
                frameData.meta.timeMs = s.tMs;
                frameData.meta.deltaMs = (!recordedFrames_.empty()) ? (s.tMs - recordedFrames_.back().meta.timeMs) : 0.0f;
                frameData.meta.phase = s.phase;
                frameData.meta.totalPhases = s.totalPhases;
                frameData.meta.pixelX = s.jx;
                frameData.meta.pixelY = s.jy;
                frameData.meta.camMoveDist = s.camMove;
                frameData.meta.camRotDeg = s.camRot;
                frameData.meta.cameraValid = s.cameraValid;
                frameData.meta.heldJitter = s.heldJitter;
                frameData.meta.sceneCut = s.sceneCut;
                frameData.meta.reset = s.reset;

                std::memcpy(frameData.tiles.data(), mapped.pData, kJitterViewGridTiles * sizeof(uint32_t));
                context->Unmap(stagingBuffers_[readSlot_].Get(), 0);
                slots_[readSlot_].inFlight = false;
                recordedFrames_.push_back(std::move(frameData));
                readSlot_ = (readSlot_ + 1) % kRingSize;
            } else {
                slots_[readSlot_].inFlight = false;
                readSlot_ = (readSlot_ + 1) % kRingSize;
            }
        }

        if (drainingFramesLeft_ > 0) {
            drainingFramesLeft_--;
        }

        bool anyInFlight = false;
        for (size_t i = 0; i < kRingSize; ++i) {
            if (slots_[i].inFlight) {
                anyInFlight = true;
                break;
            }
        }

        if (!anyInFlight || drainingFramesLeft_ == 0) {
            state_.store(State::Writing, std::memory_order_release);
            if (restoreDebugView_) {
                ContextCensusSetJitterDebugView(false);
                restoreDebugView_ = false;
            }
            triggerBackgroundWrite(renderWidth, renderHeight, width, height, isHdr, droppedFrames_);
        }
    }
}

}
