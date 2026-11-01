#include "capture_format.h"

#include <cmath>
#include <iostream>

int main() {
    // 1. halfToFloat tests
    // 0.0f
    if (edpe::halfToFloat(0x0000) != 0.0f) return 1;
    // -0.0f
    if (edpe::halfToFloat(0x8000) != -0.0f) return 2;
    // 1.0f (exponent 15)
    if (std::fabs(edpe::halfToFloat(0x3C00) - 1.0f) > 1e-6f) return 3;
    // -1.0f
    if (std::fabs(edpe::halfToFloat(0xBC00) - (-1.0f)) > 1e-6f) return 4;
    // 2.0f
    if (std::fabs(edpe::halfToFloat(0x4000) - 2.0f) > 1e-6f) return 5;
    // 0.5f
    if (std::fabs(edpe::halfToFloat(0x3800) - 0.5f) > 1e-6f) return 6;
    // Infinity
    if (!std::isinf(edpe::halfToFloat(0x7C00))) return 7;
    if (!std::isinf(edpe::halfToFloat(0xFC00))) return 8;

    // 2. decodeFloat11 tests
    // 0.0f
    if (edpe::decodeFloat11(0) != 0.0f) return 10;
    // 1.0f: exponent 15 (0x0F), mantissa 0 -> (15 << 6) = 0x3C0
    if (std::fabs(edpe::decodeFloat11(0x3C0) - 1.0f) > 1e-6f) return 11;
    // 2.0f: exponent 16 (0x10), mantissa 0 -> (16 << 6) = 0x400
    if (std::fabs(edpe::decodeFloat11(0x400) - 2.0f) > 1e-6f) return 12;
    // 1.5f: exponent 15, mantissa 32 (half of 64) -> (15 << 6) | 32 = 0x3E0
    if (std::fabs(edpe::decodeFloat11(0x3E0) - 1.5f) > 1e-6f) return 13;

    // 3. decodeFloat10 tests
    // 0.0f
    if (edpe::decodeFloat10(0) != 0.0f) return 20;
    // 1.0f: exponent 15 (0x0F), mantissa 0 -> (15 << 5) = 0x1E0
    if (std::fabs(edpe::decodeFloat10(0x1E0) - 1.0f) > 1e-6f) return 21;
    // 2.0f: exponent 16 (0x10), mantissa 0 -> (16 << 5) = 0x200
    if (std::fabs(edpe::decodeFloat10(0x200) - 2.0f) > 1e-6f) return 22;
    // 1.5f: exponent 15, mantissa 16 (half of 32) -> (15 << 5) | 16 = 0x1F0
    if (std::fabs(edpe::decodeFloat10(0x1F0) - 1.5f) > 1e-6f) return 23;

    // 4. depthContrast tests
    // Far/background: 0.0f -> 0.0f
    if (edpe::depthContrast(0.0f) != 0.0f) return 30;
    // Negative or NaN -> 0.0f
    if (edpe::depthContrast(-1.0f) != 0.0f) return 31;
    // Depth equal to k (0.001f): 0.001 / (0.001 + 0.001) = 0.5f
    if (std::fabs(edpe::depthContrast(0.001f, 0.001f) - 0.5f) > 1e-6f) return 32;
    // Very near (1.0f): 1.0 / 1.001 ~ 0.999f
    if (edpe::depthContrast(1.0f, 0.001f) < 0.99f) return 33;

    // 5. motionToBgr tests
    // Zero motion -> neutral gray (B=128, G=128, R=128)
    uint8_t b = 0, g = 0, r = 0;
    edpe::motionToBgr(0.0f, 0.0f, b, g, r);
    if (b != 128 || g != 128 || r != 128) return 40;
    // Positive X motion: R increases, G remains neutral
    edpe::motionToBgr(10.0f, 0.0f, b, g, r);
    // 0.5 + 0.5 * (10 / 20) = 0.75 -> 0.75 * 255 = 191
    if (r < 185 || r > 195 || g != 128 || b != 128) return 41;
    // Negative Y motion: G decreases, R remains neutral
    edpe::motionToBgr(0.0f, -10.0f, b, g, r);
    // 0.5 + 0.5 * (-10 / 20) = 0.25 -> 0.25 * 255 = 64
    if (g < 60 || g > 70 || r != 128 || b != 128) return 42;

    // 6. toneMapSrgb tests
    // 0.0f -> 0
    if (edpe::toneMapSrgb(0.0f) != 0) return 50;
    // 1.0f: Reinhard mapped = 0.5, gamma 2.2 = 0.5^(1/2.2) ~ 0.7297 -> ~186
    uint8_t srgb1 = edpe::toneMapSrgb(1.0f);
    if (srgb1 < 180 || srgb1 > 192) return 51;
    // High HDR brightness (100.0f): mapped ~ 0.99 -> ~254 or 255
    uint8_t srgb100 = edpe::toneMapSrgb(100.0f);
    if (srgb100 < 250) return 52;

    return 0;
}
