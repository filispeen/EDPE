#pragma once

#include <windows.h>
#include <cstdint>
#include <atomic>

namespace edpe {

enum class ProfileZone : uint32_t {
    MapUnmap = 0,
    OMSetRenderTargets,
    DrawProbe,
    TemporalUpscale,
    NvmlPoll,
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

    void onFrameEnd() {
        if (!isEnabled()) return;
        if (freq_.QuadPart == 0) {
            QueryPerformanceFrequency(&freq_);
            if (freq_.QuadPart == 0) return;
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
        }
    }

    float getAvgMs(ProfileZone zone) const {
        const uint32_t idx = static_cast<uint32_t>(zone);
        return idx < static_cast<uint32_t>(ProfileZone::Count) ? avgMs_[idx] : 0.0f;
    }

private:
    Profiler() {
        QueryPerformanceFrequency(&freq_);
    }

    std::atomic<bool> enabled_{true};
    LARGE_INTEGER freq_{};
    std::atomic<int64_t> currentTicks_[static_cast<uint32_t>(ProfileZone::Count)]{};
    int64_t windowTicks_[static_cast<uint32_t>(ProfileZone::Count)]{};
    float avgMs_[static_cast<uint32_t>(ProfileZone::Count)]{};
    uint32_t framesInWindow_{0};
};

class ScopedProfileTimer {
public:
    inline ScopedProfileTimer(ProfileZone zone) : zone_(zone) {
        if (Profiler::instance().isEnabled()) {
            QueryPerformanceCounter(&start_);
        }
    }

    inline ~ScopedProfileTimer() {
        if (start_.QuadPart > 0) {
            LARGE_INTEGER end;
            QueryPerformanceCounter(&end);
            Profiler::instance().accumulate(zone_, end.QuadPart - start_.QuadPart);
        }
    }

private:
    ProfileZone zone_;
    LARGE_INTEGER start_{};
};

} // namespace edpe
