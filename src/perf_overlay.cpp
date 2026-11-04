#include "perf_overlay.h"
#include "upscaler_backend.h"
#include "context_census.h"
#include "ngx_context.h"
#include "config.h"
#include "profiler.h"
#include "log.h"

#include <imgui.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <windows.h>

namespace edpe {

PerfTracker& PerfTracker::instance() {
    static PerfTracker s_tracker;
    return s_tracker;
}

PerfTracker::PerfTracker() {
    QueryPerformanceFrequency(&perfFreq_);
    initNvml();
}

PerfTracker::~PerfTracker() {
    shutdown();
}

void PerfTracker::shutdown() {
    {
        std::lock_guard<std::mutex> lock(nvmlMutex_);
        nvmlRunning_.store(false, std::memory_order_release);
    }
    nvmlCv_.notify_all();
    if (nvmlWorkerThread_.joinable()) {
        nvmlWorkerThread_.join();
    }
    if (nvmlModule_) {
        auto nvmlShutdown = reinterpret_cast<NvmlShutdown_t>(GetProcAddress(nvmlModule_, "nvmlShutdown"));
        if (nvmlShutdown) {
            nvmlShutdown();
        }
        FreeLibrary(nvmlModule_);
        nvmlModule_ = nullptr;
    }
    nvmlAvailable_ = false;
    nvmlDevice_ = nullptr;
    nvmlGetUtil_ = nullptr;
    nvmlGetMem_ = nullptr;
}

void PerfTracker::initNvml() {
    if (nvmlModule_) return;
    nvmlModule_ = LoadLibraryW(L"nvml.dll");
    if (!nvmlModule_) return;

    auto nvmlInit = reinterpret_cast<NvmlInit_t>(GetProcAddress(nvmlModule_, "nvmlInit_v2"));
    auto nvmlGetHandle = reinterpret_cast<NvmlGetHandle_t>(GetProcAddress(nvmlModule_, "nvmlDeviceGetHandleByIndex_v2"));
    nvmlGetUtil_ = reinterpret_cast<NvmlGetUtil_t>(GetProcAddress(nvmlModule_, "nvmlDeviceGetUtilizationRates"));
    nvmlGetMem_ = reinterpret_cast<NvmlGetMem_t>(GetProcAddress(nvmlModule_, "nvmlDeviceGetMemoryInfo"));

    if (nvmlInit && nvmlGetHandle && nvmlGetUtil_ && nvmlInit() == 0) {
        if (nvmlGetHandle(0, &nvmlDevice_) == 0) {
            nvmlAvailable_ = true;
            nvmlRunning_.store(true, std::memory_order_relaxed);
            nvmlWorkerThread_ = std::thread([this]() {
                while (true) {
                    {
                        std::unique_lock<std::mutex> lock(nvmlMutex_);
                        nvmlCv_.wait_for(lock, std::chrono::milliseconds(500), [this]() {
                            return !nvmlRunning_.load(std::memory_order_acquire);
                        });
                        if (!nvmlRunning_.load(std::memory_order_acquire)) break;
                    }
                    NvmlUtil_t util{};
                    if (nvmlGetUtil_ && nvmlDevice_ && nvmlGetUtil_(nvmlDevice_, &util) == 0) {
                        cachedGpuUsage_.store(util.gpu, std::memory_order_relaxed);
                    }
                }
            });
            EdpeLog(L"EDPE: NVML hardware telemetry initialized successfully");
        }
    }
}

void PerfTracker::onPresentStart(IDXGISwapChain* swapChain) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (lastPresentStart_.QuadPart > 0 && perfFreq_.QuadPart > 0) {
        const double deltaMs = static_cast<double>(now.QuadPart - lastPresentStart_.QuadPart) * 1000.0 / static_cast<double>(perfFreq_.QuadPart);
        if (deltaMs >= 1.5) {
            lastPresentStart_ = now;
            if (deltaMs < 1000.0) {
                telemetry_.totalFrametimeMs = static_cast<float>(deltaMs);
                smoothedFrametimeMs_ = (smoothedFrametimeMs_ > 0.0f)
                    ? (smoothedFrametimeMs_ * 0.90f + telemetry_.totalFrametimeMs * 0.10f)
                    : telemetry_.totalFrametimeMs;
                telemetry_.fps = (smoothedFrametimeMs_ > 0.0f) ? (1000.0f / smoothedFrametimeMs_) : 0.0f;
            }
        }
    } else {
        lastPresentStart_ = now;
    }
    currentPresentStart_ = now;

