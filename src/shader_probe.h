#pragma once

#include <guiddef.h>
#include <cstddef>
#include <cstdint>

// Full bytecode is attached only during an explicitly armed diagnostic run.
constexpr GUID kEdpeVertexBytecodeGuid =
    {0x0f6d1490, 0x137f, 0x47a3, {0x92, 0xca, 0xb8, 0x55, 0x36, 0xce, 0x71, 0x42}};

// EDVR uses this FNV offset for its game shader signatures (MIT, revision 96df075).
inline std::uint64_t EdpeEdvrShaderHash(const void* bytes, std::size_t size) {
    auto hash = std::uint64_t{1469598103934665603};
    const auto* data = static_cast<const unsigned char*>(bytes);
    for (std::size_t i = 0; i < size; ++i)
        hash = (hash ^ data[i]) * 1099511628211ull;
    return hash;
}

constexpr GUID kEdpeShaderHashGuid =
    {0x761fe44e, 0x16ab, 0x4e3f, {0x8c, 0xa5, 0xe5, 0x21, 0xf6, 0x11, 0x4d, 0x6a}};
