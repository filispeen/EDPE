#pragma once

#include <string>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <windows.h>

namespace edpe {

inline constexpr const char* kVersion = "v0.4.6";

inline bool isRunningInEliteProcess() {
    wchar_t modulePath[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (!len || len >= MAX_PATH) return false;
    std::wstring filename = std::filesystem::path(modulePath).filename().wstring();
    for (auto& ch : filename) ch = static_cast<wchar_t>(towlower(ch));
    return filename.find(L"elitedangerous") != std::wstring::npos;
}

class Config {
public:
    static Config& instance() {
        static Config s_cfg;
        return s_cfg;
    }

    void load() {
        const std::wstring path = getIniPath();
        if (path.empty()) return;

        upscalerBackend = GetPrivateProfileIntW(L"Upscaler", L"Backend", 0, path.c_str());
        upscaleMode = GetPrivateProfileIntW(L"Upscaler", L"Mode", 0, path.c_str());
        dlssPreset = GetPrivateProfileIntW(L"Upscaler", L"Preset", 0, path.c_str());
        autoSyncEliteConfig = GetPrivateProfileIntW(L"Upscaler", L"AutoSyncEliteConfig", 1, path.c_str()) != 0;
        preserveNativeUi = GetPrivateProfileIntW(L"Upscaler", L"PreserveNativeUi", 1, path.c_str()) != 0;
        hdrInput = GetPrivateProfileIntW(L"Upscaler", L"HdrInput", 0, path.c_str()) != 0;

        wchar_t sharpnessBuf[64]{};
        GetPrivateProfileStringW(L"Upscaler", L"Sharpness", L"0.0", sharpnessBuf, 64, path.c_str());
        sharpness = static_cast<float>(_wtof(sharpnessBuf));
        if (sharpness < 0.0f || sharpness > 1.0f) sharpness = 0.0f;

        jitterInvertX = GetPrivateProfileIntW(L"Jitter", L"InvertX", 0, path.c_str()) != 0;
        jitterInvertY = GetPrivateProfileIntW(L"Jitter", L"InvertY", 0, path.c_str()) != 0;
        jitterSwapXY = GetPrivateProfileIntW(L"Jitter", L"SwapXY", 0, path.c_str()) != 0;

        wchar_t buf[64]{};
        GetPrivateProfileStringW(L"Jitter", L"Scale", L"1.0", buf, 64, path.c_str());
        jitterScale = static_cast<float>(_wtof(buf));
        if (jitterScale < 0.0f || jitterScale > 2.0f) jitterScale = 1.0f;

        jitterPhaseCount = GetPrivateProfileIntW(L"Jitter", L"PhaseCount", 32, path.c_str());

        jitterDebugView = GetPrivateProfileIntW(L"Jitter", L"DebugView", 0, path.c_str()) != 0;
        GetPrivateProfileStringW(L"Jitter", L"DebugSensitivity", L"1.5", buf, 64, path.c_str());
        jitterDebugSensitivity = static_cast<float>(_wtof(buf));
        if (jitterDebugSensitivity < 0.1f || jitterDebugSensitivity > 10.0f) jitterDebugSensitivity = 1.5f;
        GetPrivateProfileStringW(L"Jitter", L"DebugThreshold", L"0.02", buf, 64, path.c_str());
        jitterDebugThreshold = static_cast<float>(_wtof(buf));
        if (jitterDebugThreshold < 0.001f || jitterDebugThreshold > 0.5f) jitterDebugThreshold = 0.02f;
        jitterDebugMode = GetPrivateProfileIntW(L"Jitter", L"DebugMode", 0, path.c_str());
        if (jitterDebugMode < 0 || jitterDebugMode > 3) jitterDebugMode = 0;

        zeroMotionVectors = GetPrivateProfileIntW(L"Motion", L"ZeroMotion", 0, path.c_str()) != 0;
        motionInvertX = GetPrivateProfileIntW(L"Motion", L"InvertX", 0, path.c_str()) != 0;
        motionInvertY = GetPrivateProfileIntW(L"Motion", L"InvertY", 0, path.c_str()) != 0;

        GetPrivateProfileStringW(L"Motion", L"Scale", L"1.0", buf, 64, path.c_str());
        motionScale = static_cast<float>(_wtof(buf));
        if (motionScale < 0.0f || motionScale > 2.0f) motionScale = 1.0f;

        depthSource = GetPrivateProfileIntW(L"Depth", L"DepthSource", 0, path.c_str());

        showPerfOverlay = GetPrivateProfileIntW(L"Overlay", L"ShowPerfOverlay", 0, path.c_str()) != 0;
        perfOverlayCorner = GetPrivateProfileIntW(L"Overlay", L"Corner", 0, path.c_str());
        if (perfOverlayCorner < 0 || perfOverlayCorner > 3) perfOverlayCorner = 0;

        GetPrivateProfileStringW(L"Overlay", L"Scale", L"1.0", buf, 64, path.c_str());
        perfOverlayScale = static_cast<float>(_wtof(buf));
        if (perfOverlayScale < 0.5f || perfOverlayScale > 3.0f) perfOverlayScale = 1.0f;

        GetPrivateProfileStringW(L"Overlay", L"PosX", L"-1.0", buf, 64, path.c_str());
        perfOverlayPosX = static_cast<float>(_wtof(buf));
        GetPrivateProfileStringW(L"Overlay", L"PosY", L"-1.0", buf, 64, path.c_str());
        perfOverlayPosY = static_cast<float>(_wtof(buf));
    }

    void save() {
        const std::wstring path = getIniPath();
        if (path.empty()) return;

        WritePrivateProfileStringW(L"Upscaler", L"Backend", std::to_wstring(upscalerBackend).c_str(), path.c_str());
        WritePrivateProfileStringW(L"Upscaler", L"Mode", std::to_wstring(upscaleMode).c_str(), path.c_str());
        WritePrivateProfileStringW(L"Upscaler", L"Preset", std::to_wstring(dlssPreset).c_str(), path.c_str());
        WritePrivateProfileStringW(L"Upscaler", L"AutoSyncEliteConfig", autoSyncEliteConfig ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Upscaler", L"PreserveNativeUi", preserveNativeUi ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Upscaler", L"HdrInput", hdrInput ? L"1" : L"0", path.c_str());

        wchar_t sharpnessBuf[64]{};
        swprintf_s(sharpnessBuf, L"%.2f", sharpness);
        WritePrivateProfileStringW(L"Upscaler", L"Sharpness", sharpnessBuf, path.c_str());

        WritePrivateProfileStringW(L"Jitter", L"InvertX", jitterInvertX ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Jitter", L"InvertY", jitterInvertY ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Jitter", L"SwapXY", jitterSwapXY ? L"1" : L"0", path.c_str());

        wchar_t buf[64]{};
        swprintf_s(buf, L"%.2f", jitterScale);
        WritePrivateProfileStringW(L"Jitter", L"Scale", buf, path.c_str());
        WritePrivateProfileStringW(L"Jitter", L"PhaseCount", std::to_wstring(jitterPhaseCount).c_str(), path.c_str());

        WritePrivateProfileStringW(L"Jitter", L"DebugView", jitterDebugView ? L"1" : L"0", path.c_str());
        swprintf_s(buf, L"%.2f", jitterDebugSensitivity);
        WritePrivateProfileStringW(L"Jitter", L"DebugSensitivity", buf, path.c_str());
        swprintf_s(buf, L"%.3f", jitterDebugThreshold);
        WritePrivateProfileStringW(L"Jitter", L"DebugThreshold", buf, path.c_str());
        WritePrivateProfileStringW(L"Jitter", L"DebugMode", std::to_wstring(jitterDebugMode).c_str(), path.c_str());

        WritePrivateProfileStringW(L"Motion", L"ZeroMotion", zeroMotionVectors ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Motion", L"InvertX", motionInvertX ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Motion", L"InvertY", motionInvertY ? L"1" : L"0", path.c_str());
        swprintf_s(buf, L"%.2f", motionScale);
        WritePrivateProfileStringW(L"Motion", L"Scale", buf, path.c_str());

        WritePrivateProfileStringW(L"Depth", L"DepthSource", std::to_wstring(depthSource).c_str(), path.c_str());

        WritePrivateProfileStringW(L"Overlay", L"ShowPerfOverlay", showPerfOverlay ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Overlay", L"Corner", std::to_wstring(perfOverlayCorner).c_str(), path.c_str());
        swprintf_s(buf, L"%.2f", perfOverlayScale);
        WritePrivateProfileStringW(L"Overlay", L"Scale", buf, path.c_str());
        swprintf_s(buf, L"%.1f", perfOverlayPosX);
        WritePrivateProfileStringW(L"Overlay", L"PosX", buf, path.c_str());
        swprintf_s(buf, L"%.1f", perfOverlayPosY);
        WritePrivateProfileStringW(L"Overlay", L"PosY", buf, path.c_str());
    }

    static float getTargetSsaaMultiplier(int backend, int mode) {
        if (backend == 0 || backend == 3 || mode <= 1) return 1.000000f;
        switch (mode) {
        case 2: return 0.666667f; // Quality (66.7%)
        case 3: return 0.580000f; // Balanced (58.0%)
        case 4: return 0.500000f; // Performance (50.0%)
        case 5: return 0.333333f; // Ultra Performance (33.3%)
        default: return 1.000000f;
        }
    }

    bool isRunningInEliteProcess() const {
        return edpe::isRunningInEliteProcess();
    }

    // Synchronizes Elite Dangerous's Custom.4.4.fxcfg <SSAAMultiplier> with the selected
    // EDPE UpscaleMode so Cobra renders the 3D scene at true lower internal resolution.
    bool syncEliteGraphicsConfig() const {
        if (!autoSyncEliteConfig || !isRunningInEliteProcess()) return false;

        wchar_t localAppData[MAX_PATH]{};
        const DWORD envLen = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
        if (!envLen || envLen >= MAX_PATH) return false;

        std::filesystem::path gfxDir = std::filesystem::path(localAppData) /
            L"Frontier Developments" / L"Elite Dangerous" / L"Options" / L"Graphics";
        std::filesystem::path cfgPath = gfxDir / L"Custom.4.4.fxcfg";
        std::error_code ec;
        if (!std::filesystem::exists(cfgPath, ec)) {
            cfgPath = gfxDir / L"Custom.4.0.fxcfg";
            if (!std::filesystem::exists(cfgPath, ec)) return false;
        }

        std::ifstream inFile(cfgPath, std::ios::binary);
        if (!inFile) return false;
        std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
        inFile.close();
        if (content.empty()) return false;

        const float mult = getTargetSsaaMultiplier(upscalerBackend, upscaleMode);
        char ssaaTag[64]{};
        std::snprintf(ssaaTag, sizeof(ssaaTag), "<SSAAMultiplier>%.6f</SSAAMultiplier>", mult);

        bool modified = false;
        auto replaceTag = [&](const char* openTag, const char* closeTag, const char* newFullTag) {
            const size_t start = content.find(openTag);
            if (start == std::string::npos) return;
            const size_t end = content.find(closeTag, start);
            if (end == std::string::npos) return;
            const size_t fullLen = (end - start) + std::strlen(closeTag);
            if (content.compare(start, fullLen, newFullTag) != 0) {
                content.replace(start, fullLen, newFullTag);
                modified = true;
            }
        };

        replaceTag("<SSAAMultiplier>", "</SSAAMultiplier>", ssaaTag);
        if (upscalerBackend != 0 && upscaleMode >= 1) {
            // Ensure Cobra's internal FSR1/CAS upscaler and SMAA/FXAA are off when EDPE reconstructs.
            replaceTag("<UpscalingQuality>", "</UpscalingQuality>", "<UpscalingQuality>0</UpscalingQuality>");
            replaceTag("<AAMode>", "</AAMode>", "<AAMode>0</AAMode>");
        }

        if (!modified) return true;

        std::ofstream outFile(cfgPath, std::ios::binary | std::ios::trunc);
        if (!outFile) return false;
        outFile.write(content.data(), static_cast<std::streamsize>(content.size()));
        return outFile.good();
    }

    std::wstring getIniPath() const {
        wchar_t modulePath[MAX_PATH]{};
        const DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        if (!len || len >= MAX_PATH) return L"edpe.ini";
        std::filesystem::path p(modulePath);
        return (p.parent_path() / L"edpe.ini").wstring();
    }

    int upscalerBackend = 0;
    int upscaleMode = 0;
    int dlssPreset = 0; // 0=Auto(K), 10=J, 11=K, 12=L, 13=M
    float sharpness = 0.0f;
    bool autoSyncEliteConfig = true;
    bool preserveNativeUi = true;
    bool hdrInput = false;

    bool jitterInvertX = false;
    bool jitterInvertY = false;
    bool jitterSwapXY = false;
    float jitterScale = 1.0f;
    unsigned jitterPhaseCount = 32;

    bool jitterDebugView = false;
    float jitterDebugSensitivity = 1.5f;
    float jitterDebugThreshold = 0.02f;
    int jitterDebugMode = 0;

    bool zeroMotionVectors = false;
    bool motionInvertX = false;
    bool motionInvertY = false;
    float motionScale = 1.0f;
    int depthSource = 0; // 0 = Prefer Raw Depth (default), 1 = Prefer Linear Depth

    bool showPerfOverlay = false;
    int perfOverlayCorner = 0; // 0=Top-Left, 1=Top-Right, 2=Bottom-Left, 3=Bottom-Right
    float perfOverlayScale = 1.0f;
    float perfOverlayPosX = -1.0f;
    float perfOverlayPosY = -1.0f;
};

} // namespace edpe
