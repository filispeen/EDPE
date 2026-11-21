#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace edpe {

// Win32 error code constants kept explicit so the classifier is testable on any platform.
namespace detail {
inline constexpr uint32_t kWinErrorSuccess = 0u;
inline constexpr uint32_t kWinErrorInvalidFunction = 1u;
inline constexpr uint32_t kWinErrorNotSupported = 50u;
inline constexpr uint32_t kWinErrorInvalidParameter = 87u;
inline constexpr uint32_t kWinErrorCallNotImplemented = 120u;

inline constexpr uint32_t kFileRenameInfoExClass = 22u;
inline constexpr uint32_t kFileRenameFlagReplaceIfExists = 0x00000001u;
inline constexpr uint32_t kFileRenameFlagPosixSemantics = 0x00000002u;
} // namespace detail

enum class PosixRenameOutcome : uint8_t {
    Ok = 0,
    Unsupported = 1,
    Failed = 2,
};

inline PosixRenameOutcome classifyPosixRenameError(uint32_t err) {
    switch (err) {
    case detail::kWinErrorSuccess:
        return PosixRenameOutcome::Ok;
    case detail::kWinErrorInvalidParameter:
    case detail::kWinErrorInvalidFunction:
    case detail::kWinErrorNotSupported:
    case detail::kWinErrorCallNotImplemented:
        return PosixRenameOutcome::Unsupported;
    default:
        return PosixRenameOutcome::Failed;
    }
}

// Process-wide latch set once when Wine or an older Windows filesystem refuses
// FileRenameInfoEx + FILE_RENAME_FLAG_POSIX_SEMANTICS.
inline std::atomic<bool> g_posixRenameUnsupported{false};

// Platform-independent atomic replacement state machine (used by Win32 writeFileAtomic
// and directly unit-tested with mock ops).
template <typename Ops>
inline bool replaceFileAtomicWithOps(Ops& ops,
                                     const std::wstring& tmpPath,
                                     const std::wstring& dstPath,
                                     std::atomic<bool>& posixUnsupportedLatch) {
    if (!posixUnsupportedLatch.load(std::memory_order_acquire)) {
        uint32_t err = detail::kWinErrorSuccess;
        const bool ok = ops.posixRename(tmpPath, dstPath, &err);
        if (ok) {
            return true;
        }
        // Wine safeguard: if SetFileInformationByHandle returns FALSE without setting
        // LastError (remains ERROR_SUCCESS), treat it as ERROR_CALL_NOT_IMPLEMENTED.
        if (err == detail::kWinErrorSuccess) {
            err = detail::kWinErrorCallNotImplemented;
        }
        const PosixRenameOutcome outcome = classifyPosixRenameError(err);
        if (outcome == PosixRenameOutcome::Unsupported) {
            posixUnsupportedLatch.store(true, std::memory_order_release);
        }
        // Fall through to MoveFileExW fallback.
    }

    if (ops.fallbackMove(tmpPath, dstPath)) {
        return true;
    }

    ops.deleteFile(tmpPath);
    return false;
}

#ifdef _WIN32

namespace detail {

struct FileRenameInfoExHeader {
    DWORD Flags;
    HANDLE RootDirectory;
    DWORD FileNameLength;
    WCHAR FileName[1];
};

struct Win32AtomicFileOps {
    bool posixRename(const std::wstring& tmpPath, const std::wstring& dstPath, uint32_t* outErr) const {
        HANDLE h = CreateFileW(
            tmpPath.c_str(),
            DELETE | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (outErr) *outErr = GetLastError();
            return false;
        }

        const size_t nameBytes = dstPath.size() * sizeof(wchar_t);
        const size_t totalBytes = offsetof(FileRenameInfoExHeader, FileName) + nameBytes + sizeof(wchar_t);
        std::vector<unsigned char> buffer(totalBytes, 0);
        auto* info = reinterpret_cast<FileRenameInfoExHeader*>(buffer.data());
        info->Flags = kFileRenameFlagReplaceIfExists | kFileRenameFlagPosixSemantics;
        info->RootDirectory = nullptr;
        info->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(info->FileName, dstPath.data(), nameBytes);

        // Wine safeguard: clear LastError before calling SetFileInformationByHandle.
        SetLastError(ERROR_SUCCESS);
        const BOOL renamed = SetFileInformationByHandle(
            h,
            static_cast<FILE_INFO_BY_HANDLE_CLASS>(kFileRenameInfoExClass),
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        DWORD err = renamed ? ERROR_SUCCESS : GetLastError();
        if (!renamed && err == ERROR_SUCCESS) {
            err = ERROR_CALL_NOT_IMPLEMENTED;
        }
        CloseHandle(h);
        if (outErr) *outErr = err;
        return renamed != FALSE;
    }

    bool fallbackMove(const std::wstring& tmpPath, const std::wstring& dstPath) const {
        return MoveFileExW(
            tmpPath.c_str(),
            dstPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }

    void deleteFile(const std::wstring& path) const {
        DeleteFileW(path.c_str());
    }
};

} // namespace detail

inline bool writeFileAtomic(const std::filesystem::path& path, const void* data, size_t size) {
    if (path.empty()) return false;
    const std::wstring dstPath = path.wstring();
    const std::wstring tmpPath = dstPath + L".edpe.tmp." + std::to_wstring(GetCurrentProcessId());

    HANDLE h = CreateFileW(
        tmpPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    const auto* ptr = static_cast<const unsigned char*>(data);
    size_t remaining = size;
    while (remaining > 0) {
        const DWORD chunk = (remaining > 0x40000000u) ? 0x40000000u : static_cast<DWORD>(remaining);
        DWORD written = 0;
        if (!WriteFile(h, ptr, chunk, &written, nullptr) || written == 0) {
            CloseHandle(h);
            DeleteFileW(tmpPath.c_str());
            return false;
        }
        ptr += written;
        remaining -= written;
    }

    if (!FlushFileBuffers(h)) {
        CloseHandle(h);
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    CloseHandle(h);

    detail::Win32AtomicFileOps ops;
    return replaceFileAtomicWithOps(ops, tmpPath, dstPath, g_posixRenameUnsupported);
}

inline bool writeFileAtomic(const std::filesystem::path& path, const std::string& bytes) {
    return writeFileAtomic(path, bytes.data(), bytes.size());
}

inline bool writeFileAtomic(const std::wstring& path, const std::string& bytes) {
    return writeFileAtomic(std::filesystem::path(path), bytes.data(), bytes.size());
}

#endif // _WIN32

} // namespace edpe