    // Retrieve active upscaler timing
    auto& upscaler = UpscalerManager::instance();
    if (upscaler.isReconstructionActive()) {
        const auto& diag = upscaler.getDiagnostics();
        telemetry_.upscalerFrametimeMs = diag.gpuTimeMs;
    } else {
        telemetry_.upscalerFrametimeMs = 0.0f;
    }

    // Resolution dimensions
    UINT outW = 0, outH = 0;
    if (swapChain) {
        DXGI_SWAP_CHAIN_DESC scDesc{};
        if (SUCCEEDED(swapChain->GetDesc(&scDesc))) {
            outW = scDesc.BufferDesc.Width;
            outH = scDesc.BufferDesc.Height;
        }
    }
    UINT sceneW = 0, sceneH = 0;
    ContextCensusGetSceneRenderSize(&sceneW, &sceneH);
    if (!sceneW || !sceneH) {
        sceneW = outW;
        sceneH = outH;
    }
    telemetry_.renderWidth = sceneW;
    telemetry_.renderHeight = sceneH;
    telemetry_.outputWidth = outW;
    telemetry_.outputHeight = outH;

    // Game frametime is time spent by game engine before upscaler / composite
    telemetry_.gameFrametimeMs = (std::max)(0.0f, telemetry_.totalFrametimeMs - telemetry_.upscalerFrametimeMs);

    FrametimeSample sample;
    sample.gameMs = telemetry_.gameFrametimeMs;
    sample.upscalerMs = telemetry_.upscalerFrametimeMs;
    sample.totalMs = telemetry_.totalFrametimeMs;
    frametimeHistory_[historyIndex_] = sample;
    historyIndex_ = (historyIndex_ + 1) % kFrametimeHistorySize;
    if (historyCount_ < kFrametimeHistorySize) {
        historyCount_++;
    }

    // Rate-limit hardware telemetry to ~4 times per second (250 ms) BEFORE updating cached display strings
    if (perfFreq_.QuadPart > 0) {
        const double elapsedSinceUpdateMs = static_cast<double>(now.QuadPart - lastTelemetryUpdate_.QuadPart) * 1000.0 / static_cast<double>(perfFreq_.QuadPart);
        if (lastTelemetryUpdate_.QuadPart == 0 || elapsedSinceUpdateMs >= 250.0) {
            updateTelemetry(swapChain);
            lastTelemetryUpdate_ = now;
        }
    }

    // Rate-limit display telemetry & profiler updates to 66.67 ms (15 Hz / 15 FPS) using high-resolution steady_clock
    const auto nowTime = std::chrono::steady_clock::now();
    const double elapsedCacheMs = (lastCacheUpdate_.time_since_epoch().count() == 0)
        ? 1000.0
        : std::chrono::duration<double, std::milli>(nowTime - lastCacheUpdate_).count();

    if (elapsedCacheMs >= 66.67) {
        updateCachedStats();
        lastCacheUpdate_ = nowTime;
    }
}

void PerfTracker::onPresentEnd() {
    QueryPerformanceCounter(&lastPresentEnd_);
}

float PerfTracker::calculateOnePercentLow() const {
    if (historyCount_ < 5) {
        return telemetry_.fps;
    }

    std::array<float, kFrametimeHistorySize> times{};
    size_t validCount = 0;
    const size_t count = historyCount_;
    const size_t startIndex = (count < kFrametimeHistorySize) ? 0 : historyIndex_;

    for (size_t i = 0; i < count; ++i) {
        const float t = frametimeHistory_[(startIndex + i) % kFrametimeHistorySize].totalMs;
        if (t > 0.5f && t < 1000.0f) {
            times[validCount++] = t;
        }
    }

    if (validCount < 5) {
        return telemetry_.fps;
    }

    // Number of slowest samples in top 1% (at least 1 sample)
    const size_t numSlowest = (std::max)(size_t(1), static_cast<size_t>(std::ceil(validCount * 0.01f)));

    // Partition so the first `numSlowest` elements are the largest (worst) frametimes in descending order
    std::nth_element(times.begin(), times.begin() + (numSlowest - 1), times.begin() + validCount, std::greater<float>());

    // Compute the average of the slowest 1% frametimes (1% Low Average) to stabilize spikes
    float sumSlowestMs = 0.0f;
    for (size_t i = 0; i < numSlowest; ++i) {
        sumSlowestMs += times[i];
    }
    const float avgSlowestMs = sumSlowestMs / static_cast<float>(numSlowest);
    if (avgSlowestMs <= 0.001f) {
        return telemetry_.fps;
    }

    const float oneLow = 1000.0f / avgSlowestMs;
    return (telemetry_.fps > 0.0f) ? (std::min)(telemetry_.fps, oneLow) : oneLow;
}

