#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <windows.h>
#include "profiler.h"
#include "atomic_file.h"
#include "renderdoc_support.h"

namespace edpe {

extern const char* const kVersion;

inline bool isRunningInEliteProcess() {
    wchar_t modulePath[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (!len || len >= MAX_PATH) return false;
    std::wstring filename = std::filesystem::path(modulePath).filename().wstring();
    for (auto& ch : filename) ch = static_cast<wchar_t>(towlower(ch));
    return filename.find(L"elitedangerous") != std::wstring::npos;
}

// Used to apply Proton/Wine on Linux compatibility defaults (e.g. DirectInput keyboard proxy).
inline bool isRunningUnderWine() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

class Config {
public:
    static Config& instance() {
        static Config s_cfg;
        return s_cfg;
    }

    void load() {
        loadFrom(getIniPath());
    }

    void loadFrom(const std::wstring& path) {
        if (path.empty()) return;

        upscalerBackend = GetPrivateProfileIntW(L"Upscaler", L"Backend", 0, path.c_str());
        upscaleMode = GetPrivateProfileIntW(L"Upscaler", L"Mode", 0, path.c_str());
        dlssPreset = GetPrivateProfileIntW(L"Upscaler", L"Preset", 0, path.c_str());
        autoSyncEliteConfig = GetPrivateProfileIntW(L"Upscaler", L"AutoSyncEliteConfig", 1, path.c_str()) != 0;
        preserveNativeUi = GetPrivateProfileIntW(L"Upscaler", L"PreserveNativeUi", 1, path.c_str()) != 0;
        hdrInput = GetPrivateProfileIntW(L"Upscaler", L"HdrInput", 0, path.c_str()) != 0;
        fsr4 = GetPrivateProfileIntW(L"Upscaler", L"Fsr4", 0, path.c_str()) != 0;

        wchar_t sharpnessBuf[64]{};
        GetPrivateProfileStringW(L"Upscaler", L"Sharpness", L"0.0", sharpnessBuf, 64, path.c_str());
        sharpness = static_cast<float>(_wtof(sharpnessBuf));
        if (sharpness < 0.0f || sharpness > 1.0f) sharpness = 0.0f;

        wchar_t fsrVelBuf[64]{};
        GetPrivateProfileStringW(L"Upscaler", L"FsrVelocityFactor", L"0.00", fsrVelBuf, 64, path.c_str());
        fsrVelocityFactor = static_cast<float>(_wtof(fsrVelBuf));
        if (!std::isfinite(fsrVelocityFactor) || fsrVelocityFactor < 0.0f || fsrVelocityFactor > 1.0f) {
            fsrVelocityFactor = 0.00f;
        }

        wchar_t mipBuf[64]{};
        GetPrivateProfileStringW(L"Upscaler", L"MipLodBiasOffset", L"0.00", mipBuf, 64, path.c_str());
        mipLodBiasOffset = static_cast<float>(_wtof(mipBuf));
        if (!std::isfinite(mipLodBiasOffset) || mipLodBiasOffset < -2.0f || mipLodBiasOffset > 2.0f) {
            mipLodBiasOffset = 0.0f;
        }

        frameGenBackend = GetPrivateProfileIntW(L"FrameGeneration", L"Backend", 0, path.c_str());
        if (frameGenBackend < 0 || frameGenBackend > 2) frameGenBackend = 0;
        frameGenMultiplier = GetPrivateProfileIntW(L"FrameGeneration", L"Multiplier", 2, path.c_str());
        if (frameGenMultiplier < 2 || frameGenMultiplier > 4) frameGenMultiplier = 2;
        frameGenDynamicMfg = GetPrivateProfileIntW(L"FrameGeneration", L"DynamicMfg", 0, path.c_str()) != 0;
        wchar_t fgTargetBuf[64]{};
        GetPrivateProfileStringW(L"FrameGeneration", L"DynamicTargetFps", L"0.0", fgTargetBuf, 64, path.c_str());
        frameGenDynamicTargetFps = static_cast<float>(_wtof(fgTargetBuf));
        if (frameGenDynamicTargetFps < 0.0f || frameGenDynamicTargetFps > 1000.0f) frameGenDynamicTargetFps = 0.0f;
        frameGenReflexBoost = GetPrivateProfileIntW(L"FrameGeneration", L"ReflexBoost", 0, path.c_str()) != 0;

        wchar_t fpsLimitBuf[64]{};
        GetPrivateProfileStringW(L"FrameLimiter", L"GameFpsLimit", L"0.0", fpsLimitBuf, 64, path.c_str());
        gameFpsLimit = static_cast<float>(_wtof(fpsLimitBuf));
        if (!std::isfinite(gameFpsLimit) || gameFpsLimit <= 0.0f) {
            gameFpsLimit = 0.0f;
        } else {
            gameFpsLimit = std::clamp(gameFpsLimit, 10.0f, 500.0f);
        }

        jitterEnabled = GetPrivateProfileIntW(L"Jitter", L"Enabled", 1, path.c_str()) != 0;
        jitterInvertX = GetPrivateProfileIntW(L"Jitter", L"InvertX", 0, path.c_str()) != 0;
        jitterInvertY = GetPrivateProfileIntW(L"Jitter", L"InvertY", 0, path.c_str()) != 0;
        jitterSwapXY = GetPrivateProfileIntW(L"Jitter", L"SwapXY", 0, path.c_str()) != 0;

        wchar_t buf[64]{};
        GetPrivateProfileStringW(L"Jitter", L"Scale", L"1.0", buf, 64, path.c_str());
        jitterScale = static_cast<float>(_wtof(buf));
        if (jitterScale < 0.0f || jitterScale > 2.0f) jitterScale = 1.0f;

        jitterPhaseCount = GetPrivateProfileIntW(L"Jitter", L"PhaseCount", 32, path.c_str());
        wchar_t modeBuf[64]{};
        GetPrivateProfileStringW(L"Jitter", L"Mode", L"auto", modeBuf, 64, path.c_str());
        jitterMode = parseJitterModeString(modeBuf);
        jitterLogEnabled = GetPrivateProfileIntW(L"Jitter", L"LogEnabled", 0, path.c_str()) != 0;
        int logIntVal = GetPrivateProfileIntW(L"Jitter", L"LogInterval", 10, path.c_str());
        jitterLogInterval = (logIntVal < 1) ? 1u : (logIntVal > 600 ? 600u : static_cast<unsigned>(logIntVal));
        int logFlushVal = GetPrivateProfileIntW(L"Jitter", L"LogFlushMs", 1000, path.c_str());
        jitterLogFlushMs = (logFlushVal < 100) ? 100u : (logFlushVal > 10000 ? 10000u : static_cast<unsigned>(logFlushVal));

        jitterDebugView = GetPrivateProfileIntW(L"Jitter", L"DebugView", 0, path.c_str()) != 0;
        GetPrivateProfileStringW(L"Jitter", L"DebugSensitivity", L"1.5", buf, 64, path.c_str());
        jitterDebugSensitivity = static_cast<float>(_wtof(buf));
        if (jitterDebugSensitivity < 0.1f || jitterDebugSensitivity > 10.0f) jitterDebugSensitivity = 1.5f;
        GetPrivateProfileStringW(L"Jitter", L"DebugThreshold", L"0.02", buf, 64, path.c_str());
        jitterDebugThreshold = static_cast<float>(_wtof(buf));
        if (jitterDebugThreshold < 0.001f || jitterDebugThreshold > 0.5f) jitterDebugThreshold = 0.02f;
        jitterDebugMode = GetPrivateProfileIntW(L"Jitter", L"DebugMode", 0, path.c_str());
        if (jitterDebugMode < 0 || jitterDebugMode > 3) jitterDebugMode = 0;

        hudDebugView = GetPrivateProfileIntW(L"HUD", L"DebugView", 0, path.c_str()) != 0;
        GetPrivateProfileStringW(L"HUD", L"DebugOpacity", L"1.0", buf, 64, path.c_str());
        hudDebugOpacity = static_cast<float>(_wtof(buf));
        if (hudDebugOpacity < 0.1f || hudDebugOpacity > 5.0f) hudDebugOpacity = 1.0f;
        GetPrivateProfileStringW(L"HUD", L"DebugThreshold", L"0.02", buf, 64, path.c_str());
        hudDebugThreshold = static_cast<float>(_wtof(buf));
        if (hudDebugThreshold < 0.001f || hudDebugThreshold > 0.5f) hudDebugThreshold = 0.02f;
        hudDebugMode = GetPrivateProfileIntW(L"HUD", L"DebugMode", 0, path.c_str());
        if (hudDebugMode < 0 || hudDebugMode > 4) hudDebugMode = 0;

        zeroMotionVectors = GetPrivateProfileIntW(L"Motion", L"ZeroMotion", 0, path.c_str()) != 0;
        motionInvertX = GetPrivateProfileIntW(L"Motion", L"InvertX", 0, path.c_str()) != 0;
        motionInvertY = GetPrivateProfileIntW(L"Motion", L"InvertY", 0, path.c_str()) != 0;

        GetPrivateProfileStringW(L"Motion", L"Scale", L"1.0", buf, 64, path.c_str());
        motionScale = static_cast<float>(_wtof(buf));
        if (motionScale < 0.0f || motionScale > 2.0f) motionScale = 1.0f;

        showPerfOverlay = GetPrivateProfileIntW(L"Overlay", L"ShowPerfOverlay", 0, path.c_str()) != 0;
        captureOverlayInComparison = GetPrivateProfileIntW(L"Overlay", L"CaptureOverlayInComparison", 0, path.c_str()) != 0;
        enableProfiler = GetPrivateProfileIntW(L"Overlay", L"EnableProfiler", 0, path.c_str()) != 0;
        Profiler::instance().setEnabled(enableProfiler);
        perfOverlayCorner = GetPrivateProfileIntW(L"Overlay", L"Corner", 0, path.c_str());
        if (perfOverlayCorner < 0 || perfOverlayCorner > 7) perfOverlayCorner = 0;

        GetPrivateProfileStringW(L"Overlay", L"Scale", L"1.0", buf, 64, path.c_str());
        perfOverlayScale = static_cast<float>(_wtof(buf));
        if (perfOverlayScale < 0.5f || perfOverlayScale > 3.0f) perfOverlayScale = 1.0f;

        GetPrivateProfileStringW(L"Overlay", L"UiScale", L"1.0", buf, 64, path.c_str());
        uiScale = static_cast<float>(_wtof(buf));
        if (uiScale < 0.5f || uiScale > 2.5f) uiScale = 1.0f;

        GetPrivateProfileStringW(L"Overlay", L"PosX", L"-1.0", buf, 64, path.c_str());
        perfOverlayPosX = static_cast<float>(_wtof(buf));
        GetPrivateProfileStringW(L"Overlay", L"PosY", L"-1.0", buf, 64, path.c_str());
        perfOverlayPosY = static_cast<float>(_wtof(buf));

        GetPrivateProfileStringW(L"Overlay", L"MenuPosX", L"-1.0", buf, 64, path.c_str());
        menuPosX = static_cast<float>(_wtof(buf));
        GetPrivateProfileStringW(L"Overlay", L"MenuPosY", L"-1.0", buf, 64, path.c_str());
        menuPosY = static_cast<float>(_wtof(buf));
        GetPrivateProfileStringW(L"Overlay", L"MenuWidth", L"-1.0", buf, 64, path.c_str());
        menuWidth = static_cast<float>(_wtof(buf));
        GetPrivateProfileStringW(L"Overlay", L"MenuHeight", L"-1.0", buf, 64, path.c_str());
        menuHeight = static_cast<float>(_wtof(buf));

        int devVal = GetPrivateProfileIntW(L"Overlay", L"DEV", -1, path.c_str());
        if (devVal < 0) {
            devVal = GetPrivateProfileIntW(L"General", L"DEV", 0, path.c_str());
        }
        devMode = (devVal > 0);

        debugViewActive = GetPrivateProfileIntW(L"Pipeline", L"DebugView", 0, path.c_str()) != 0;
        wchar_t bounceBuf[64]{};
        GetPrivateProfileStringW(L"Pipeline", L"MapBounceMode", L"auto", bounceBuf, 64, path.c_str());
        mapBounceMode = parseMapBounceModeString(bounceBuf);

        renderDocAnnotations = GetPrivateProfileIntW(L"RenderDoc", L"EnableAnnotations", 1, path.c_str()) != 0;
        // Allow forcing D3D11/D3D12 debug markers & object names when external tools (e.g., Linux
        // RenderDoc via ENABLE_VULKAN_RENDERDOC_CAPTURE=1 + DXVK_DEBUG=markers) attach without
        // loading Windows renderdoc.dll into the process.
        renderDocForceAnnotations = GetPrivateProfileIntW(L"RenderDoc", L"ForceAnnotations", 0, path.c_str()) != 0;
        RenderDocBridge::instance().setAnnotationsEnabled(renderDocAnnotations);
        RenderDocBridge::instance().setForceAnnotations(renderDocForceAnnotations);
        RenderDocBridge::instance().setForceAnnotations(renderDocForceAnnotations);

        wrapDirectInputMouse = GetPrivateProfileIntW(L"Input", L"WrapDirectInputMouse", 1, path.c_str()) != 0;
        // Default SysKeyboard proxy off under Proton/Wine on Linux so remapped/virtual keys work reliably.
        const int defaultWrapKeyboard = isRunningUnderWine() ? 0 : 1;
        wrapDirectInputKeyboard = GetPrivateProfileIntW(L"Input", L"WrapDirectInputKeyboard", defaultWrapKeyboard, path.c_str()) != 0;

        wchar_t chainBuf[MAX_PATH]{};
        GetPrivateProfileStringW(L"Proxy", L"ChainDxgiDll", L"", chainBuf, MAX_PATH, path.c_str());
        std::wstring chainVal(chainBuf);
        while (!chainVal.empty() && std::iswspace(chainVal.front())) chainVal.erase(chainVal.begin());
        while (!chainVal.empty() && std::iswspace(chainVal.back())) chainVal.pop_back();
        chainDxgiDll = std::move(chainVal);
    }

    void save() {
        saveTo(getIniPath());
    }

    std::string serializeIni() const {
        std::string out;
        out.reserve(2048);
        auto appendSection = [&](const char* name) {
            if (!out.empty()) out += "\r\n";
            out += '[';
            out += name;
            out += "]\r\n";
        };
        auto appendInt = [&](const char* key, int val) {
            char line[96]{};
            std::snprintf(line, sizeof(line), "%s=%d\r\n", key, val);
            out += line;
        };
        auto appendUInt = [&](const char* key, unsigned val) {
            char line[96]{};
            std::snprintf(line, sizeof(line), "%s=%u\r\n", key, val);
            out += line;
        };
        auto appendBool = [&](const char* key, bool val) {
            out += key;
            out += val ? "=1\r\n" : "=0\r\n";
        };
        auto appendFloat = [&](const char* key, const char* fmt, float val) {
            char valBuf[64]{};
            std::snprintf(valBuf, sizeof(valBuf), fmt, val);
            out += key;
            out += '=';
            out += valBuf;
            out += "\r\n";
        };
        auto appendWide = [&](const char* key, const wchar_t* val) {
            out += key;
            out += '=';
            if (val && val[0]) {
                const int needed = WideCharToMultiByte(CP_UTF8, 0, val, -1, nullptr, 0, nullptr, nullptr);
                if (needed > 1) {
                    std::string utf8(static_cast<size_t>(needed - 1), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, val, -1, utf8.data(), needed, nullptr, nullptr);
                    out += utf8;
                }
            }
            out += "\r\n";
        };

        appendSection("Upscaler");
        appendInt("Backend", upscalerBackend);
        appendInt("Mode", upscaleMode);
        appendInt("Preset", dlssPreset);
        appendBool("AutoSyncEliteConfig", autoSyncEliteConfig);
        appendBool("PreserveNativeUi", preserveNativeUi);
        appendBool("HdrInput", hdrInput);
        appendBool("Fsr4", fsr4);
        appendFloat("Sharpness", "%.2f", sharpness);
        appendFloat("FsrVelocityFactor", "%.2f", fsrVelocityFactor);
        appendFloat("MipLodBiasOffset", "%.2f", mipLodBiasOffset);

        appendSection("FrameGeneration");
        appendInt("Backend", frameGenBackend);
        appendInt("Multiplier", frameGenMultiplier);
        appendBool("DynamicMfg", frameGenDynamicMfg);
        appendFloat("DynamicTargetFps", "%.1f", frameGenDynamicTargetFps);
        appendBool("ReflexBoost", frameGenReflexBoost);

        appendSection("FrameLimiter");
        appendFloat("GameFpsLimit", "%.1f", gameFpsLimit);

        appendSection("Jitter");
        appendBool("Enabled", jitterEnabled);
        appendBool("InvertX", jitterInvertX);
        appendBool("InvertY", jitterInvertY);
        appendBool("SwapXY", jitterSwapXY);
        appendFloat("Scale", "%.2f", jitterScale);
        appendUInt("PhaseCount", jitterPhaseCount);
        appendWide("Mode", jitterModeToString(jitterMode));
        appendBool("LogEnabled", jitterLogEnabled);
        appendUInt("LogInterval", jitterLogInterval);
        appendUInt("LogFlushMs", jitterLogFlushMs);
        appendBool("DebugView", jitterDebugView);
        appendFloat("DebugSensitivity", "%.2f", jitterDebugSensitivity);
        appendFloat("DebugThreshold", "%.3f", jitterDebugThreshold);
        appendInt("DebugMode", jitterDebugMode);

        appendSection("HUD");
        appendBool("DebugView", hudDebugView);
        appendFloat("DebugOpacity", "%.2f", hudDebugOpacity);
        appendFloat("DebugThreshold", "%.3f", hudDebugThreshold);
        appendInt("DebugMode", hudDebugMode);

        appendSection("Motion");
        appendBool("ZeroMotion", zeroMotionVectors);
        appendBool("InvertX", motionInvertX);
        appendBool("InvertY", motionInvertY);
        appendFloat("Scale", "%.2f", motionScale);

        appendSection("Overlay");
        appendBool("ShowPerfOverlay", showPerfOverlay);
        appendBool("CaptureOverlayInComparison", captureOverlayInComparison);
        appendBool("EnableProfiler", enableProfiler);
        appendInt("Corner", perfOverlayCorner);
        appendFloat("Scale", "%.2f", perfOverlayScale);
        appendFloat("UiScale", "%.2f", uiScale);
        appendFloat("PosX", "%.1f", perfOverlayPosX);
        appendFloat("PosY", "%.1f", perfOverlayPosY);
        appendFloat("MenuPosX", "%.1f", menuPosX);
        appendFloat("MenuPosY", "%.1f", menuPosY);
        appendFloat("MenuWidth", "%.1f", menuWidth);
        appendFloat("MenuHeight", "%.1f", menuHeight);
        appendBool("DEV", devMode);

        appendSection("Pipeline");
        appendBool("DebugView", debugViewActive);
        appendWide("MapBounceMode", mapBounceModeToString(mapBounceMode));

        appendSection("RenderDoc");
        appendBool("EnableAnnotations", renderDocAnnotations);
        appendBool("ForceAnnotations", renderDocForceAnnotations);

        appendSection("Input");
        appendBool("WrapDirectInputMouse", wrapDirectInputMouse);
        appendBool("WrapDirectInputKeyboard", wrapDirectInputKeyboard);

        appendSection("Proxy");
        appendWide("ChainDxgiDll", chainDxgiDll.c_str());

        return out;
    }

    bool saveTo(const std::wstring& path) {
        if (path.empty()) return false;
        Profiler::instance().setEnabled(enableProfiler);
        RenderDocBridge::instance().setAnnotationsEnabled(renderDocAnnotations);
        return writeFileAtomic(path, serializeIni());
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

    static std::filesystem::path resolveEliteGraphicsConfigPath() {
        wchar_t localAppData[MAX_PATH]{};
        const DWORD envLen = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
        if (!envLen || envLen >= MAX_PATH) return {};

        std::filesystem::path gfxDir = std::filesystem::path(localAppData) /
            L"Frontier Developments" / L"Elite Dangerous" / L"Options" / L"Graphics";
        std::filesystem::path cfgPath = gfxDir / L"Custom.4.4.fxcfg";
        std::error_code ec;
        if (!std::filesystem::exists(cfgPath, ec)) {
            cfgPath = gfxDir / L"Custom.4.0.fxcfg";
            if (!std::filesystem::exists(cfgPath, ec)) return {};
        }
        return cfgPath;
    }

    static float parseEliteSsaaMultiplierXml(const std::string& content) {
        constexpr char kOpenTag[] = "<SSAAMultiplier>";
        constexpr char kCloseTag[] = "</SSAAMultiplier>";
        const size_t start = content.find(kOpenTag);
        if (start == std::string::npos) return 0.0f;
        const size_t valStart = start + sizeof(kOpenTag) - 1;
        const size_t end = content.find(kCloseTag, valStart);
        if (end == std::string::npos || end <= valStart) return 0.0f;
        const std::string numStr = content.substr(valStart, end - valStart);
        char* parseEnd = nullptr;
        const float val = std::strtof(numStr.c_str(), &parseEnd);
        if (parseEnd == numStr.c_str() || !std::isfinite(val) || val <= 0.0f || val > 4.0f) {
            return 0.0f;
        }
        return val;
    }

    static float readEliteSsaaMultiplier() {
        if (!edpe::isRunningInEliteProcess()) return 0.0f;
        const std::filesystem::path cfgPath = resolveEliteGraphicsConfigPath();
        if (cfgPath.empty()) return 0.0f;
        std::ifstream inFile(cfgPath, std::ios::binary);
        if (!inFile) return 0.0f;
        std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
        return parseEliteSsaaMultiplierXml(content);
    }

    static float computeEffectiveMipLodBias(int backend, int mode, float eliteSsaaMultiplier, float userOffset) {
        if (backend == 0) return 0.0f;
        const float ratio = (std::isfinite(eliteSsaaMultiplier) && eliteSsaaMultiplier > 0.0f && eliteSsaaMultiplier < 1.0f)
            ? eliteSsaaMultiplier
            : getTargetSsaaMultiplier(backend, mode);
        const float baseBias = (ratio > 0.0f && ratio < 1.0f) ? std::log2(ratio) : 0.0f;
        const float cleanOffset = std::isfinite(userOffset) ? std::clamp(userOffset, -2.0f, 2.0f) : 0.0f;
        return std::clamp(baseBias + cleanOffset, -3.0f, 2.0f);
    }

    bool isRunningInEliteProcess() const {
        return edpe::isRunningInEliteProcess();
    }

    // Synchronizes Elite Dangerous's Custom.4.4.fxcfg <SSAAMultiplier> with the selected
    // EDPE UpscaleMode so Cobra renders the 3D scene at true lower internal resolution.
    bool syncEliteGraphicsConfig() const {
        if (!autoSyncEliteConfig || !isRunningInEliteProcess()) return false;

        const std::filesystem::path cfgPath = resolveEliteGraphicsConfigPath();
        if (cfgPath.empty()) return false;

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

        return writeFileAtomic(cfgPath, content);
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
    int dlssPreset = 0; // 0=Auto(K), 5=E(CNN Fast), 10=J, 11=K, 12=L, 13=M
    float sharpness = 0.0f;
    float fsrVelocityFactor = 0.00f;
    float mipLodBiasOffset = 0.0f;
    bool autoSyncEliteConfig = true;
    bool preserveNativeUi = true;
    bool hdrInput = false;
    bool fsr4 = false; // [Upscaler] Fsr4=1: start on FSR 4 (D3D12 bridge) in the AMD FSR backend

    int frameGenBackend = 0; // 0=Off, 1=AMD FSR FG, 2=NVIDIA DLSS-G
    int frameGenMultiplier = 2; // 2..4 (clamped by backend capability)
    bool frameGenDynamicMfg = false;
    float frameGenDynamicTargetFps = 0.0f;
    bool frameGenReflexBoost = false;

    float gameFpsLimit = 0.0f;

    static int parseJitterModeString(const wchar_t* s) {
        if (!s || !s[0]) return 0;
        if (_wcsicmp(s, L"d3d11") == 0 || wcscmp(s, L"1") == 0) return 1;
        if (_wcsicmp(s, L"upstream") == 0 || wcscmp(s, L"2") == 0) return 2;
        return 0; // auto
    }

    static const wchar_t* jitterModeToString(int mode) {
        switch (mode) {
        case 1: return L"d3d11";
        case 2: return L"upstream";
        default: return L"auto";
        }
    }

    bool jitterEnabled = true;
    bool jitterInvertX = false;
    bool jitterInvertY = false;
    bool jitterSwapXY = false;
    float jitterScale = 1.0f;
    unsigned jitterPhaseCount = 32;
    int jitterMode = 0; // 0=auto (upstream->d3d11 fallback), 1=d3d11, 2=upstream
    bool jitterLogEnabled = false;
    unsigned jitterLogInterval = 10;
    unsigned jitterLogFlushMs = 1000;

    bool jitterDebugView = false;
    float jitterDebugSensitivity = 1.5f;
    float jitterDebugThreshold = 0.02f;
    int jitterDebugMode = 0;

    bool hudDebugView = false;
    float hudDebugOpacity = 1.0f;
    float hudDebugThreshold = 0.02f;
    int hudDebugMode = 0;

    bool zeroMotionVectors = false;
    bool motionInvertX = false;
    bool motionInvertY = false;
    float motionScale = 1.0f;

    bool showPerfOverlay = false;
    bool captureOverlayInComparison = false;
    bool enableProfiler = false;
    int perfOverlayCorner = 0; // 0=Top-Left, 1=Top, 2=Top-Right, 3=Left, 4=Right, 5=Bottom-Left, 6=Bottom, 7=Bottom-Right
    float perfOverlayScale = 1.0f;
    float uiScale = 1.0f;
    float perfOverlayPosX = -1.0f;
    float perfOverlayPosY = -1.0f;
    float menuPosX = -1.0f;
    float menuPosY = -1.0f;
    float menuWidth = -1.0f;
    float menuHeight = -1.0f;
    bool devMode = false;

    bool debugViewActive = false;
    int mapBounceMode = 0; // 0=auto, 1=on, 2=off

    static int parseMapBounceModeString(const wchar_t* s) {
        if (!s || !s[0]) return 0;
        if (_wcsicmp(s, L"on") == 0 || wcscmp(s, L"1") == 0) return 1;
        if (_wcsicmp(s, L"off") == 0 || wcscmp(s, L"2") == 0) return 2;
        return 0; // auto
    }

    static const wchar_t* mapBounceModeToString(int mode) {
        switch (mode) {
        case 1: return L"on";
        case 2: return L"off";
        default: return L"auto";
        }
    }

    bool renderDocAnnotations = true;
    bool renderDocForceAnnotations = false;

    bool wrapDirectInputMouse = true;
    bool wrapDirectInputKeyboard = !isRunningUnderWine();

    std::wstring chainDxgiDll;
};

} // namespace edpe
