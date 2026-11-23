#include "frame_limiter.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace edpe {

namespace {

int64_t queryQpcNow() {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

int64_t queryQpcFreq() {
    LARGE_INTEGER li{};
    QueryPerformanceFrequency(&li);
    return li.QuadPart;
}

} // namespace

FrameLimiter& FrameLimiter::instance() {
    static FrameLimiter s_limiter;
    return s_limiter;
}

FrameLimiter::FrameLimiter()
    : useCustomOps_(false)
    , qpcFrequency_(queryQpcFreq()) {}

FrameLimiter::FrameLimiter(const FrameLimiterClockOps& customOps)
    : useCustomOps_(true)
    , ops_(customOps)
    , qpcFrequency_(customOps.frequency) {
    highResTimer_.store(customOps.highResTimer, std::memory_order_relaxed);
}

FrameLimiter::~FrameLimiter() {
    shutdown();
}

float FrameLimiter::sanitizeTargetFps(float fps) {
    if (!std::isfinite(fps) || fps <= 0.0f) {
        return 0.0f;
    }
    return std::clamp(fps, kMinTargetFps, kMaxTargetFps);
}

void FrameLimiter::configure(bool enabled, float targetFps) {
    const float cleanFps = sanitizeTargetFps(targetFps);
    const bool cleanEnabled = enabled && (cleanFps >= kMinTargetFps);

    const bool prevEnabled = enabled_.exchange(cleanEnabled, std::memory_order_acq_rel);
    const float prevFps = targetFps_.exchange(cleanFps, std::memory_order_acq_rel);

    const bool changed = (prevEnabled != cleanEnabled) || (std::fabs(prevFps - cleanFps) > 1e-4f);
    if (changed) {
        resetGeneration_.fetch_add(1, std::memory_order_release);
        if (!cleanEnabled) {
            lastWaitMs_.store(0.0f, std::memory_order_relaxed);
            lastFrameOverrun_.store(false, std::memory_order_relaxed);
        }
        if (!useCustomOps_ && (prevEnabled != cleanEnabled || cleanEnabled)) {
            wchar_t msg[160]{};
            if (cleanEnabled) {
                std::swprintf(msg, 160,
                    L"EDPE: async Game FPS limiter enabled (target=%.1f FPS)",
                    cleanFps);
            } else {
                std::swprintf(msg, 160, L"EDPE: async Game FPS limiter disabled");
            }
            EdpeLog(msg);
        }
    }
}

void FrameLimiter::reset() {
    resetGeneration_.fetch_add(1, std::memory_order_release);
}

void FrameLimiter::shutdown() {
    enabled_.store(false, std::memory_order_release);
    resetGeneration_.fetch_add(1, std::memory_order_release);
    anchored_ = false;
    anchorTick_ = 0;
    frameStep_ = 1;
    lastGeneration_ = 0;
    lastWaitMs_.store(0.0f, std::memory_order_relaxed);
    lastFrameOverrun_.store(false, std::memory_order_relaxed);

    if (!useCustomOps_) {
        std::lock_guard<std::mutex> lock(timerMutex_);
        HANDLE h = static_cast<HANDLE>(timerHandle_.exchange(nullptr, std::memory_order_acq_rel));
        timerInitAttempted_.store(false, std::memory_order_release);
        highResTimer_.store(false, std::memory_order_release);
        if (h) {
            CloseHandle(h);
        }
    }
}

int64_t FrameLimiter::currentNowTicks() const {
    if (useCustomOps_) {
        return ops_.nowTicks ? ops_.nowTicks(ops_.ctx) : 0;
    }
    return queryQpcNow();
}

int64_t FrameLimiter::queryFrequency() const {
    return useCustomOps_ ? ops_.frequency : qpcFrequency_;
}

void* FrameLimiter::ensureTimerHandle() {
    if (useCustomOps_) {
        return nullptr;
    }
    if (timerInitAttempted_.load(std::memory_order_acquire)) {
        return timerHandle_.load(std::memory_order_acquire);
    }

    std::lock_guard<std::mutex> lock(timerMutex_);
    if (!timerInitAttempted_.load(std::memory_order_relaxed)) {
        HANDLE h = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        bool highRes = (h != nullptr);
        if (!h) {
            h = CreateWaitableTimerW(nullptr, FALSE, nullptr);
            highRes = false;
        }
        timerHandle_.store(h, std::memory_order_release);
        highResTimer_.store(highRes, std::memory_order_release);
        timerInitAttempted_.store(true, std::memory_order_release);
    }
    return timerHandle_.load(std::memory_order_acquire);
}

void FrameLimiter::waitForNextFrame() {
    if (!enabled_.load(std::memory_order_acquire)) {
        return;
    }

    const float targetFps = targetFps_.load(std::memory_order_acquire);
    if (!std::isfinite(targetFps) || targetFps < kMinTargetFps) {
        return;
    }

    const int64_t freq = queryFrequency();
    if (freq <= 0) {
        return;
    }

    const uint64_t gen = resetGeneration_.load(std::memory_order_acquire);
    HANDLE timer = static_cast<HANDLE>(ensureTimerHandle());

    const int64_t now = currentNowTicks();
    if (!anchored_ || lastGeneration_ != gen) {
        anchorTick_ = now;
        frameStep_ = 1;
        lastGeneration_ = gen;
        anchored_ = true;
        lastWaitMs_.store(0.0f, std::memory_order_relaxed);
        lastFrameOverrun_.store(false, std::memory_order_relaxed);
        pacedFramesTotal_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const double periodTicks = static_cast<double>(freq) / static_cast<double>(targetFps);
    const int64_t deadlineOffset = static_cast<int64_t>(
        std::llround(static_cast<double>(frameStep_) * periodTicks));
    const int64_t deadlineTick = anchorTick_ + deadlineOffset;
    const int64_t remainingTicks = deadlineTick - now;

    // Async non-catchup overrun: frame missed its budget, so skip waiting and re-anchor
    // from now rather than bursting subsequent frames.
    if (remainingTicks <= 0) {
        anchorTick_ = now;
        frameStep_ = 1;
        lastWaitMs_.store(0.0f, std::memory_order_relaxed);
        lastFrameOverrun_.store(true, std::memory_order_relaxed);
        overrunFramesTotal_.fetch_add(1, std::memory_order_relaxed);
        pacedFramesTotal_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Reject implausibly large gaps (> 1 s) such as clock discontinuities.
    if (remainingTicks > freq) {
        anchorTick_ = now;
        frameStep_ = 1;
        lastWaitMs_.store(0.0f, std::memory_order_relaxed);
        lastFrameOverrun_.store(false, std::memory_order_relaxed);
        pacedFramesTotal_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const bool highRes = useCustomOps_ ? ops_.highResTimer : highResTimer_.load(std::memory_order_relaxed);
    const double spinMarginMs = highRes ? kHighResSpinMarginMs : kFallbackSpinMarginMs;
    const double ticksPerMs = static_cast<double>(freq) / 1000.0;

    // Passive kernel sleep phase for (remaining - spinMargin), bounded to 4 retries.
    for (int attempt = 0; attempt < 4; ++attempt) {
        const int64_t curNow = currentNowTicks();
        const int64_t curRemaining = deadlineTick - curNow;
        if (curRemaining <= 0) {
            break;
        }
        const double curRemainingMs = static_cast<double>(curRemaining) / ticksPerMs;
        const double curSleepMs = curRemainingMs - spinMarginMs;
        if (curSleepMs < kMinKernelSleepMs) {
            break;
        }

        if (useCustomOps_) {
            if (!ops_.sleepTicks) {
                break;
            }
            const int64_t sleepTickCount = static_cast<int64_t>(std::llround(curSleepMs * ticksPerMs));
            if (sleepTickCount <= 0) {
                break;
            }
            ops_.sleepTicks(sleepTickCount, ops_.ctx);
        } else {
            // Re-read handle in case shutdown() closed it concurrently.
            timer = timerHandle_.load(std::memory_order_acquire);
            if (timer) {
                const int64_t due100Ns = -static_cast<int64_t>(std::llround(curSleepMs * 10000.0));
                if (due100Ns >= 0) {
                    break;
                }
                LARGE_INTEGER dueTime{};
                dueTime.QuadPart = due100Ns;
                if (SetWaitableTimer(timer, &dueTime, 0, nullptr, nullptr, FALSE)) {
                    const DWORD timeoutMs = static_cast<DWORD>(std::ceil(curSleepMs)) + 50u;
                    WaitForSingleObject(timer, timeoutMs);
                } else {
                    const DWORD ms = static_cast<DWORD>(curSleepMs);
                    if (ms == 0) break;
                    Sleep(ms);
                }
            } else {
                const DWORD ms = static_cast<DWORD>(curSleepMs);
                if (ms == 0) break;
                Sleep(ms);
            }
        }

        const int64_t afterSleep = currentNowTicks();
        if (afterSleep <= curNow) {
            break;
        }
    }

    // Precision spin finish for the remaining <= spinMargin window.
    int64_t wakeNow = currentNowTicks();
    while (wakeNow < deadlineTick) {
        const int64_t prevTick = wakeNow;
        if (useCustomOps_) {
            if (ops_.spinYield) {
                ops_.spinYield(ops_.ctx);
            }
        } else {
            YieldProcessor();
        }
        wakeNow = currentNowTicks();
        if (wakeNow <= prevTick) {
            break;
        }
    }

    const double waitedMs = static_cast<double>(wakeNow - now) / ticksPerMs;
    lastWaitMs_.store(waitedMs > 0.0 ? static_cast<float>(waitedMs) : 0.0f, std::memory_order_relaxed);
    lastFrameOverrun_.store(false, std::memory_order_relaxed);
    pacedFramesTotal_.fetch_add(1, std::memory_order_relaxed);

    const int64_t overshootTicks = wakeNow - deadlineTick;
    if (static_cast<double>(overshootTicks) > 0.5 * periodTicks) {
        anchorTick_ = wakeNow;
        frameStep_ = 1;
    } else if (frameStep_ >= kRebaseFrameInterval) {
        anchorTick_ = deadlineTick;
        frameStep_ = 1;
    } else {
        ++frameStep_;
    }
}

FrameLimiterStats FrameLimiter::getStats() const {
    FrameLimiterStats s{};
    s.enabled = enabled_.load(std::memory_order_acquire);
    s.targetFps = targetFps_.load(std::memory_order_acquire);
    s.targetFrameTimeMs = (s.targetFps > 0.0f) ? (1000.0f / s.targetFps) : 0.0f;
    s.lastWaitMs = lastWaitMs_.load(std::memory_order_relaxed);
    s.lastFrameOverrun = lastFrameOverrun_.load(std::memory_order_relaxed);
    s.highResTimer = highResTimer_.load(std::memory_order_relaxed);
    s.pacedFramesTotal = pacedFramesTotal_.load(std::memory_order_relaxed);
    s.overrunFramesTotal = overrunFramesTotal_.load(std::memory_order_relaxed);
    return s;
}

} // namespace edpe
