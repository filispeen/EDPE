#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace edpe {

// Decodes a 16-bit half-precision float (IEEE 754 binary16) to 32-bit single-precision float.
inline float halfToFloat(uint16_t h) noexcept {
    const uint32_t sign = (h >> 15) & 0x0001;
    uint32_t exp  = (h >> 10) & 0x001F;
    uint32_t mant = h & 0x03FF;
    if (exp == 0) {
        if (mant == 0) {
            const uint32_t val = sign << 31;
            float f = 0.0f;
            std::memcpy(&f, &val, 4);
            return f;
        }
        while (!(mant & 0x0400)) {
            mant <<= 1;
            exp--;
        }
        exp++;
        mant &= ~0x0400;
    } else if (exp == 31) {
        const uint32_t val = (sign << 31) | 0x7F800000 | (mant << 13);
        float f = 0.0f;
        std::memcpy(&f, &val, 4);
        return f;
    }
    exp = exp + (127 - 15);
    mant = mant << 13;
    const uint32_t val = (sign << 31) | (exp << 23) | mant;
    float f = 0.0f;
    std::memcpy(&f, &val, 4);
    return f;
}

// Decodes an unsigned 11-bit floating point value (DXGI_FORMAT_R11G11B10_FLOAT red/green).
// 5-bit exponent, 6-bit mantissa, no sign bit.
inline float decodeFloat11(uint32_t val) noexcept {
    const uint32_t mant = val & 0x3F;
    const uint32_t exp  = (val >> 6) & 0x1F;
    if (exp == 0) {
        return (mant != 0) ? std::ldexp(static_cast<float>(mant) / 64.0f, -14) : 0.0f;
    } else if (exp == 31) {
        return (mant == 0) ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
    }
    return std::ldexp(1.0f + static_cast<float>(mant) / 64.0f, static_cast<int>(exp) - 15);
}

// Decodes an unsigned 10-bit floating point value (DXGI_FORMAT_R11G11B10_FLOAT blue).
// 5-bit exponent, 5-bit mantissa, no sign bit.
inline float decodeFloat10(uint32_t val) noexcept {
    const uint32_t mant = val & 0x1F;
    const uint32_t exp  = (val >> 5) & 0x1F;
    if (exp == 0) {
        return (mant != 0) ? std::ldexp(static_cast<float>(mant) / 32.0f, -14) : 0.0f;
    } else if (exp == 31) {
        return (mant == 0) ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
    }
    return std::ldexp(1.0f + static_cast<float>(mant) / 32.0f, static_cast<int>(exp) - 15);
}

// Applies contrast enhancement to Elite's reversed-Z depth buffer values:
// z / (z + k), mapping near geometry to bright values while keeping infinite background dark.
inline float depthContrast(float depth, float k = 0.001f) noexcept {
    if (!std::isfinite(depth) || depth <= 0.0f) return 0.0f;
    return std::clamp(depth / (depth + k), 0.0f, 1.0f);
}

// Maps pixel displacement (mx, my) to visual BGR colors matching the ImGui motion preview shader.
// Neutral motion is (128, 128, 128). Horizontal motion shifts Red, vertical motion shifts Green.
inline void motionToBgr(float mx, float my, uint8_t& out_b, uint8_t& out_g, uint8_t& out_r, float scale = 10.0f) noexcept {
    float r = 0.5f;
    float g = 0.5f;
    if (std::isfinite(mx)) {
        r = 0.5f + 0.5f * (mx / (std::abs(mx) + scale));
    }
    if (std::isfinite(my)) {
        g = 0.5f + 0.5f * (my / (std::abs(my) + scale));
    }
    out_b = 128;
    out_g = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
    out_r = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Reinhard tone mapping + gamma 2.2 approximation for HDR scene colors.
inline uint8_t toneMapSrgb(float c) noexcept {
    if (!std::isfinite(c) || c <= 0.0f) return 0;
    const float mapped = c / (c + 1.0f);
    const float srgb = std::pow(mapped, 1.0f / 2.2f);
    return static_cast<uint8_t>(std::clamp(srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
}

} // namespace edpe