void PerfTracker::updateCachedStats() {
    cachedStats_.fps = telemetry_.fps;
    cachedStats_.onePercentLowFps = calculateOnePercentLow();
    cachedStats_.gameFrametimeMs = telemetry_.gameFrametimeMs;
    cachedStats_.upscalerFrametimeMs = telemetry_.upscalerFrametimeMs;
    cachedStats_.totalFrametimeMs = telemetry_.totalFrametimeMs;
    cachedStats_.renderWidth = telemetry_.renderWidth;
    cachedStats_.renderHeight = telemetry_.renderHeight;
    cachedStats_.outputWidth = telemetry_.outputWidth;
    cachedStats_.outputHeight = telemetry_.outputHeight;
    cachedStats_.gpuName = telemetry_.gpuName;
    cachedStats_.gpuUsagePct = telemetry_.gpuUsagePct;
    cachedStats_.gpuLoadAvailable = telemetry_.gpuLoadAvailable;
    cachedStats_.vramUsedBytes = telemetry_.vramUsedBytes;
    cachedStats_.vramTotalBytes = telemetry_.vramTotalBytes;

    // Pre-format all telemetry strings at 15 Hz / 66.67 ms for zero per-frame UI formatting overhead
    cachedStats_.fpsColor = (cachedStats_.fps >= 58.0f) ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f) :
                            (cachedStats_.fps >= 30.0f) ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) :
                                                          ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    snprintf(cachedStats_.fpsLine, sizeof(cachedStats_.fpsLine),
        "FPS: %5.1f | 1%% Low: %5.1f FPS", cachedStats_.fps, cachedStats_.onePercentLowFps);

    snprintf(cachedStats_.gameFtLine, sizeof(cachedStats_.gameFtLine),
        "%6.2f ms", cachedStats_.gameFrametimeMs);

    auto& upscaler = UpscalerManager::instance();
    UpscalerBackend shownBackend = upscaler.getBackend();
    bool upscalerActive = upscaler.isReconstructionActive();
    const float upscalerMs = (upscalerActive && cachedStats_.upscalerFrametimeMs > 0.0f) ? cachedStats_.upscalerFrametimeMs : 0.0f;
    snprintf(cachedStats_.upscalerFtLine, sizeof(cachedStats_.upscalerFtLine),
        "%6.2f ms", upscalerMs);

    snprintf(cachedStats_.totalFtLine, sizeof(cachedStats_.totalFtLine),
        "%6.2f ms", cachedStats_.totalFrametimeMs);

    uint32_t rW = cachedStats_.renderWidth;
    uint32_t rH = cachedStats_.renderHeight;
    const uint32_t oW = cachedStats_.outputWidth;
    const uint32_t oH = cachedStats_.outputHeight;
    if ((rW == 0 || rH == 0) && (oW > 0 && oH > 0)) {
        rW = oW;
        rH = oH;
    }
    const float scalePct = (oW > 0) ? (100.0f * static_cast<float>(rW) / static_cast<float>(oW)) : 100.0f;
    const bool isSr = (rW < oW || rH < oH);

    if (upscalerActive) {
        const bool isAmd = (shownBackend == UpscalerBackend::AmdFsr);
        const bool isTaa = (shownBackend == UpscalerBackend::Taa);
        if (isTaa) {
            snprintf(cachedStats_.upscalerInfoLine, sizeof(cachedStats_.upscalerInfoLine), "TAA (%.1f%%)", scalePct);
        } else if (isAmd) {
            snprintf(cachedStats_.upscalerInfoLine, sizeof(cachedStats_.upscalerInfoLine), "%s (%.1f%%)",
                isSr ? "AMD FSR SR" : "AMD FSR Native AA", scalePct);
        } else {
            snprintf(cachedStats_.upscalerInfoLine, sizeof(cachedStats_.upscalerInfoLine), "%s (%.1f%%)",
                isSr ? "NVIDIA DLSS" : "NVIDIA DLAA", scalePct);
        }
    } else {
        snprintf(cachedStats_.upscalerInfoLine, sizeof(cachedStats_.upscalerInfoLine), "Bypass (Native) (100.0%%)");
    }

    char gpuNameUtf8[128]{};
    WideCharToMultiByte(CP_UTF8, 0, cachedStats_.gpuName.c_str(), -1, gpuNameUtf8, sizeof(gpuNameUtf8), nullptr, nullptr);
    const char* gpuDisplayName = gpuNameUtf8[0] ? gpuNameUtf8 : "Direct3D 11 Adapter";
    if (cachedStats_.gpuLoadAvailable) {
        if (cachedStats_.gpuUsagePct >= 100) {
            snprintf(cachedStats_.gpuLine, sizeof(cachedStats_.gpuLine), "%s (100%%)", gpuDisplayName);
        } else if (cachedStats_.gpuUsagePct >= 10) {
            snprintf(cachedStats_.gpuLine, sizeof(cachedStats_.gpuLine), "%s (%u%%) ", gpuDisplayName, cachedStats_.gpuUsagePct);
        } else {
            snprintf(cachedStats_.gpuLine, sizeof(cachedStats_.gpuLine), "%s (%u%%)  ", gpuDisplayName, cachedStats_.gpuUsagePct);
        }
    } else {
        snprintf(cachedStats_.gpuLine, sizeof(cachedStats_.gpuLine), "%s (N/A) ", gpuDisplayName);
    }

    if (cachedStats_.vramTotalBytes > 0) {
        const double usedGb = static_cast<double>(cachedStats_.vramUsedBytes) / (1024.0 * 1024.0 * 1024.0);
        const double totalGb = static_cast<double>(cachedStats_.vramTotalBytes) / (1024.0 * 1024.0 * 1024.0);
        const unsigned pct = static_cast<unsigned>(100.0 * static_cast<double>(cachedStats_.vramUsedBytes) / static_cast<double>(cachedStats_.vramTotalBytes));
        if (pct >= 100) {
            snprintf(cachedStats_.vramLine, sizeof(cachedStats_.vramLine), "%.2f / %.2f GB (100%%)", usedGb, totalGb);
        } else if (pct >= 10) {
            snprintf(cachedStats_.vramLine, sizeof(cachedStats_.vramLine), "%.2f / %.2f GB (%u%%) ", usedGb, totalGb, pct);
        } else {
            snprintf(cachedStats_.vramLine, sizeof(cachedStats_.vramLine), "%.2f / %.2f GB (%u%%)  ", usedGb, totalGb, pct);
        }
    } else if (cachedStats_.vramUsedBytes > 0) {
        const double usedMb = static_cast<double>(cachedStats_.vramUsedBytes) / (1024.0 * 1024.0);
        snprintf(cachedStats_.vramLine, sizeof(cachedStats_.vramLine), "%.1f MB", usedMb);
    } else {
        snprintf(cachedStats_.vramLine, sizeof(cachedStats_.vramLine), "N/A");
    }

    snprintf(cachedStats_.resLine, sizeof(cachedStats_.resLine),
        "Resolution: Scene %ux%u -> Output %ux%u (%.1f%%)", rW, rH, oW, oH, scalePct);

    const bool backend_active = (shownBackend != UpscalerBackend::None);
    const bool nvidia_active = (shownBackend == UpscalerBackend::NvidiaDlss);
    const bool amd_active = (shownBackend == UpscalerBackend::AmdFsr);
    const bool taa_active = (shownBackend == UpscalerBackend::Taa);
    const uint32_t dsvBinds = ContextCensusGetDsvBinds();
    const bool isReconActive = nvidia_active ? edpe::NgxContext::has_evaluated()
                                            : (upscaler.isReconstructionActive() && upscaler.hasEvaluated());

    if (backend_active) {
        const char* backendName = taa_active ? "Native D3D11" : (amd_active ? "AMD" : "NVIDIA");
        const char* techName = taa_active
            ? (isSr ? "TAA Super Resolution" : "TAA Native AA")
            : (amd_active ? (isSr ? "FSR Super Resolution" : "FSR Native AA")
                          : (isSr ? "DLSS Super Resolution" : "DLAA"));
        if (isReconActive) {
            cachedStats_.reconStatusColor = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);
            snprintf(cachedStats_.reconStatusLine, sizeof(cachedStats_.reconStatusLine),
                "Reconstruction: %s %s Active", backendName, techName);
        } else if (dsvBinds == 0) {
            cachedStats_.reconStatusColor = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
            snprintf(cachedStats_.reconStatusLine, sizeof(cachedStats_.reconStatusLine),
                "Reconstruction: %s %s (Initialized, no depth found)", backendName, techName);
        } else {
            cachedStats_.reconStatusColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
            snprintf(cachedStats_.reconStatusLine, sizeof(cachedStats_.reconStatusLine),
                "Reconstruction: %s %s (Standby)", backendName, techName);
        }
    } else {
        cachedStats_.reconStatusColor = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
        snprintf(cachedStats_.reconStatusLine, sizeof(cachedStats_.reconStatusLine),
            "Reconstruction: Inactive (Scene %ux%u -> Output %ux%u)", rW, rH, oW, oH);
    }

    if (Profiler::instance().isEnabled()) {
        profilerDisplay_.mapMs = Profiler::instance().getAvgMs(ProfileZone::MapUnmap);
        profilerDisplay_.omMs = Profiler::instance().getAvgMs(ProfileZone::OMSetRenderTargets);
        profilerDisplay_.deltaMs = Profiler::instance().getAvgMs(ProfileZone::CameraDeltaCopy);
        profilerDisplay_.drawMs = Profiler::instance().getAvgMs(ProfileZone::DrawProbe);
        profilerDisplay_.upscaleMs = Profiler::instance().getAvgMs(ProfileZone::TemporalUpscale);
        const float measuredSum = profilerDisplay_.mapMs + profilerDisplay_.omMs + profilerDisplay_.deltaMs + profilerDisplay_.drawMs;
        profilerDisplay_.unaccountedMs = (std::max)(0.0f, cachedStats_.gameFrametimeMs - measuredSum);
    }
}

