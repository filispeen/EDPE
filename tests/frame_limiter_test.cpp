#include "frame_limiter.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

int fail(int code, const char* msg) {
    std::fprintf(stderr, "frame_limiter_test FAILED (%d): %s\n", code, msg);
    return code;
}

struct MockClockState {
    int64_t ticks = 0;
    int64_t sleepCalls = 0;
    int64_t spinCalls = 0;
    int64_t lastSleepTicks = 0;
    int64_t spinStepTicks = 100; // 0.01 ms at 10 MHz
    int64_t extraSleepOvershootTicks = 0;
    std::vector<int64_t> sleepHistory;
};

int64_t mockNow(void* ctx) {
    return static_cast<MockClockState*>(ctx)->ticks;
}

void mockSleep(int64_t ticks, void* ctx) {
    auto* st = static_cast<MockClockState*>(ctx);
    st->ticks += ticks + st->extraSleepOvershootTicks;
    st->extraSleepOvershootTicks = 0;
    st->lastSleepTicks = ticks;
    ++st->sleepCalls;
    if (st->sleepHistory.size() < 4096) {
        st->sleepHistory.push_back(ticks);
    }
}

void mockSpin(void* ctx) {
    auto* st = static_cast<MockClockState*>(ctx);
    st->ticks += st->spinStepTicks;
    ++st->spinCalls;
}

edpe::FrameLimiterClockOps makeMockOps(MockClockState* st, bool highRes = true, int64_t freq = 10'000'000) {
    edpe::FrameLimiterClockOps ops{};
    ops.frequency = freq;
    ops.nowTicks = &mockNow;
    ops.sleepTicks = &mockSleep;
    ops.spinYield = &mockSpin;
    ops.highResTimer = highRes;
    ops.ctx = st;
    return ops;
}

} // namespace

