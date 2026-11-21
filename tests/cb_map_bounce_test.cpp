#include "cb_map_bounce.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int fail(int code, const char* msg) {
    std::fprintf(stderr, "cb_map_bounce_test FAILED (%d): %s\n", code, msg);
    return code;
}

} // namespace

int main() {
    using namespace edpe;

    // 1. Mode::Off never bounces, even under Wine.
    {
        CbMapBounce bounce;
        bounce.configure(MapBounceMode::Off, true);
        if (bounce.isActive()) return fail(1, "Mode::Off should not be active");

        std::array<unsigned char, 5376> realBuf{};
        void* p = bounce.onMap(0x1001, 0xA001, realBuf.data(), 5376, true, 1);
        if (p != realBuf.data()) return fail(2, "Mode::Off should pass through realPtr");
        if (bounce.openSlotCount() != 0) return fail(3, "Mode::Off should not allocate slot");
        if (bounce.onUnmap(0x1001, 0xA001)) return fail(4, "Mode::Off onUnmap should return false");
    }

    // 2. Mode::Auto under Wine activates immediately; non-discard and ineligible widths pass through.
    {
        CbMapBounce bounce;
        bounce.configure(MapBounceMode::Auto, true);
        if (!bounce.isActive()) return fail(5, "Mode::Auto under Wine should be active immediately");

        std::array<unsigned char, 5376> realBuf{};
        // Non-WRITE_DISCARD must pass through untouched.
        void* pNoDiscard = bounce.onMap(0x1002, 0xA001, realBuf.data(), 5376, false, 1);
        if (pNoDiscard != realBuf.data()) return fail(6, "Non-WRITE_DISCARD should pass through");

        // Ineligible width (e.g. 64 bytes or 8192 bytes) must pass through untouched.
        std::array<unsigned char, 8192> bigBuf{};
        void* pSmall = bounce.onMap(0x1003, 0xA001, bigBuf.data(), 64, true, 1);
        if (pSmall != bigBuf.data()) return fail(7, "Ineligible 64 B width should pass through");
        void* pLarge = bounce.onMap(0x1004, 0xA001, bigBuf.data(), 8192, true, 1);
        if (pLarge != bigBuf.data()) return fail(8, "Ineligible 8192 B width should pass through");

        // Eligible widths (208, 272..4096, 5376) with WRITE_DISCARD bounce and flush sequentially.
        std::array<unsigned char, 208> real208{};
        std::memset(real208.data(), 0xAA, real208.size());
        void* bounced = bounce.onMap(0x2001, 0xA001, real208.data(), 208, true, 1);
        if (bounced == real208.data()) return fail(9, "Eligible 208 B WRITE_DISCARD should bounce");
        if ((reinterpret_cast<uintptr_t>(bounced) % 64u) != 0u) {
            return fail(10, "Bounced slot must be 64-byte aligned");
        }
        // Write a pattern into the bounced slot, verify realBuf untouched until Unmap.
        for (size_t i = 0; i < 208; ++i) {
            static_cast<unsigned char*>(bounced)[i] = static_cast<unsigned char>(i ^ 0x5A);
        }
        if (real208[0] != 0xAA) return fail(11, "Real buffer should not be modified before onUnmap");

        if (!bounce.onUnmap(0x2001, 0xA001)) return fail(12, "onUnmap should return true for bounced slot");
        for (size_t i = 0; i < 208; ++i) {
            if (real208[i] != static_cast<unsigned char>(i ^ 0x5A)) {
                return fail(13, "Sequential flush to real buffer did not match bounced data");
            }
        }
        if (bounce.openSlotCount() != 0) return fail(14, "Slot should be released after onUnmap");
        if (!bounce.onPresent(1)) return fail(15, "onPresent should succeed when no slots are open");
    }

    // 3. Mode::Auto on native Windows: fast reads disable bouncing; slow WC reads enable bouncing.
    {
        CbMapBounce fastBounce;
        fastBounce.configure(MapBounceMode::Auto, false);
        if (fastBounce.isActive() || !fastBounce.needsAutoProbe()) {
            return fail(16, "Auto non-Wine should start in Probing state");
        }
        for (uint32_t i = 0; i < CbMapBounce::kAutoSampleCount; ++i) {
            if (fastBounce.recordDirectReadSample(5376, 300)) {
                return fail(17, "Fast reads should not activate bouncing");
            }
        }
        if (fastBounce.isActive() || fastBounce.autoState() != CbMapBounce::AutoState::Disabled) {
            return fail(18, "Fast reads should transition AutoState to Disabled");
        }

        CbMapBounce slowBounce;
        slowBounce.configure(MapBounceMode::Auto, false);
        bool activated = false;
        for (uint32_t i = 0; i < CbMapBounce::kAutoSampleCount; ++i) {
            // 5376 bytes taking 25,000 ns (~4760 ns/KB) simulates WC BAR memory.
            if (slowBounce.recordDirectReadSample(5376, 25000)) {
                activated = true;
            }
        }
        if (!activated || !slowBounce.isActive() ||
            slowBounce.autoState() != CbMapBounce::AutoState::Enabled) {
            return fail(19, "Slow WC reads should activate Auto bouncing");
        }
        if (!slowBounce.consumeAutoActivatedLogPending()) {
            return fail(20, "Auto activation log flag should be set once");
        }
        if (slowBounce.consumeAutoActivatedLogPending()) {
            return fail(21, "Auto activation log flag should clear after consume");
        }
    }

    // 4. Mode::On bounces unconditionally (non-Wine) and supports 8 concurrent slots.
    {
        CbMapBounce bounce;
        bounce.configure(MapBounceMode::On, false);
        if (!bounce.isActive()) return fail(22, "Mode::On should be active");

        std::array<std::array<unsigned char, 512>, 9> bufs{};
        void* ptrs[9]{};
        for (size_t i = 0; i < 8; ++i) {
            ptrs[i] = bounce.onMap(0x3000 + i, 0xB001, bufs[i].data(), 512, true, 2);
            if (ptrs[i] == bufs[i].data()) return fail(23, "Slots 0..7 should bounce");
        }
        // 9th concurrent map passes through cleanly without tripping.
        ptrs[8] = bounce.onMap(0x3008, 0xB001, bufs[8].data(), 512, true, 2);
        if (ptrs[8] != bufs[8].data()) return fail(24, "9th concurrent map should pass through");
        if (bounce.isTripped()) return fail(25, "Pool exhaustion should not trip fail-safe");

        for (size_t i = 0; i < 8; ++i) {
            if (!bounce.onUnmap(0x3000 + i, 0xB001)) return fail(26, "Unmap of slot 0..7 failed");
        }
        if (bounce.openSlotCount() != 0) return fail(27, "All 8 slots should be released");
    }

    // 5. Fail-safe trip: Remap (mapping a resource already open in bounce pool).
    {
        CbMapBounce bounce;
        bounce.configure(MapBounceMode::On, true);
        std::array<unsigned char, 5376> realBuf{};
        void* bounced = bounce.onMap(0x4001, 0xC001, realBuf.data(), 5376, true, 3);
        if (bounced == realBuf.data()) return fail(28, "Expected bounce before Remap test");
        static_cast<unsigned char*>(bounced)[0] = 0x77;

        std::array<unsigned char, 5376> secondReal{};
        void* secondPtr = bounce.onMap(0x4001, 0xC001, secondReal.data(), 5376, true, 3);
        if (secondPtr != secondReal.data()) return fail(29, "Remap trip must return realPtr");
        if (!bounce.isTripped() || bounce.lastTripReason() != MapBounceTrip::Remap) {
            return fail(30, "Remap must trip with TripReason::Remap");
        }
        if (realBuf[0] != 0x77) return fail(31, "Remap trip must flush open slot");
        if (bounce.openSlotCount() != 0) return fail(32, "Remap trip must release all slots");
        if (bounce.isActive()) return fail(33, "Bounce must remain disabled after Remap trip");
        MapBounceTrip reason = MapBounceTrip::None;
        if (!bounce.consumeTripLogPending(&reason) || reason != MapBounceTrip::Remap) {
            return fail(34, "Remap trip log should be pending once");
        }
        if (bounce.consumeTripLogPending(&reason)) {
            return fail(35, "Trip log should only fire once");
        }
    }

    // 6. Fail-safe trip: Present (slot still open at Present).
    {
        CbMapBounce bounce;
        bounce.configure(MapBounceMode::On, true);
        std::array<unsigned char, 272> realBuf{};
        void* bounced = bounce.onMap(0x5001, 0xD001, realBuf.data(), 272, true, 4);
        if (bounced == realBuf.data()) return fail(36, "Expected bounce before Present test");
        static_cast<unsigned char*>(bounced)[10] = 0x42;

        if (bounce.onPresent(4)) return fail(37, "onPresent should return false when slot is open");
        if (!bounce.isTripped() || bounce.lastTripReason() != MapBounceTrip::Present) {
            return fail(38, "Present must trip with TripReason::Present");
        }
        if (realBuf[10] != 0x42) return fail(39, "Present trip must flush open slot");
        if (bounce.openSlotCount() != 0) return fail(40, "Present trip must release all slots");
        if (bounce.isActive()) return fail(41, "Bounce must remain disabled after Present trip");
    }

    // 7. Fail-safe trip: Context (Unmap called on a different context than Map).
    {
        CbMapBounce bounce;
        bounce.configure(MapBounceMode::On, true);
        std::array<unsigned char, 1024> realBuf{};
        void* bounced = bounce.onMap(0x6001, 0xE001, realBuf.data(), 1024, true, 5);
        if (bounced == realBuf.data()) return fail(42, "Expected bounce before Context test");
        static_cast<unsigned char*>(bounced)[500] = 0x99;

        if (!bounce.onUnmap(0x6001, 0xE002)) return fail(43, "onUnmap should still flush on Context mismatch");
        if (!bounce.isTripped() || bounce.lastTripReason() != MapBounceTrip::Context) {
            return fail(44, "Context mismatch must trip with TripReason::Context");
        }
        if (realBuf[500] != 0x99) return fail(45, "Context trip must still flush data to realPtr");
        if (bounce.openSlotCount() != 0) return fail(46, "Context trip must release slot");
        if (bounce.isActive()) return fail(47, "Bounce must remain disabled after Context trip");
    }

    std::printf("cb_map_bounce_test PASSED\n");
    return 0;
}