void PerfTracker::updateTelemetry(IDXGISwapChain* swapChain) {
    if (!swapChain) return;

    // 1. Static GPU adapter information
    if (!gpuInfoCached_) {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device)))) {
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
            if (SUCCEEDED(device.As(&dxgiDevice))) {
                Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
                if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                    DXGI_ADAPTER_DESC desc{};
                    if (SUCCEEDED(adapter->GetDesc(&desc))) {
                        telemetry_.gpuName = desc.Description;
                        telemetry_.vramTotalBytes = desc.DedicatedVideoMemory;
                        gpuInfoCached_ = true;
                    }
                }
            }
        }
    }

    // 2. Video Memory (VRAM) Usage
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device)))) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        if (SUCCEEDED(device.As(&dxgiDevice))) {
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
                if (SUCCEEDED(adapter.As(&adapter3))) {
                    DXGI_QUERY_VIDEO_MEMORY_INFO memInfo{};
                    if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo))) {
                        telemetry_.vramUsedBytes = memInfo.CurrentUsage;
                    }
                }
            }
        }
    }

    // 3. GPU Utilization Percentage (%)
    if (nvmlAvailable_ && nvmlDevice_) {
        telemetry_.gpuUsagePct = cachedGpuUsage_.load(std::memory_order_relaxed);
        telemetry_.gpuLoadAvailable = true;
    } else {
        telemetry_.gpuUsagePct = 0;
        telemetry_.gpuLoadAvailable = false;
    }
}

