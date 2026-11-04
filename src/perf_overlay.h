#pragma once

#include <d3d11.h>
#include <dxgi1_4.h>
#include <string>
#include <cstdint>
#include <array>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <imgui.h>

namespace edpe {

// Throttled (15 Hz / 66.67 ms) telemetry snapshot consumed by all text metrics in both overlays
struct CachedTelemetryStats {
    float fps = 0.0f;
    float onePercentLowFps = 0.0f;
    float gameFrametimeMs = 0.0f;
    float upscalerFrametimeMs = 0.0f;
    float totalFrametimeMs = 0.0f;

    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;

    std::wstring gpuName;
    uint32_t gpuUsagePct = 0;
    bool gpuLoadAvailable = false;

    uint64_t vramUsedBytes = 0;
    uint64_t vramTotalBytes = 0;

    // Pre-formatted strings (updated at 15 Hz / 66.67 ms) for zero-overhead per-frame rendering
    char fpsLine[64]{};
    char gameFtLine[64]{};
    char upscalerFtLine[64]{};
    char totalFtLine[64]{};
    char upscalerInfoLine[96]{};
    char gpuLine[96]{};
    char vramLine[64]{};
    char resLine[96]{};
    char reconStatusLine[96]{};
    ImVec4 fpsColor{0.2f, 1.0f, 0.2f, 1.0f};
    ImVec4 reconStatusColor{0.2f, 1.0f, 0.2f, 1.0f};
};

struct PerfTelemetry {
    float fps = 0.0f;
    float totalFrametimeMs = 0.0f;
    float gameFrametimeMs = 0.0f;
    float upscalerFrametimeMs = 0.0f;

    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;

    std::wstring gpuName;
    uint32_t gpuUsagePct = 0;
    bool gpuLoadAvailable = false;

    uint64_t vramUsedBytes = 0;
    uint64_t vramTotalBytes = 0;
};

// Replaces the live upscaler information in the HUD (used when stamping comparison images).
struct PerfOverlayOverride {
    int backendId = 0;        // 0 = none/before reconstruction, 1 = DLSS/DLAA, 2 = FSR, 3 = TAA
    float upscalerMs = 0.0f;  // GPU time of that backend
};

class PerfTracker {
public:
    static PerfTracker& instance();

    void onPresentStart(IDXGISwapChain* swapChain);
    void onPresentEnd();

    const PerfTelemetry& getTelemetry() const { return telemetry_; }
    const CachedTelemetryStats& getCachedTelemetry() const { return cachedStats_; }
    const CachedTelemetryStats& getCachedStats() const { return cachedStats_; }

    void renderOverlay(bool isMenuOpen, int corner, float scale = 1.0f, const PerfOverlayOverride* override = nullptr);
    void initNvml();
    void shutdown();
    void updateCachedStats();
    void refreshCachedStats() { updateCachedStats(); }

private:
    PerfTracker();
    ~PerfTracker();

    void updateTelemetry(IDXGISwapChain* swapChain);
    void renderFrametimeGraph(float scale, const PerfOverlayOverride* override = nullptr);
    float calculateOnePercentLow() const;

    PerfTelemetry telemetry_{};
    CachedTelemetryStats cachedStats_{};
    std::chrono::steady_clock::time_point lastCacheUpdate_{};

    struct ProfilerDisplaySnapshot {
        float mapMs = 0.0f;
        float omMs = 0.0f;
        float deltaMs = 0.0f;
        float drawMs = 0.0f;
        float upscaleMs = 0.0f;
        float unaccountedMs = 0.0f;
    };
    ProfilerDisplaySnapshot profilerDisplay_{};

    LARGE_INTEGER perfFreq_{};
    LARGE_INTEGER lastPresentStart_{};
    LARGE_INTEGER lastPresentEnd_{};
    LARGE_INTEGER currentPresentStart_{};

    float smoothedFrametimeMs_ = 16.67f;

    struct FrametimeSample {
        float gameMs = 0.0f;
        float upscalerMs = 0.0f;
        float totalMs = 0.0f;
    };
    static constexpr size_t kFrametimeHistorySize = 360;
    static constexpr size_t kGraphSampleCount = 120;
    std::array<FrametimeSample, kFrametimeHistorySize> frametimeHistory_{};
    size_t historyIndex_ = 0;
    size_t historyCount_ = 0;

    // Hardware telemetry handles
    bool gpuInfoCached_ = false;
    HMODULE nvmlModule_ = nullptr;
    void* nvmlDevice_ = nullptr;
    bool nvmlAvailable_ = false;

    // Dynamic NVML function pointer types
    typedef int (*NvmlInit_t)();
    typedef int (*NvmlShutdown_t)();
    typedef int (*NvmlGetHandle_t)(unsigned int, void**);
    typedef int (*NvmlGetName_t)(void*, char*, unsigned int);
    struct NvmlUtil_t { unsigned int gpu; unsigned int memory; };
    typedef int (*NvmlGetUtil_t)(void*, NvmlUtil_t*);
    struct NvmlMem_t { unsigned long long total; unsigned long long free; unsigned long long used; };
    typedef int (*NvmlGetMem_t)(void*, NvmlMem_t*);

    NvmlGetUtil_t nvmlGetUtil_ = nullptr;
    NvmlGetMem_t nvmlGetMem_ = nullptr;

    // Telemetry rate limiting
    LARGE_INTEGER lastTelemetryUpdate_{};
    std::thread nvmlWorkerThread_;
    std::atomic<bool> nvmlRunning_{false};
    std::atomic<uint32_t> cachedGpuUsage_{0};
    std::condition_variable nvmlCv_;
    std::mutex nvmlMutex_;
};

} // namespace edpe
