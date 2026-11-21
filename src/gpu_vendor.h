#pragma once

#include <cstddef>
#include <cstdint>
#include <cwctype>

namespace edpe {

enum class GpuVendor : uint8_t {
    Unknown = 0,
    Nvidia = 1,
    Amd = 2,
    Intel = 3,
    Other = 4,
};

inline const wchar_t* gpuVendorNameW(GpuVendor vendor) {
    switch (vendor) {
    case GpuVendor::Nvidia: return L"NVIDIA";
    case GpuVendor::Amd:    return L"AMD";
    case GpuVendor::Intel:  return L"Intel";
    case GpuVendor::Other:  return L"Other";
    default:                return L"Unknown";
    }
}

namespace detail {

inline bool isAsciiAlphaNum(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
}

inline bool containsCaseInsensitive(const wchar_t* haystack, const wchar_t* needle, bool wholeWord = false) {
    if (!haystack || !needle || !needle[0]) return false;
    size_t needleLen = 0;
    while (needle[needleLen] != L'\0') ++needleLen;

    for (size_t i = 0; haystack[i] != L'\0'; ++i) {
        size_t j = 0;
        while (j < needleLen && haystack[i + j] != L'\0' &&
               std::towlower(haystack[i + j]) == std::towlower(needle[j])) {
            ++j;
        }
        if (j == needleLen) {
            if (!wholeWord) return true;
            const bool leftOk = (i == 0) || !isAsciiAlphaNum(haystack[i - 1]);
            const bool rightOk = (haystack[i + j] == L'\0') || !isAsciiAlphaNum(haystack[i + j]);
            if (leftOk && rightOk) return true;
        }
    }
    return false;
}

} // namespace detail

// Classify the real GPU vendor, guarding against DXVK's Elite Dangerous profile
// and DXVK_ENABLE_NVAPI=1 spoofing DXGI_ADAPTER_DESC::VendorId to 0x10DE (NVIDIA)
// on AMD GPUs.
inline GpuVendor classifyGpuVendor(uint32_t vendorId, uint32_t deviceId,
                                   const wchar_t* description, bool isWine) {
    // Known DXVK/Proton spoofed AMD device IDs under Wine, even if description is also spoofed.
    if (isWine && vendorId == 0x10DEu && deviceId == 0x1586u) {
        return GpuVendor::Amd;
    }

    // Check adapter description case-insensitively before trusting PCI VendorId.
    if (description && description[0] != L'\0') {
        if (detail::containsCaseInsensitive(description, L"radeon") ||
            detail::containsCaseInsensitive(description, L"amd", true)) {
            return GpuVendor::Amd;
        }
        if (detail::containsCaseInsensitive(description, L"nvidia") ||
            detail::containsCaseInsensitive(description, L"geforce") ||
            detail::containsCaseInsensitive(description, L"quadro") ||
            detail::containsCaseInsensitive(description, L"rtx", true)) {
            return GpuVendor::Nvidia;
        }
        if (detail::containsCaseInsensitive(description, L"intel") ||
            detail::containsCaseInsensitive(description, L"arc", true)) {
            return GpuVendor::Intel;
        }
    }

    // Fall back to PCI VendorId.
    if (vendorId == 0x1002u || vendorId == 0x1022u) return GpuVendor::Amd;
    if (vendorId == 0x10DEu) return GpuVendor::Nvidia;
    if (vendorId == 0x8086u) return GpuVendor::Intel;
    if (vendorId == 0u && (!description || description[0] == L'\0')) return GpuVendor::Unknown;
    return GpuVendor::Other;
}

} // namespace edpe