int main() {
    using namespace edpe;

    // 1. Sanitize / clamp / disabled behavior.
    {
        if (FrameLimiter::sanitizeTargetFps(0.0f) != 0.0f) return fail(1, "0 fps sanitize");
        if (FrameLimiter::sanitizeTargetFps(-60.0f) != 0.0f) return fail(2, "negative fps sanitize");
        if (FrameLimiter::sanitizeTargetFps(std::numeric_limits<float>::quiet_NaN()) != 0.0f) return fail(3, "NaN fps sanitize");
        if (FrameLimiter::sanitizeTargetFps(std::numeric_limits<float>::infinity()) != 0.0f) return fail(4, "Inf fps sanitize");
        if (FrameLimiter::sanitizeTargetFps(-std::numeric_limits<float>::infinity()) != 0.0f) return fail(5, "-Inf fps sanitize");
        if (FrameLimiter::sanitizeTargetFps(5.0f) != FrameLimiter::kMinTargetFps) return fail(6, "min clamp");
        if (FrameLimiter::sanitizeTargetFps(999.0f) != FrameLimiter::kMaxTargetFps) return fail(7, "max clamp");
        if (FrameLimiter::sanitizeTargetFps(144.0f) != 144.0f) return fail(8, "valid 144 fps");

        MockClockState clock{};
        FrameLimiter lim(makeMockOps(&clock));
        lim.configure(0.0f);
        if (lim.isEnabled()) return fail(9, "disabled state mismatch");
        clock.ticks += 50'000;
        lim.waitForNextFrame();
        if (clock.sleepCalls != 0 || clock.spinCalls != 0 || lim.getStats().pacedFramesTotal != 0) {
            return fail(10, "disabled limiter must not sleep/spin/pace");
        }

        lim.configure(-10.0f);
        if (lim.isEnabled()) return fail(11, "negative fps must keep limiter disabled");
        lim.configure(std::numeric_limits<float>::quiet_NaN());
        if (lim.isEnabled()) return fail(12, "NaN fps must keep limiter disabled");
        lim.configure(5.0f);
        if (!lim.isEnabled() || lim.targetFps() != FrameLimiter::kMinTargetFps) {
            return fail(13, "clamped min fps mismatch");
        }
        lim.configure(999.0f);
        if (!lim.isEnabled() || lim.targetFps() != FrameLimiter::kMaxTargetFps) {
            return fail(14, "clamped max fps mismatch");
        }
    }

    // 2. Deterministic mock-clock steady 60 FPS over 1000 frames:
    // zero sleep on frame 1, zero cumulative drift across 1000 frames.
    {
        MockClockState clock{};
        clock.spinStepTicks = 10; // 1 us granularity
        FrameLimiter lim(makeMockOps(&clock, true, 10'000'000));
        lim.configure(60.0f);

        clock.ticks += 50'000; // 5 ms render for frame 1
        lim.waitForNextFrame();
        const int64_t frame1Exit = clock.ticks;
        if (clock.sleepCalls != 0 || clock.spinCalls != 0 || frame1Exit != 50'000) {
            return fail(20, "frame 1 must anchor with zero sleep/spin");
        }

        for (int i = 1; i < 1000; ++i) {
            clock.ticks += 50'000; // 5 ms render work
            lim.waitForNextFrame();
        }
        const int64_t elapsed = clock.ticks - frame1Exit;
        const int64_t expected = static_cast<int64_t>(std::llround(999.0 * 10'000'000.0 / 60.0));
        if (std::llabs(elapsed - expected) > 20) {
            return fail(21, "60 FPS 1000-frame cumulative drift exceeded 2 us");
        }
        const auto s = lim.getStats();
        if (s.pacedFramesTotal != 1000 || s.overrunFramesTotal != 0 || clock.sleepCalls != 999) {
            return fail(22, "60 FPS 1000-frame counters mismatch");
        }
    }

    // 3. Fractional target FPS (59.94 FPS) accuracy over 1000 frames.
    {
        MockClockState clock{};
        clock.spinStepTicks = 10;
        FrameLimiter lim(makeMockOps(&clock, true, 10'000'000));
        const float target = 59.94f;
        lim.configure(target);

        clock.ticks += 50'000;
        lim.waitForNextFrame();
        const int64_t frame1Exit = clock.ticks;

        for (int i = 1; i < 1000; ++i) {
            clock.ticks += 50'000;
            lim.waitForNextFrame();
        }
        const int64_t elapsed = clock.ticks - frame1Exit;
        const int64_t expected = static_cast<int64_t>(std::llround(999.0 * 10'000'000.0 / static_cast<double>(target)));
        if (std::llabs(elapsed - expected) > 20) {
            return fail(30, "59.94 FPS 1000-frame drift exceeded 2 us");
        }
    }

    // 4. Async non-catchup overrun: 35 ms stutter frame sleeps 0 ms, increments overrun,
    // and next 6 ms frame paces cleanly to 16.67 ms (no burst catch-up).
    {
        MockClockState clock{};
        clock.spinStepTicks = 10;
        FrameLimiter lim(makeMockOps(&clock, true, 10'000'000));
        lim.configure(60.0f);

        // Frame 1: anchor
        clock.ticks += 60'000;
        lim.waitForNextFrame();

        // Frame 2: normal 6 ms render -> paces to 16.67 ms
        clock.ticks += 60'000;
        lim.waitForNextFrame();
        if (clock.sleepCalls != 1 || lim.getStats().overrunFramesTotal != 0) {
            return fail(40, "normal pre-stutter frame failed");
        }

        // Frame 3: 35 ms stutter
        const int64_t sleepsBeforeStutter = clock.sleepCalls;
        clock.ticks += 350'000;
        const int64_t preWaitTick = clock.ticks;
        lim.waitForNextFrame();
        const auto s3 = lim.getStats();
        if (clock.sleepCalls != sleepsBeforeStutter || clock.ticks != preWaitTick ||
            !s3.lastFrameOverrun || s3.lastWaitMs != 0.0f || s3.overrunFramesTotal != 1) {
            return fail(41, "stutter overrun must not sleep and must increment overrun count");
        }

        // Frame 4: 6 ms render -> must wait ~10.67 ms (total frame 4 = 16.67 ms), not burst
        clock.ticks += 60'000;
        lim.waitForNextFrame();
        const auto s4 = lim.getStats();
        if (clock.sleepCalls != sleepsBeforeStutter + 1 || s4.lastFrameOverrun ||
            s4.lastWaitMs < 10.5f || s4.lastWaitMs > 10.8f || s4.overrunFramesTotal != 1) {
            return fail(42, "post-overrun recovery frame did not pace cleanly");
        }
    }

    // 5. Severe OS sleep overshoot (> 0.5 * period): re-anchors without early next frame.
    {
        MockClockState clock{};
        clock.spinStepTicks = 10;
        FrameLimiter lim(makeMockOps(&clock, true, 10'000'000));
        lim.configure(60.0f);

        // Frame 1: anchor
        clock.ticks += 50'000;
        lim.waitForNextFrame();

        // Frame 2: OS stalls during sleep by +20 ms (> 0.5 * 16.67 ms)
        clock.ticks += 50'000;
        clock.extraSleepOvershootTicks = 200'000;
        lim.waitForNextFrame();
        const int64_t afterFrame2 = clock.ticks;

        // Frame 3: 5 ms render -> must still pace ~11.67 ms from afterFrame2!
        const int64_t sleepsBefore = clock.sleepCalls;
        clock.ticks += 50'000;
        lim.waitForNextFrame();
        const auto s3 = lim.getStats();
        const int64_t frame3TotalTicks = clock.ticks - afterFrame2;
        if (clock.sleepCalls != sleepsBefore + 1 || s3.lastFrameOverrun ||
            s3.lastWaitMs < 11.5f || s3.lastWaitMs > 11.8f ||
            std::llabs(frame3TotalTicks - 166'667) > 30) {
            return fail(50, "severe sleep overshoot did not re-anchor next frame");
        }
    }

    // 6. Dynamic target change (30 -> 120 FPS) and reset() re-anchoring.
    {
        MockClockState clock{};
        clock.spinStepTicks = 10;
        FrameLimiter lim(makeMockOps(&clock, true, 10'000'000));
        lim.configure(30.0f);

        clock.ticks += 50'000;
        lim.waitForNextFrame(); // anchor
        for (int i = 0; i < 4; ++i) {
            clock.ticks += 50'000;
            lim.waitForNextFrame();
        }
        const auto s30 = lim.getStats();
        if (s30.lastWaitMs < 28.0f || s30.lastWaitMs > 28.6f) {
            return fail(60, "30 FPS wait time mismatch");
        }

        // Switch to 120 FPS: transition frame must re-anchor with 0 sleep.
        lim.configure(120.0f);
        const int64_t sleepsAtSwitch = clock.sleepCalls;
        clock.ticks += 20'000;
        lim.waitForNextFrame();
        if (clock.sleepCalls != sleepsAtSwitch) {
            return fail(61, "configure target change must re-anchor without sleeping");
        }
        clock.ticks += 20'000; // 2 ms render at 120 FPS (8.33 ms period)
        lim.waitForNextFrame();
        const auto s120 = lim.getStats();
        if (s120.lastWaitMs < 6.1f || s120.lastWaitMs > 6.6f) {
            return fail(62, "120 FPS wait time mismatch");
        }

        // reset() must re-anchor next frame cleanly.
        lim.reset();
        const int64_t sleepsAtReset = clock.sleepCalls;
        clock.ticks += 20'000;
        lim.waitForNextFrame();
        if (clock.sleepCalls != sleepsAtReset) {
            return fail(63, "reset() must re-anchor without sleeping");
        }
        clock.ticks += 20'000;
        lim.waitForNextFrame();
        const auto sAfterReset = lim.getStats();
        if (sAfterReset.lastWaitMs < 6.1f || sAfterReset.lastWaitMs > 6.6f) {
            return fail(64, "post-reset 120 FPS wait time mismatch");
        }
    }

    // 7. Periodic rebase across > 65536 simulated frames with zero drift.
    {
        MockClockState clock{};
        clock.spinStepTicks = 100; // 10 us granularity for fast 70k-frame loop
        FrameLimiter lim(makeMockOps(&clock, true, 10'000'000));
        lim.configure(60.0f);

        clock.ticks += 50'000;
        lim.waitForNextFrame();
        const int64_t startTick = clock.ticks;

        constexpr int kFrames = 70'000;
        for (int i = 1; i < kFrames; ++i) {
            clock.ticks += 50'000;
            lim.waitForNextFrame();
        }
        const int64_t elapsed = clock.ticks - startTick;
        const int64_t expected = static_cast<int64_t>(
            std::llround(static_cast<double>(kFrames - 1) * 10'000'000.0 / 60.0));
        if (std::llabs(elapsed - expected) > 200) {
            return fail(70, "70k-frame periodic rebase drift exceeded 0.02 ms");
        }
        if (lim.getStats().pacedFramesTotal != static_cast<uint64_t>(kFrames)) {
            return fail(71, "70k-frame paced count mismatch");
        }
    }

    // 8. Sleep-vs-spin split verification.
    {
        // 8a: High-res timer (1.5 ms spin margin).
        MockClockState clock{};
        clock.spinStepTicks = 10;
        FrameLimiter limHr(makeMockOps(&clock, true, 10'000'000));
        limHr.configure(60.0f);

        clock.ticks += 50'000;
        limHr.waitForNextFrame(); // anchor

        // Remaining = 11.6667 ms -> sleep = 10.1667 ms >= 0.5 ms -> sleeps once.
        clock.ticks += 50'000;
        limHr.waitForNextFrame();
        if (clock.sleepCalls != 1 || clock.lastSleepTicks < 101'000 || clock.lastSleepTicks > 102'000 ||
            clock.spinCalls == 0) {
            return fail(80, "high-res sleep+spin split mismatch");
        }

        // Remaining = 1.8667 ms <= 1.5 + 0.5 ms -> zero kernel sleep, spin only.
        clock.ticks += 148'000;
        const int64_t prevSleeps = clock.sleepCalls;
        const int64_t prevSpins = clock.spinCalls;
        limHr.waitForNextFrame();
        if (clock.sleepCalls != prevSleeps || clock.spinCalls <= prevSpins) {
            return fail(81, "high-res near-deadline must skip kernel sleep and spin");
        }

        // 8b: Fallback timer (2.5 ms spin margin).
        MockClockState clockFb{};
        clockFb.spinStepTicks = 10;
        FrameLimiter limFb(makeMockOps(&clockFb, false, 10'000'000));
        limFb.configure(60.0f);

        clockFb.ticks += 50'000;
        limFb.waitForNextFrame(); // anchor

        clockFb.ticks += 50'000;
        limFb.waitForNextFrame();
        if (clockFb.sleepCalls != 1 || clockFb.lastSleepTicks < 91'000 || clockFb.lastSleepTicks > 92'000 ||
            clockFb.spinCalls == 0) {
            return fail(82, "fallback sleep+spin split mismatch");
        }

        clockFb.ticks += 138'000; // remaining = 2.8667 ms <= 2.5 + 0.5 ms
        const int64_t prevFbSleeps = clockFb.sleepCalls;
        const int64_t prevFbSpins = clockFb.spinCalls;
        limFb.waitForNextFrame();
        if (clockFb.sleepCalls != prevFbSleeps || clockFb.spinCalls <= prevFbSpins) {
            return fail(83, "fallback near-deadline must skip kernel sleep and spin");
        }
    }

    // 9. Live Win32 QPC + waitable timer pacing test at 120 FPS for 20 frames.
    {
        FrameLimiter liveLimiter;
        liveLimiter.configure(120.0f);

        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);

        liveLimiter.waitForNextFrame(); // frame 1 anchor
        LARGE_INTEGER t0{};
        QueryPerformanceCounter(&t0);

        for (int i = 1; i < 20; ++i) {
            liveLimiter.waitForNextFrame();
        }
        LARGE_INTEGER t1{};
        QueryPerformanceCounter(&t1);

        const double avgMs =
            (static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart)) / 19.0;
        const auto s = liveLimiter.getStats();
        if (avgMs < 7.5 || avgMs > 9.5) {
            std::fprintf(stderr, "live avgMs=%.3f\n", avgMs);
            return fail(90, "live 120 FPS pacing average out of [7.5, 9.5] ms");
        }
        if (s.lastWaitMs <= 0.5f) {
            std::fprintf(stderr, "live lastWaitMs=%.3f\n", s.lastWaitMs);
            return fail(91, "live 120 FPS lastWaitMs should be > 0.5 ms");
        }

        liveLimiter.shutdown();
        if (liveLimiter.isEnabled() || liveLimiter.targetFps() != 0.0f) {
            return fail(92, "shutdown must leave limiter disabled");
        }
        // Safe to re-configure after shutdown.
        liveLimiter.configure(120.0f);
        liveLimiter.waitForNextFrame();
        liveLimiter.shutdown();
    }

    std::printf("frame_limiter_test PASSED\n");
    return 0;
}
