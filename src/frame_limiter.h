#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace edpe {

// Snapshot of the asynchronous post-Present Game FPS limiter state for UI and telemetry.
struct FrameLimiterStats {
    bool enabled = false;
    float targetFps = 0.0f;
    float targetFrameTimeMs = 0.0f;
    float lastWaitMs = 0.0f;
    bool lastFrameOverrun = false;
    bool highResTimer = false;
    uint64_t pacedFramesTotal = 0;
    uint64_t overrunFramesTotal = 0;
};

// Optional clock injection callbacks for deterministic unit testing.
struct FrameLimiterClockOps {
    int64_t frequency = 0;
    int64_t (*nowTicks)(void* ctx) = nullptr;
    void (*sleepTicks)(int64_t ticks, void* ctx) = nullptr;
    void (*spinYield)(void* ctx) = nullptr;
    bool highResTimer = false;
    void* ctx = nullptr;
};

// Asynchronous CPU-side Game FPS limiter.
// Invoked at the tail of Present() so the GPU and any Frame Generation pass work on frame N
// while the game thread sleeps until the next frame N+1 input/simulation boundary.
class FrameLimiter {
public:
    static constexpr float kMinTargetFps = 10.0f;
    static constexpr float kMaxTargetFps = 500.0f;
    static constexpr float kDefaultTargetFps = 60.0f;
    static constexpr double kHighResSpinMarginMs = 1.5;
    static constexpr double kFallbackSpinMarginMs = 2.5;
    static constexpr double kMinKernelSleepMs = 0.5;
    static constexpr uint64_t kRebaseFrameInterval = 65536ull;

    static FrameLimiter& instance();

    FrameLimiter();
    explicit FrameLimiter(const FrameLimiterClockOps& customOps);
    ~FrameLimiter();

    FrameLimiter(const FrameLimiter&) = delete;
    FrameLimiter& operator=(const FrameLimiter&) = delete;

    // Clamps valid target FPS to [kMinTargetFps, kMaxTargetFps]; non-finite or <= 0 returns 0.0f.
    static float sanitizeTargetFps(float fps);

    // Updates limiter enabled state and target FPS; thread-safe and lock-free for callers.
    void configure(bool enabled, float targetFps);

    // Invalidates the current pacing phase anchor (e.g. on swapchain resize or teardown).
    void reset();

    // Releases the owned Win32 timer handle and clears pacing state.
    void shutdown();

    // Sleeps/spins after Present() to pace rendered Game FPS without catch-up bursts on stutters.
    void waitForNextFrame();

    bool isEnabled() const { return enabled_.load(std::memory_order_acquire); }
    float targetFps() const { return targetFps_.load(std::memory_order_acquire); }
    FrameLimiterStats getStats() const;

private:
    int64_t currentNowTicks() const;
    int64_t queryFrequency() const;
    void* ensureTimerHandle();

    bool useCustomOps_ = false;
    FrameLimiterClockOps ops_{};
    int64_t qpcFrequency_ = 0;

    std::atomic<bool> enabled_{false};
    std::atomic<float> targetFps_{kDefaultTargetFps};
    std::atomic<uint64_t> resetGeneration_{1};

    std::atomic<float> lastWaitMs_{0.0f};
    std::atomic<bool> lastFrameOverrun_{false};
    std::atomic<bool> highResTimer_{false};
    std::atomic<uint64_t> pacedFramesTotal_{0};
    std::atomic<uint64_t> overrunFramesTotal_{0};

    // Pacing phase state owned by the render/present thread.
    bool anchored_ = false;
    int64_t anchorTick_ = 0;
    uint64_t frameStep_ = 1;
    uint64_t lastGeneration_ = 0;

    std::mutex timerMutex_;
    std::atomic<bool> timerInitAttempted_{false};
    std::atomic<void*> timerHandle_{nullptr};
};

} // namespace edpe
