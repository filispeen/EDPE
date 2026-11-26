#pragma once

#include <windows.h>
#include <cstdint>
#include <atomic>

namespace edpe {

enum class ProfileZone : uint32_t {
    MapUnmap = 0,
    OMSetRenderTargets,
    CameraDeltaCopy,
    DrawProbe,
    TemporalUpscale,
    Count
};

enum class GpuProfileZone : uint32_t {
    DepthCopy = 0,
    MotionPass,
    ResidualFlow,
    ColorPrep,
    UpscaleEval,
    UiComposite,
    HudBloom,
    Count
};

class Profiler {
public:
    static Profiler& instance() {
        static Profiler s_instance;
        return s_instance;
    }

    void setEnabled(bool enabled) {
        enabled_.store(enabled, std::memory_order_relaxed);
    }

    bool isEnabled() const {
        return enabled_.load(std::memory_order_relaxed);
    }

    void accumulate(ProfileZone zone, int64_t ticks) {
        if (!isEnabled()) return;
        currentTicks_[static_cast<uint32_t>(zone)].fetch_add(ticks, std::memory_order_relaxed);
    }

    void submitGpuSample(const double (&zoneMs)[static_cast<uint32_t>(GpuProfileZone::Count)]) {
        if (!isEnabled()) return;
        const uint32_t gpuCount = static_cast<uint32_t>(GpuProfileZone::Count);
        for (uint32_t i = 0; i < gpuCount; ++i) {
            gpuWindowSumMs_[i] += zoneMs[i];
        }
        ++gpuWindowSamples_;
    }

    void setDirectColorStatus(bool active, uint32_t format) {
        directColorActive_.store(active, std::memory_order_relaxed);
        directColorFormat_.store(format, std::memory_order_relaxed);
    }

    bool isDirectColorActive() const {
        return directColorActive_.load(std::memory_order_relaxed);
    }

    uint32_t getDirectColorFormat() const {
        return directColorFormat_.load(std::memory_order_relaxed);
    }

    bool onFrameEnd() {
        if (!isEnabled()) return false;
        if (freq_.QuadPart == 0) {
            QueryPerformanceFrequency(&freq_);
            if (freq_.QuadPart == 0) return false;
        }

        const uint32_t count = static_cast<uint32_t>(ProfileZone::Count);
        for (uint32_t i = 0; i < count; ++i) {
            windowTicks_[i] += currentTicks_[i].exchange(0, std::memory_order_relaxed);
        }

        ++framesInWindow_;
        if (framesInWindow_ >= 120) {
            const double divisor = static_cast<double>(freq_.QuadPart) * static_cast<double>(framesInWindow_);
            for (uint32_t i = 0; i < count; ++i) {
                avgMs_[i] = static_cast<float>((static_cast<double>(windowTicks_[i]) * 1000.0) / divisor);
                windowTicks_[i] = 0;
            }
            framesInWindow_ = 0;

            const uint32_t gpuCount = static_cast<uint32_t>(GpuProfileZone::Count);
            if (gpuWindowSamples_ > 0) {
                const double invSamples = 1.0 / static_cast<double>(gpuWindowSamples_);
                for (uint32_t i = 0; i < gpuCount; ++i) {
                    gpuAvgMs_[i] = static_cast<float>(gpuWindowSumMs_[i] * invSamples);
                    gpuWindowSumMs_[i] = 0.0;
                }
                gpuWindowSamples_ = 0;
            } else {
                for (uint32_t i = 0; i < gpuCount; ++i) {
                    gpuAvgMs_[i] = 0.0f;
                    gpuWindowSumMs_[i] = 0.0;
                }
            }
            return true;
        }
        return false;
    }

    float getAvgMs(ProfileZone zone) const {
        const uint32_t idx = static_cast<uint32_t>(zone);
        return idx < static_cast<uint32_t>(ProfileZone::Count) ? avgMs_[idx] : 0.0f;
    }

    float getGpuAvgMs(GpuProfileZone zone) const {
        const uint32_t idx = static_cast<uint32_t>(zone);
        return idx < static_cast<uint32_t>(GpuProfileZone::Count) ? gpuAvgMs_[idx] : 0.0f;
    }

private:
    Profiler() {
        QueryPerformanceFrequency(&freq_);
    }

    std::atomic<bool> enabled_{false};
    std::atomic<bool> directColorActive_{false};
    std::atomic<uint32_t> directColorFormat_{0};
    LARGE_INTEGER freq_{};
    std::atomic<int64_t> currentTicks_[static_cast<uint32_t>(ProfileZone::Count)]{};
    int64_t windowTicks_[static_cast<uint32_t>(ProfileZone::Count)]{};
    float avgMs_[static_cast<uint32_t>(ProfileZone::Count)]{};
    double gpuWindowSumMs_[static_cast<uint32_t>(GpuProfileZone::Count)]{};
    float gpuAvgMs_[static_cast<uint32_t>(GpuProfileZone::Count)]{};
    uint32_t gpuWindowSamples_{0};
    uint32_t framesInWindow_{0};
};

class ScopedProfileTimer {
public:
    inline ScopedProfileTimer(ProfileZone zone, int64_t multiplier = 1) : zone_(zone), multiplier_(multiplier) {
        if (Profiler::instance().isEnabled()) {
            QueryPerformanceCounter(&start_);
        }
    }

    inline ~ScopedProfileTimer() {
        if (start_.QuadPart > 0) {
            LARGE_INTEGER end;
            QueryPerformanceCounter(&end);
            Profiler::instance().accumulate(zone_, (end.QuadPart - start_.QuadPart) * multiplier_);
        }
    }

private:
    ProfileZone zone_;
    int64_t multiplier_{1};
    LARGE_INTEGER start_{};
};

}
