#include "gpu_vendor.h"

#include <cstdio>

namespace {

int fail(int code, const char* msg) {
    std::fprintf(stderr, "gpu_vendor_test FAILED (%d): %s\n", code, msg);
    return code;
}

} // namespace

int main() {
    using namespace edpe;

    // 1. Spoofed AMD under Wine: VendorId=0x10DE with Radeon/AMD description.
    if (classifyGpuVendor(0x10DEu, 0x744Cu, L"AMD Radeon RX 7900 XTX", true) != GpuVendor::Amd) {
        return fail(1, "Spoofed 0x10DE with AMD Radeon description should classify as AMD");
    }

    // 2. Known spoofed AMD device under Wine (0x10DE / 0x1586) even with spoofed NVIDIA description.
    if (classifyGpuVendor(0x10DEu, 0x1586u, L"NVIDIA GeForce RTX 4090", true) != GpuVendor::Amd) {
        return fail(2, "Wine 0x10DE/0x1586 must classify as AMD even when description is spoofed");
    }

    // 3. Same 0x10DE/0x1586 on native Windows with NVIDIA description stays NVIDIA.
    if (classifyGpuVendor(0x10DEu, 0x1586u, L"NVIDIA GeForce RTX 4090", false) != GpuVendor::Nvidia) {
        return fail(3, "Non-Wine NVIDIA description should classify as NVIDIA");
    }

    // 4. Real NVIDIA GPUs (GeForce, Quadro, RTX, PCI fallback).
    if (classifyGpuVendor(0x10DEu, 0x2684u, L"NVIDIA GeForce RTX 4090", false) != GpuVendor::Nvidia) {
        return fail(4, "Real NVIDIA GeForce should classify as NVIDIA");
    }
    if (classifyGpuVendor(0x10DEu, 0x1BB0u, L"Quadro P5000", false) != GpuVendor::Nvidia) {
        return fail(5, "Quadro description should classify as NVIDIA");
    }
    if (classifyGpuVendor(0x10DEu, 0x2704u, L"RTX 4080", true) != GpuVendor::Nvidia) {
        return fail(6, "RTX description should classify as NVIDIA");
    }
    if (classifyGpuVendor(0x10DEu, 0x2704u, L"", false) != GpuVendor::Nvidia) {
        return fail(7, "Empty description with 0x10DE should fall back to NVIDIA");
    }

    // 5. Intel GPUs (Intel, Arc, PCI fallback).
    if (classifyGpuVendor(0x8086u, 0x56A0u, L"Intel(R) Arc(TM) A770 Graphics", false) != GpuVendor::Intel) {
        return fail(8, "Intel Arc should classify as Intel");
    }
    if (classifyGpuVendor(0x10DEu, 0x56A0u, L"Arc B580", true) != GpuVendor::Intel) {
        return fail(9, "Spoofed VendorId with Arc description should classify as Intel");
    }
    if (classifyGpuVendor(0x8086u, 0x4680u, nullptr, false) != GpuVendor::Intel) {
        return fail(10, "Null description with 0x8086 should fall back to Intel");
    }

    // 6. AMD PCI fallbacks (0x1002 and 0x1022).
    if (classifyGpuVendor(0x1002u, 0x744Cu, L"", false) != GpuVendor::Amd) {
        return fail(11, "Empty description with 0x1002 should fall back to AMD");
    }
    if (classifyGpuVendor(0x1022u, 0x1506u, nullptr, false) != GpuVendor::Amd) {
        return fail(12, "Null description with 0x1022 should fall back to AMD");
    }

    // 7. Unknown and Other edge cases.
    if (classifyGpuVendor(0u, 0u, L"", false) != GpuVendor::Unknown) {
        return fail(13, "Zero vendorId and empty description should classify as Unknown");
    }
    if (classifyGpuVendor(0u, 0u, nullptr, false) != GpuVendor::Unknown) {
        return fail(14, "Zero vendorId and null description should classify as Unknown");
    }
    if (classifyGpuVendor(0x1414u, 0x008Cu, L"Microsoft Basic Render Driver", false) != GpuVendor::Other) {
        return fail(15, "Microsoft Basic Render Driver should classify as Other");
    }

    // 8. Word-boundary check: "architecture" must not falsely match "arc".
    if (classifyGpuVendor(0x1414u, 0x0001u, L"Generic Architecture Adapter", false) != GpuVendor::Other) {
        return fail(16, "Word 'Architecture' should not falsely match Intel Arc");
    }

    std::printf("gpu_vendor_test PASSED\n");
    return 0;
}
