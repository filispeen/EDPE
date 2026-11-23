#include "atomic_file.h"

#ifdef _WIN32
#include "config.h"
#endif

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int fail(int code, const char* msg) {
    std::fprintf(stderr, "atomic_file_test FAILED (%d): %s\n", code, msg);
    return code;
}

struct MockAtomicOps {
    bool posixResult = true;
    uint32_t posixErr = 0;
    bool fallbackResult = true;
    int posixCalls = 0;
    int fallbackCalls = 0;
    int deleteCalls = 0;
    std::wstring lastDeleted;

    bool posixRename(const std::wstring& /*tmp*/, const std::wstring& /*dst*/, uint32_t* outErr) {
        ++posixCalls;
        if (outErr) *outErr = posixErr;
        return posixResult;
    }

    bool fallbackMove(const std::wstring& /*tmp*/, const std::wstring& /*dst*/) {
        ++fallbackCalls;
        return fallbackResult;
    }

    void deleteFile(const std::wstring& path) {
        ++deleteCalls;
        lastDeleted = path;
    }
};

} // namespace

int main() {
    using namespace edpe;

    // 1. Error classifier checks.
    if (classifyPosixRenameError(0u) != PosixRenameOutcome::Ok) return fail(1, "ERROR_SUCCESS -> Ok");
    if (classifyPosixRenameError(1u) != PosixRenameOutcome::Unsupported) return fail(2, "INVALID_FUNCTION -> Unsupported");
    if (classifyPosixRenameError(50u) != PosixRenameOutcome::Unsupported) return fail(3, "NOT_SUPPORTED -> Unsupported");
    if (classifyPosixRenameError(87u) != PosixRenameOutcome::Unsupported) return fail(4, "INVALID_PARAMETER -> Unsupported");
    if (classifyPosixRenameError(120u) != PosixRenameOutcome::Unsupported) return fail(5, "CALL_NOT_IMPLEMENTED -> Unsupported");
    if (classifyPosixRenameError(5u) != PosixRenameOutcome::Failed) return fail(6, "ACCESS_DENIED -> Failed");

    // 2. POSIX rename succeeds on first try: no fallback, no latch, no delete.
    {
        std::atomic<bool> latch{false};
        MockAtomicOps ops;
        ops.posixResult = true;
        if (!replaceFileAtomicWithOps(ops, L"a.tmp", L"a.ini", latch)) {
            return fail(7, "Expected POSIX rename success");
        }
        if (ops.posixCalls != 1 || ops.fallbackCalls != 0 || ops.deleteCalls != 0 || latch.load()) {
            return fail(8, "Unexpected fallback/latch/delete on POSIX rename success");
        }
    }

    // 3. Wine quirk: SetFileInformationByHandle returns FALSE with LastError == ERROR_SUCCESS (0).
    // Must treat as ERROR_CALL_NOT_IMPLEMENTED, latch unsupported, and fall back to MoveFileExW.
    {
        std::atomic<bool> latch{false};
        MockAtomicOps ops;
        ops.posixResult = false;
        ops.posixErr = 0u; // Wine quirk!
        ops.fallbackResult = true;

        if (!replaceFileAtomicWithOps(ops, L"b.tmp", L"b.ini", latch)) {
            return fail(9, "Expected fallback success on Wine POSIX refusal");
        }
        if (ops.posixCalls != 1 || ops.fallbackCalls != 1 || !latch.load()) {
            return fail(10, "Wine FALSE+ERROR_SUCCESS must latch posixUnsupported and use fallback");
        }

        // Second call must skip posixRename completely because latch is set.
        if (!replaceFileAtomicWithOps(ops, L"b2.tmp", L"b.ini", latch)) {
            return fail(11, "Second call should succeed via fallback");
        }
        if (ops.posixCalls != 1 || ops.fallbackCalls != 2) {
            return fail(12, "Latched unsupported must bypass posixRename on subsequent writes");
        }
    }

    // 4. Both POSIX rename and fallback fail: temp file must be cleaned up.
    {
        std::atomic<bool> latch{false};
        MockAtomicOps ops;
        ops.posixResult = false;
        ops.posixErr = 87u; // ERROR_INVALID_PARAMETER
        ops.fallbackResult = false;

        if (replaceFileAtomicWithOps(ops, L"c.tmp", L"c.ini", latch)) {
            return fail(13, "Expected failure when both rename and fallback fail");
        }
        if (ops.deleteCalls != 1 || ops.lastDeleted != L"c.tmp" || !latch.load()) {
            return fail(14, "Temp file must be deleted on failure");
        }
    }

#ifdef _WIN32
    // 5. Real Win32 atomic write + overwrite test.
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path();
        const std::filesystem::path target = dir / L"edpe_atomic_file_test.txt";
        std::error_code ec;
        std::filesystem::remove(target, ec);

        const std::string v1 = "first-payload\r\n";
        const std::string v2 = "second-replaced-payload\r\n";
        if (!writeFileAtomic(target, v1)) return fail(20, "Initial writeFileAtomic failed");
        if (!writeFileAtomic(target, v2)) return fail(21, "Overwrite writeFileAtomic failed");

        std::ifstream in(target, std::ios::binary);
        std::string readBack((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::filesystem::remove(target, ec);
        if (readBack != v2) return fail(22, "Replaced file contents did not match");
    }

    // 6. Round-trip Config::saveTo() / Config::loadFrom() parity test.
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path();
        const std::filesystem::path iniPath = dir / L"edpe_config_roundtrip_test.ini";
        std::error_code ec;
        std::filesystem::remove(iniPath, ec);

        Config saved{};
        saved.upscalerBackend = 2;
        saved.upscaleMode = 3;
        saved.dlssPreset = 11;
        saved.autoSyncEliteConfig = false;
        saved.preserveNativeUi = true;
        saved.hdrInput = true;
        saved.sharpness = 0.65f;
        saved.fsrVelocityFactor = 0.35f;
        saved.mipLodBiasOffset = 0.35f;
        saved.jitterEnabled = true;
        saved.jitterInvertX = true;
        saved.jitterInvertY = false;
        saved.jitterSwapXY = true;
        saved.jitterScale = 1.25f;
        saved.jitterPhaseCount = 16;
        saved.jitterMode = 2;
        saved.jitterLogEnabled = true;
        saved.jitterLogInterval = 25;
        saved.jitterLogFlushMs = 2500;
        saved.jitterDebugView = true;
        saved.jitterDebugSensitivity = 2.5f;
        saved.jitterDebugThreshold = 0.050f;
        saved.jitterDebugMode = 2;
        saved.hudDebugView = true;
        saved.hudDebugOpacity = 1.5f;
        saved.hudDebugThreshold = 0.030f;
        saved.hudDebugMode = 3;
        saved.zeroMotionVectors = true;
        saved.motionInvertX = true;
        saved.motionInvertY = true;
        saved.motionScale = 0.75f;
        saved.showPerfOverlay = true;
        saved.captureOverlayInComparison = true;
        saved.enableProfiler = false;
        saved.perfOverlayCorner = 4;
        saved.perfOverlayScale = 1.25f;
        saved.uiScale = 1.15f;
        saved.perfOverlayPosX = 120.5f;
        saved.perfOverlayPosY = 240.5f;
        saved.menuPosX = 300.0f;
        saved.menuPosY = 400.0f;
        saved.menuWidth = 620.0f;
        saved.menuHeight = 710.0f;
        saved.devMode = true;
        saved.debugViewActive = true;
        saved.mapBounceMode = 1;
        saved.renderDocAnnotations = false;
        saved.wrapDirectInputMouse = false;
        saved.wrapDirectInputKeyboard = true;
        saved.gameFpsLimit = 120.0f;
        saved.fsr4 = true;

        if (!saved.saveTo(iniPath.wstring())) return fail(23, "Config::saveTo failed");

        Config loaded{};
        loaded.loadFrom(iniPath.wstring());
        std::filesystem::remove(iniPath, ec);

        if (loaded.upscalerBackend != saved.upscalerBackend) return fail(24, "upscalerBackend mismatch");
        if (loaded.upscaleMode != saved.upscaleMode) return fail(25, "upscaleMode mismatch");
        if (loaded.dlssPreset != saved.dlssPreset) return fail(26, "dlssPreset mismatch");
        if (loaded.autoSyncEliteConfig != saved.autoSyncEliteConfig) return fail(27, "autoSyncEliteConfig mismatch");
        if (loaded.preserveNativeUi != saved.preserveNativeUi) return fail(28, "preserveNativeUi mismatch");
        if (loaded.hdrInput != saved.hdrInput) return fail(29, "hdrInput mismatch");
        if (std::fabs(loaded.sharpness - saved.sharpness) > 0.01f) return fail(30, "sharpness mismatch");
        if (std::fabs(loaded.fsrVelocityFactor - saved.fsrVelocityFactor) > 0.01f) return fail(38, "fsrVelocityFactor mismatch");
        if (std::fabs(loaded.mipLodBiasOffset - saved.mipLodBiasOffset) > 0.01f) return fail(39, "mipLodBiasOffset mismatch");
        if (loaded.jitterMode != saved.jitterMode) return fail(31, "jitterMode mismatch");
        if (loaded.jitterPhaseCount != saved.jitterPhaseCount) return fail(32, "jitterPhaseCount mismatch");
        if (loaded.mapBounceMode != saved.mapBounceMode) return fail(33, "mapBounceMode mismatch");
        if (loaded.devMode != saved.devMode) return fail(34, "devMode mismatch");
        if (loaded.renderDocAnnotations != saved.renderDocAnnotations) return fail(35, "renderDocAnnotations mismatch");
        if (loaded.wrapDirectInputMouse != saved.wrapDirectInputMouse) return fail(36, "wrapDirectInputMouse mismatch");
        if (loaded.wrapDirectInputKeyboard != saved.wrapDirectInputKeyboard) return fail(37, "wrapDirectInputKeyboard mismatch");
        if (std::fabs(loaded.gameFpsLimit - saved.gameFpsLimit) > 0.1f) return fail(47, "gameFpsLimit mismatch");

        // GameFpsLimit=0 must disable limiter cleanly (0.0).
        const std::filesystem::path zeroIniPath = dir / L"edpe_config_zero_fps_test.ini";
        std::filesystem::remove(zeroIniPath, ec);
        if (!writeFileAtomic(zeroIniPath, "[FrameLimiter]\r\nGameFpsLimit=0\r\n")) {
            return fail(48, "write zero fps ini failed");
        }
        Config zeroLoaded{};
        zeroLoaded.loadFrom(zeroIniPath.wstring());
        std::filesystem::remove(zeroIniPath, ec);
        if (zeroLoaded.gameFpsLimit != 0.0f) {
            return fail(49, "GameFpsLimit=0 must load as 0.0f");
        }
        if (loaded.fsr4 != saved.fsr4) return fail(38, "fsr4 mismatch");
    }

    // 6b. The AMD FSR 4.1.2 menu entry persists as Backend=4, and defaults apply when the FSR 4 keys are absent.
    {
        const std::filesystem::path iniPath = std::filesystem::temp_directory_path() / L"edpe_config_fsr4_test.ini";
        std::error_code ec;
        std::filesystem::remove(iniPath, ec);

        Config saved{};
        saved.upscalerBackend = 4;
        saved.upscaleMode = 4;
        saved.fsr4 = true;
        if (!saved.saveTo(iniPath.wstring())) return fail(42, "Config::saveTo (FSR 4 selection) failed");
        Config loaded{};
        loaded.loadFrom(iniPath.wstring());
        if (loaded.upscalerBackend != 4 || !loaded.fsr4) return fail(43, "FSR 4 selection did not round trip");

        // An ini without the FSR 4 key (older files) must load with FSR 4 off.
        std::ofstream(iniPath, std::ios::binary | std::ios::trunc) << "[Upscaler]\r\nBackend=2\r\nMode=4\r\n";
        Config legacy{};
        legacy.fsr4 = true;
        legacy.loadFrom(iniPath.wstring());
        std::filesystem::remove(iniPath, ec);
        if (legacy.fsr4) return fail(44, "FSR 4 key default wrong");
    }

    // 7. SSAA XML parser and effective MipLODBias calculation tests.
    {
        if (std::fabs(Config::parseEliteSsaaMultiplierXml("<Graphics><SSAAMultiplier>0.500000</SSAAMultiplier></Graphics>") - 0.5f) > 1e-5f) {
            return fail(40, "parseEliteSsaaMultiplierXml 0.5 mismatch");
        }
        if (Config::parseEliteSsaaMultiplierXml("<Graphics></Graphics>") != 0.0f) {
            return fail(41, "parseEliteSsaaMultiplierXml missing tag != 0");
        }
        if (Config::parseEliteSsaaMultiplierXml("<SSAAMultiplier>invalid</SSAAMultiplier>") != 0.0f) {
            return fail(42, "parseEliteSsaaMultiplierXml invalid != 0");
        }
        if (Config::computeEffectiveMipLodBias(0, 4, 0.5f, 0.25f) != 0.0f) {
            return fail(43, "bypass MipLODBias != 0");
        }
        if (std::fabs(Config::computeEffectiveMipLodBias(2, 1, 0.5f, 0.0f) - (-1.0f)) > 1e-5f) {
            return fail(44, "eliteSsaa=0.5 override mode=1 MipLODBias != -1.0");
        }
        if (std::fabs(Config::computeEffectiveMipLodBias(2, 1, 0.5f, 0.5f) - (-0.5f)) > 1e-5f) {
            return fail(45, "offset=+0.5 MipLODBias != -0.5");
        }
        if (std::fabs(Config::computeEffectiveMipLodBias(2, 4, 0.0f, 0.0f) - (-1.0f)) > 1e-5f) {
            return fail(46, "fallback mode=4 MipLODBias != -1.0");
        }
    }
#endif

    std::printf("atomic_file_test PASSED\n");
    return 0;
}
