#pragma once

#include <guiddef.h>
#include <cstddef>
#include <cstdint>

// Full bytecode is attached only during an explicitly armed diagnostic run.
constexpr GUID kEdpeVertexBytecodeGuid =
    {0x0f6d1490, 0x137f, 0x47a3, {0x92, 0xca, 0xb8, 0x55, 0x36, 0xce, 0x71, 0x42}};

// Pixel bytecode is retained at creation for later one-frame HDR fanout.
constexpr GUID kEdpePixelBytecodeGuid =
    {0x8b7c68fa, 0x223f, 0x49e9, {0xb1, 0x0c, 0x7c, 0xd5, 0x2e, 0x86, 0x09, 0xa4}};

// 64-bit FNV-1a hash used for game shader signatures.
inline std::uint64_t EdpeShaderHash(const void* bytes, std::size_t size) {
    auto hash = std::uint64_t{1469598103934665603};
    const auto* data = static_cast<const unsigned char*>(bytes);
    for (std::size_t i = 0; i < size; ++i)
        hash = (hash ^ data[i]) * 1099511628211ull;
    return hash;
}

constexpr GUID kEdpeShaderHashGuid =
    {0x761fe44e, 0x16ab, 0x4e3f, {0x8c, 0xa5, 0xe5, 0x21, 0xf6, 0x11, 0x4d, 0x6a}};

// Cached classification of which CB(s) a vertex shader reads for clip-space projection.
constexpr GUID kEdpeVsProjCbGuid =
    {0x3a91c8e2, 0x4b5d, 0x4f71, {0x9e, 0x12, 0x6c, 0x84, 0xd0, 0x25, 0x7b, 0x91}};

// Cached flag (uint8_t: 1 = frosted backdrop PS sampling blurred scene t0 via SV_Position * cb1[332].zw).
constexpr GUID kEdpePsFrostedBackdropGuid =
    {0x6c42d91f, 0x8e3a, 0x4b12, {0xa5, 0x71, 0x2d, 0x90, 0x4e, 0xc8, 0x3b, 0x54}};
