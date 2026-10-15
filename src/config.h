#pragma once

#include <string>
#include <filesystem>
#include <cmath>
#include <windows.h>

namespace edpe {

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

        jitterInvertX = GetPrivateProfileIntW(L"Jitter", L"InvertX", 0, path.c_str()) != 0;
        jitterInvertY = GetPrivateProfileIntW(L"Jitter", L"InvertY", 0, path.c_str()) != 0;
        jitterSwapXY = GetPrivateProfileIntW(L"Jitter", L"SwapXY", 0, path.c_str()) != 0;

        wchar_t buf[64]{};
        GetPrivateProfileStringW(L"Jitter", L"Scale", L"1.0", buf, 64, path.c_str());
        jitterScale = static_cast<float>(_wtof(buf));
        if (jitterScale < 0.0f || jitterScale > 2.0f) jitterScale = 1.0f;

        jitterPhaseCount = GetPrivateProfileIntW(L"Jitter", L"PhaseCount", 32, path.c_str());

        zeroMotionVectors = GetPrivateProfileIntW(L"Motion", L"ZeroMotion", 0, path.c_str()) != 0;
        motionInvertX = GetPrivateProfileIntW(L"Motion", L"InvertX", 0, path.c_str()) != 0;
        motionInvertY = GetPrivateProfileIntW(L"Motion", L"InvertY", 0, path.c_str()) != 0;

        GetPrivateProfileStringW(L"Motion", L"Scale", L"1.0", buf, 64, path.c_str());
        motionScale = static_cast<float>(_wtof(buf));
        if (motionScale < 0.0f || motionScale > 2.0f) motionScale = 1.0f;
    }

    void save() {
        const std::wstring path = getIniPath();
        if (path.empty()) return;

        WritePrivateProfileStringW(L"Upscaler", L"Backend", std::to_wstring(upscalerBackend).c_str(), path.c_str());
        WritePrivateProfileStringW(L"Upscaler", L"Mode", std::to_wstring(upscaleMode).c_str(), path.c_str());

        WritePrivateProfileStringW(L"Jitter", L"InvertX", jitterInvertX ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Jitter", L"InvertY", jitterInvertY ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Jitter", L"SwapXY", jitterSwapXY ? L"1" : L"0", path.c_str());

        wchar_t buf[64]{};
        swprintf_s(buf, L"%.2f", jitterScale);
        WritePrivateProfileStringW(L"Jitter", L"Scale", buf, path.c_str());
        WritePrivateProfileStringW(L"Jitter", L"PhaseCount", std::to_wstring(jitterPhaseCount).c_str(), path.c_str());

        WritePrivateProfileStringW(L"Motion", L"ZeroMotion", zeroMotionVectors ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Motion", L"InvertX", motionInvertX ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Motion", L"InvertY", motionInvertY ? L"1" : L"0", path.c_str());
        swprintf_s(buf, L"%.2f", motionScale);
        WritePrivateProfileStringW(L"Motion", L"Scale", buf, path.c_str());
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

    bool jitterInvertX = false;
    bool jitterInvertY = false;
    bool jitterSwapXY = false;
    float jitterScale = 1.0f;
    unsigned jitterPhaseCount = 32;

    bool zeroMotionVectors = false;
    bool motionInvertX = false;
    bool motionInvertY = false;
    float motionScale = 1.0f;
};

} // namespace edpe