void PerfTracker::renderFrametimeGraph(float scale, const PerfOverlayOverride* override) {
    ImGui::Spacing();

    const float availW = ImGui::GetContentRegionAvail().x;
    const float graphWidth = (availW > 40.0f) ? availW : 240.0f * scale;
    const float graphHeight = 42.0f * scale;

    const ImVec2 pMin = ImGui::GetCursorScreenPos();
    const ImVec2 pMax = ImVec2(pMin.x + graphWidth, pMin.y + graphHeight);
    ImGui::Dummy(ImVec2(0.0f, graphHeight));

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (!drawList) return;

    // Background & border
    drawList->AddRectFilled(pMin, pMax, IM_COL32(20, 22, 27, 230), 2.0f * scale);
    drawList->AddRect(pMin, pMax, IM_COL32(70, 75, 85, 180), 2.0f * scale);

    const size_t totalCount = historyCount_;
    const size_t count = (std::min)(totalCount, kGraphSampleCount);
    const size_t startIndex = (historyIndex_ + kFrametimeHistorySize - count) % kFrametimeHistorySize;

    // Calculate vertical scaling with headroom
    float maxMs = 20.0f;
    for (size_t i = 0; i < count; ++i) {
        const auto& s = frametimeHistory_[(startIndex + i) % kFrametimeHistorySize];
        if (s.totalMs > maxMs) maxMs = s.totalMs;
        if (s.gameMs > maxMs) maxMs = s.gameMs;
    }
    maxMs = (std::min)(maxMs * 1.15f, 120.0f);

    const float innerH = graphHeight - 4.0f * scale;
    const float bottomY = pMax.y - 2.0f * scale;

    // Helper to draw horizontal dashed reference lines across the graph
    auto drawDashedHLine = [&](float y, ImU32 col, float dash, float gap) {
        float x = pMin.x + 2.0f * scale;
        const float xEnd = pMax.x - 2.0f * scale;
        while (x < xEnd) {
            const float nextX = (std::min)(x + dash, xEnd);
            drawList->AddLine(ImVec2(x, y), ImVec2(nextX, y), col, 1.0f);
            x += dash + gap;
        }
    };

    // 1. Reference line at 8.3 ms (120 FPS)
    if (maxMs >= 8.3f) {
        const float y83 = bottomY - (8.3f / maxMs) * innerH;
        drawDashedHLine(y83, IM_COL32(110, 115, 125, 120), 4.0f * scale, 3.0f * scale);
        const char* label83 = "8.3 ms";
        const ImVec2 size83 = ImGui::CalcTextSize(label83);
        const float label83X = pMax.x - size83.x - 4.0f * scale;
        const float label83Y = (std::max)(pMin.y + 1.0f * scale, y83 - size83.y);
        drawList->AddText(ImVec2(label83X, label83Y), IM_COL32(140, 145, 155, 160), label83);
    }

    // 2. Reference line at 16.6 ms (60 FPS)
    if (maxMs >= 16.6f) {
        const float y166 = bottomY - (16.6f / maxMs) * innerH;
        drawDashedHLine(y166, IM_COL32(110, 115, 125, 120), 4.0f * scale, 3.0f * scale);
        const char* label166 = "16.6 ms";
        const ImVec2 size166 = ImGui::CalcTextSize(label166);
        const float label166X = pMax.x - size166.x - 4.0f * scale;
        const float label166Y = (std::max)(pMin.y + 1.0f * scale, y166 - size166.y);
        drawList->AddText(ImVec2(label166X, label166Y), IM_COL32(140, 145, 155, 160), label166);
    }

    if (count < 2) return;

    // 1. Total Frametime: Orange
    for (size_t i = 0; i < count - 1; ++i) {
        const auto& s0 = frametimeHistory_[(startIndex + i) % kFrametimeHistorySize];
        const auto& s1 = frametimeHistory_[(startIndex + i + 1) % kFrametimeHistorySize];
        const float x0 = pMin.x + (static_cast<float>(i) / static_cast<float>(count - 1)) * graphWidth;
        const float x1 = pMin.x + (static_cast<float>(i + 1) / static_cast<float>(count - 1)) * graphWidth;
        const float y0 = bottomY - ((std::clamp)(s0.totalMs, 0.0f, maxMs) / maxMs) * innerH;
        const float y1 = bottomY - ((std::clamp)(s1.totalMs, 0.0f, maxMs) / maxMs) * innerH;
        drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 150, 20, 255), 1.6f * scale);
    }

    // 2. Game Frametime: White
    for (size_t i = 0; i < count - 1; ++i) {
        const auto& s0 = frametimeHistory_[(startIndex + i) % kFrametimeHistorySize];
        const auto& s1 = frametimeHistory_[(startIndex + i + 1) % kFrametimeHistorySize];
        const float x0 = pMin.x + (static_cast<float>(i) / static_cast<float>(count - 1)) * graphWidth;
        const float x1 = pMin.x + (static_cast<float>(i + 1) / static_cast<float>(count - 1)) * graphWidth;
        const float y0 = bottomY - ((std::clamp)(s0.gameMs, 0.0f, maxMs) / maxMs) * innerH;
        const float y1 = bottomY - ((std::clamp)(s1.gameMs, 0.0f, maxMs) / maxMs) * innerH;
        drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 255, 255, 240), 1.2f * scale);
    }

    // 3. Upscaler Frametime: Cyan
    for (size_t i = 0; i < count - 1; ++i) {
        const auto& s0 = frametimeHistory_[(startIndex + i) % kFrametimeHistorySize];
        const auto& s1 = frametimeHistory_[(startIndex + i + 1) % kFrametimeHistorySize];
        const float x0 = pMin.x + (static_cast<float>(i) / static_cast<float>(count - 1)) * graphWidth;
        const float x1 = pMin.x + (static_cast<float>(i + 1) / static_cast<float>(count - 1)) * graphWidth;
        const float y0 = bottomY - ((std::clamp)(s0.upscalerMs, 0.0f, maxMs) / maxMs) * innerH;
        const float y1 = bottomY - ((std::clamp)(s1.upscalerMs, 0.0f, maxMs) / maxMs) * innerH;
        drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 240, 240, 255), 1.2f * scale);
    }
}

void PerfTracker::renderOverlay(bool isMenuOpen, int corner, float scale, const PerfOverlayOverride* override) {
    auto& cfg = edpe::Config::instance();
    const float PAD = 10.0f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 work_pos = viewport->WorkPos;
    const ImVec2 work_size = viewport->WorkSize;
    ImVec2 window_pos, window_pos_pivot;

    switch (corner) {
    case 1: // Top
        window_pos = ImVec2(work_pos.x + work_size.x * 0.5f, work_pos.y + PAD);
        window_pos_pivot = ImVec2(0.5f, 0.0f);
        break;
    case 2: // Top-Right
        window_pos = ImVec2(work_pos.x + work_size.x - PAD, work_pos.y + PAD);
        window_pos_pivot = ImVec2(1.0f, 0.0f);
        break;
    case 3: // Left
        window_pos = ImVec2(work_pos.x + PAD, work_pos.y + work_size.y * 0.5f);
        window_pos_pivot = ImVec2(0.0f, 0.5f);
        break;
    case 4: // Right
        window_pos = ImVec2(work_pos.x + work_size.x - PAD, work_pos.y + work_size.y * 0.5f);
        window_pos_pivot = ImVec2(1.0f, 0.5f);
        break;
    case 5: // Bottom-Left
        window_pos = ImVec2(work_pos.x + PAD, work_pos.y + work_size.y - PAD);
        window_pos_pivot = ImVec2(0.0f, 1.0f);
        break;
    case 6: // Bottom
        window_pos = ImVec2(work_pos.x + work_size.x * 0.5f, work_pos.y + work_size.y - PAD);
        window_pos_pivot = ImVec2(0.5f, 1.0f);
        break;
    case 7: // Bottom-Right
        window_pos = ImVec2(work_pos.x + work_size.x - PAD, work_pos.y + work_size.y - PAD);
        window_pos_pivot = ImVec2(1.0f, 1.0f);
        break;
    case 0: // Top-Left
    default:
        window_pos = ImVec2(work_pos.x + PAD, work_pos.y + PAD);
        window_pos_pivot = ImVec2(0.0f, 0.0f);
        break;
    }

    static int s_last_corner = -1;
    static bool s_was_custom_pos = false;
    const bool corner_changed = (s_last_corner != corner);
    const bool snap_to_corner = (s_was_custom_pos && cfg.perfOverlayPosX < 0.0f) || (cfg.perfOverlayPosX < 0.0f && corner_changed);
    s_last_corner = corner;

    if (cfg.perfOverlayPosX >= 0.0f && cfg.perfOverlayPosY >= 0.0f) {
        ImGui::SetNextWindowPos(ImVec2(cfg.perfOverlayPosX, cfg.perfOverlayPosY), isMenuOpen ? ImGuiCond_Appearing : ImGuiCond_Always);
        s_was_custom_pos = true;
    } else {
        ImGui::SetNextWindowPos(window_pos, (!isMenuOpen || snap_to_corner) ? ImGuiCond_Always : ImGuiCond_Appearing, window_pos_pivot);
        s_was_custom_pos = false;
    }
    ImGui::SetNextWindowBgAlpha(0.70f);

    static float s_max_overlay_width = 0.0f;
    static float s_last_scale = -1.0f;
    static int s_last_backend = -1;
    const int cur_backend = static_cast<int>(edpe::UpscalerManager::instance().getBackend());
    if (std::fabs(scale - s_last_scale) > 0.01f || cur_backend != s_last_backend) {
        s_max_overlay_width = 0.0f;
        s_last_scale = scale;
        s_last_backend = cur_backend;
    }
    if (s_max_overlay_width > 0.0f) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(s_max_overlay_width, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    }

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav;
    if (!isMenuOpen) {
        window_flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
    }

    if (ImGui::Begin("##EDPE_PerfOverlay", nullptr, window_flags)) {
        const float curW = ImGui::GetWindowWidth();
        if (curW > s_max_overlay_width) {
            s_max_overlay_width = curW;
        }

        if (isMenuOpen) {
            static bool s_is_dragging_hud = false;
            if ((ImGui::IsWindowHovered() || ImGui::IsWindowFocused()) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                s_is_dragging_hud = true;
            }
            if (s_is_dragging_hud && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                const ImVec2 curPos = ImGui::GetWindowPos();
                if (std::fabs(curPos.x - cfg.perfOverlayPosX) > 0.5f || std::fabs(curPos.y - cfg.perfOverlayPosY) > 0.5f) {
                    cfg.perfOverlayPosX = curPos.x;
                    cfg.perfOverlayPosY = curPos.y;
                    cfg.save();
                }
                s_is_dragging_hud = false;
            }
        }
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "EDPE Performance HUD");
        ImGui::Separator();

        // 1. FPS with 1% Low FPS (pre-formatted at 66.67 ms, zero-overhead TextUnformatted)
        if (override) {
            const ImVec4 fpsColor = (telemetry_.fps >= 58.0f) ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f) :
                                    (telemetry_.fps >= 30.0f) ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) :
                                                                ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, fpsColor);
            ImGui::Text("FPS: %5.1f | 1%% Low: %5.1f FPS", telemetry_.fps, telemetry_.fps);
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, cachedStats_.fpsColor);
            ImGui::TextUnformatted(cachedStats_.fpsLine);
            ImGui::PopStyleColor();
        }

        // 2. Frametimes (pre-formatted at 66.67 ms, zero-overhead TextUnformatted)
        ImGui::TextUnformatted("Game Frametime:     ");
        ImGui::SameLine();
        if (override) {
            const float shownGameMs = (std::max)(0.0f, telemetry_.totalFrametimeMs - override->upscalerMs);
            ImGui::Text("%6.2f ms", shownGameMs);
        } else {
            ImGui::TextUnformatted(cachedStats_.gameFtLine);
        }

        ImGui::TextUnformatted("Upscaler Frametime: ");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.95f, 0.95f, 1.0f));
        if (override) {
            ImGui::Text("%6.2f ms", override->upscalerMs);
        } else {
            ImGui::TextUnformatted(cachedStats_.upscalerFtLine);
        }
        ImGui::PopStyleColor();

        ImGui::TextUnformatted("Total Frametime:    ");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.60f, 0.05f, 1.0f));
        if (override) {
            ImGui::Text("%6.2f ms", telemetry_.totalFrametimeMs);
        } else {
            ImGui::TextUnformatted(cachedStats_.totalFtLine);
        }
        ImGui::PopStyleColor();

        renderFrametimeGraph(scale, override);

        ImGui::Separator();

        // 3. Consolidated Hardware section (3 column-aligned rows via ImGui::BeginTable)
        if (ImGui::BeginTable("##PerfHudDetails", 2, ImGuiTableFlags_None)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Upscaler:  ").x);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("Upscaler:");
            ImGui::TableSetColumnIndex(1);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.4f, 1.0f));
            if (override) {
                ImGui::TextUnformatted(override->backendId >= 0 ? "Comparison Capture" : "Off (Before reconstruction)");
            } else {
                ImGui::TextUnformatted(cachedStats_.upscalerInfoLine);
            }
            ImGui::PopStyleColor();

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("GPU:");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(cachedStats_.gpuLine);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("VRAM:");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(cachedStats_.vramLine);

            ImGui::EndTable();
        }

        if (cfg.enableProfiler) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Profiler (120f avg):");
            ImGui::Text("  Map/Unmap (CB/Jitter): %6.3f ms", profilerDisplay_.mapMs);
            ImGui::Text("  OMSetRenderTargets:    %6.3f ms", profilerDisplay_.omMs);
            ImGui::Text("  Camera Delta Copy:     %6.3f ms", profilerDisplay_.deltaMs);
            ImGui::Text("  HUD Draw Probe:        %6.3f ms", profilerDisplay_.drawMs);
            ImGui::Text("  Upscale Prep & Eval:   %6.3f ms", profilerDisplay_.upscaleMs);
            ImGui::Text("  Unaccounted (Game):    %6.3f ms", profilerDisplay_.unaccountedMs);
        }
    }
    ImGui::End();
}

} // namespace edpe
