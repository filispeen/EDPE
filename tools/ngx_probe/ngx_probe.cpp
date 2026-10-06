// ngx_probe: standalone NGX lifecycle probe (child process).
// Real NGX SDK calls on a real hardware adapter. No stubs, no WARP, no fallbacks.
// One flushed log line per stage; run through ngx_probe_host for the timeout.
//
// Usage: ngx_probe.exe --variant N --log <file> [--appdata <dir>]

#define NGX_ENABLE_DEPRECATED_SHUTDOWN  // exposes NVSDK_NGX_D3D11_Shutdown() for variant V2

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_d3d.h"
#include "probe_common.h"

// EDPE's own NGX project ID for NVSDK_NGX_D3D11_Init_with_ProjectID.
// Generated for EDPE with PowerShell New-Guid on 2026-10-05; it is not EDVR's ID
// and not the example GUID from nvsdk_ngx.h. Must be GUID-like, no braces.
// Keep identical to kNgxProjectId in src/ngx_context.cpp.
static const char kNgxProjectId[] = "48d353f3-d07b-4048-876b-09f8622f5a27";
static const char kNgxEngineVersion[] = "EDPE-unreleased";

// DLAA: render size == output size.
static const unsigned kWidth = 2560;
static const unsigned kHeight = 1440;

static FILE* g_log = nullptr;
static LARGE_INTEGER g_freq;
static LARGE_INTEGER g_t0;

static double elapsed_ms() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

static std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static void write_line(const char* stage, const char* status, unsigned code, const char* detail) {
    char line[1024];
    snprintf(line, sizeof(line), "%.1f|%s|%s|0x%08X|%s", elapsed_ms(), stage, status, code, detail);
    printf("%s\n", line);
    fflush(stdout);
    if (g_log) {
        fprintf(g_log, "%s\n", line);
        fflush(g_log);  // flushed per line so a kill by the parent loses nothing
    }
}

static void info(const char* fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write_line("info", "ok", 0, buf);
}

// Logs "ok" only when `success` is true, i.e. the real call returned success.
static bool stage(int s, bool success, unsigned code, const char* fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write_line(kStageNames[s], success ? "ok" : "fail", code, buf);
    return success;
}

static void stage_skip(int s, const char* reason) {
    write_line(kStageNames[s], "skip", 0, reason);
}

static std::wstring exe_dir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

static std::string file_version(const std::wstring& path) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return "not found";
    std::vector<char> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return "unreadable";
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", (void**)&fi, &len) || !fi) return "no version info";
    char buf[64];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
             HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    return buf;
}

static int finish(int code) {
    if (g_log) {
        fclose(g_log);
        g_log = nullptr;
    }
    return code;
}

struct EvalTextures {
    ID3D11Texture2D* color = nullptr;
    ID3D11Texture2D* depth = nullptr;
    ID3D11Texture2D* motion = nullptr;
    ID3D11Texture2D* output = nullptr;
    void release() {
        ID3D11Texture2D** all[] = {&color, &depth, &motion, &output};
        for (auto** t : all) {
            if (*t) (*t)->Release();
            *t = nullptr;
        }
    }
};

static HRESULT make_texture(ID3D11Device* dev, DXGI_FORMAT fmt, UINT bind, const void* init, UINT pitch,
                            ID3D11Texture2D** out) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = kWidth;
    d.Height = kHeight;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = init;
    sd.SysMemPitch = pitch;
    return dev->CreateTexture2D(&d, init ? &sd : nullptr, out);
}

int wmain(int argc, wchar_t** argv) {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);

    int variant = 0;
    std::wstring log_path;
    std::wstring appdata;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--variant") && i + 1 < argc) variant = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--log") && i + 1 < argc) log_path = argv[++i];
        else if (!wcscmp(argv[i], L"--appdata") && i + 1 < argc) appdata = argv[++i];
    }
    if (variant < VariantV1 || variant > VariantV5 || log_path.empty()) {
        fprintf(stderr, "usage: ngx_probe --variant 1..5 --log <file> [--appdata <dir>]\n");
        return kExitSetupError;
    }
    g_log = _wfopen(log_path.c_str(), L"ab");
    if (!g_log) {
        fprintf(stderr, "cannot open log file\n");
        return kExitSetupError;
    }
    if (appdata.empty()) appdata = exe_dir();
    const std::wstring dll_dir = exe_dir();

    info("variant=%s", kVariantNames[variant]);
    info("sdk_api_version=0x%07X project_id=%s engine_version=%s", (unsigned)NVSDK_NGX_Version_API, kNgxProjectId,
         kNgxEngineVersion);
    info("nvngx_dlss.dll version=%s", file_version(dll_dir + L"\\nvngx_dlss.dll").c_str());

    // a) D3D11 device on the first hardware DXGI adapter. Never WARP.
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    if (FAILED(hr)) {
        stage(StageCreateDevice, false, (unsigned)hr, "CreateDXGIFactory1 failed");
        return finish(kExitSetupError);
    }
    IDXGIAdapter1* adapter = nullptr;
    DXGI_ADAPTER_DESC1 desc = {};
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d = {};
        a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            a->Release();
            continue;
        }
        adapter = a;
        desc = d;
        break;
    }
    factory->Release();
    if (!adapter) {
        stage(StageCreateDevice, false, (unsigned)DXGI_ERROR_NOT_FOUND, "no hardware DXGI adapter, aborting (no WARP)");
        return finish(kExitSetupError);
    }
    LARGE_INTEGER umd = {};
    adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
    char drv[96];
    unsigned c = HIWORD(umd.LowPart), d4 = LOWORD(umd.LowPart);
    snprintf(drv, sizeof(drv), "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart), c, d4);
    char nv[32] = "";
    if (desc.VendorId == 0x10DE) {  // NVIDIA: last five digits of c+d4 form the marketing version
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%u%04u", c, d4);
        size_t n = strlen(tmp);
        const char* t5 = n > 5 ? tmp + n - 5 : tmp;
        snprintf(nv, sizeof(nv), " (NVIDIA %.3s.%s)", t5, t5 + 3);
    }
    info("adapter=%s vendor=0x%04X device=0x%04X driver_umd=%s%s vram_mb=%llu", narrow(desc.Description).c_str(),
         desc.VendorId, desc.DeviceId, drv, nv, (unsigned long long)(desc.DedicatedVideoMemory >> 20));

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = {};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &dev, &fl,
                           &ctx);
    adapter->Release();
    if (!stage(StageCreateDevice, SUCCEEDED(hr), (unsigned)hr, "D3D11CreateDevice feature_level=0x%X", (unsigned)fl))
        return finish(kExitStageFailed);

    // b) NGX init with the EDPE project ID
    const wchar_t* paths[1] = {dll_dir.c_str()};
    NVSDK_NGX_FeatureCommonInfo common = {};
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = 1;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(kNgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                              kNgxEngineVersion, appdata.c_str(), dev, &common,
                                                              NVSDK_NGX_Version_API);
    if (!stage(StageNgxInit, r == NVSDK_NGX_Result_Success, (unsigned)r, "NVSDK_NGX_D3D11_Init_with_ProjectID"))
        return finish(kExitStageFailed);

    // c) capability parameters and DLSS availability
    NVSDK_NGX_Parameter* params = nullptr;
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&params);
    int available = -1, init_result = -1;
    if (r == NVSDK_NGX_Result_Success && params) {
        NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);
    }
    if (!stage(StageCapability, r == NVSDK_NGX_Result_Success && params && available == 1, (unsigned)r,
               "GetCapabilityParameters SuperSampling.Available=%d FeatureInitResult=0x%X", available,
               (unsigned)init_result))
        return finish(kExitStageFailed);

    // d) optimal settings for DLAA at 2560x1440
    unsigned ow = 0, oh = 0, maxw = 0, maxh = 0, minw = 0, minh = 0;
    float sharp = 0.0f;
    r = NGX_DLSS_GET_OPTIMAL_SETTINGS(params, kWidth, kHeight, NVSDK_NGX_PerfQuality_Value_DLAA, &ow, &oh, &maxw,
                                      &maxh, &minw, &minh, &sharp);
    if (!stage(StageOptimalSettings, r == NVSDK_NGX_Result_Success, (unsigned)r,
               "GET_OPTIMAL_SETTINGS DLAA %ux%u -> optimal %ux%u max %ux%u min %ux%u", kWidth, kHeight, ow, oh, maxw,
               maxh, minw, minh))
        return finish(kExitStageFailed);

    // e) create feature (DLAA: render == output). MVLowRes matches EDPE's render-resolution motion.
    NVSDK_NGX_Handle* feature = nullptr;
    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = kWidth;
    cp.Feature.InHeight = kHeight;
    cp.Feature.InTargetWidth = kWidth;
    cp.Feature.InTargetHeight = kHeight;
    cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &feature, params, &cp);
    if (!stage(StageCreateFeature, r == NVSDK_NGX_Result_Success && feature, (unsigned)r,
               "NGX_D3D11_CREATE_DLSS_EXT flags=MVLowRes|AutoExposure"))
        return finish(kExitStageFailed);

    // f) evaluate on synthetic inputs (flat gray color, constant depth, zero motion, zero jitter)
    EvalTextures tex;
    if (variant == VariantV5) {
        stage_skip(StageEvaluate, "skipped by variant V5");
    } else {
        const size_t px = (size_t)kWidth * kHeight;
        std::vector<unsigned char> color(px * 4, 0x80);
        std::vector<float> depth(px, 0.5f);
        std::vector<unsigned char> motion(px * 4, 0);
        HRESULT th = make_texture(dev, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, color.data(),
                                  kWidth * 4, &tex.color);
        if (SUCCEEDED(th))
            th = make_texture(dev, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, depth.data(), kWidth * 4,
                              &tex.depth);
        if (SUCCEEDED(th))
            th = make_texture(dev, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, motion.data(), kWidth * 4,
                              &tex.motion);
        if (SUCCEEDED(th))
            th = make_texture(dev, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0, &tex.output);
        if (FAILED(th)) {
            stage(StageEvaluate, false, (unsigned)th, "synthetic texture creation failed");
            return finish(kExitStageFailed);
        }
        NVSDK_NGX_D3D11_DLSS_Eval_Params ep = {};
        ep.Feature.pInColor = tex.color;
        ep.Feature.pInOutput = tex.output;
        ep.pInDepth = tex.depth;
        ep.pInMotionVectors = tex.motion;
        ep.InJitterOffsetX = 0.0f;
        ep.InJitterOffsetY = 0.0f;
        ep.InRenderSubrectDimensions.Width = kWidth;
        ep.InRenderSubrectDimensions.Height = kHeight;
        ep.InReset = 1;
        r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, feature, params, &ep);
        if (!stage(StageEvaluate, r == NVSDK_NGX_Result_Success, (unsigned)r,
                   "NGX_D3D11_EVALUATE_DLSS_EXT synthetic inputs, reset=1"))
            return finish(kExitStageFailed);
    }

    // g) release feature
    r = NVSDK_NGX_D3D11_ReleaseFeature(feature);
    tex.release();
    if (!stage(StageReleaseFeature, r == NVSDK_NGX_Result_Success, (unsigned)r, "NVSDK_NGX_D3D11_ReleaseFeature"))
        return finish(kExitStageFailed);

    // h) release parameters
    r = NVSDK_NGX_D3D11_DestroyParameters(params);
    if (!stage(StageReleaseParams, r == NVSDK_NGX_Result_Success, (unsigned)r, "NVSDK_NGX_D3D11_DestroyParameters"))
        return finish(kExitStageFailed);

    // i) context Flush and sync (variant V3 only)
    if (variant == VariantV3) {
        ctx->Flush();
        ID3D11Query* q = nullptr;
        D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
        HRESULT qh = dev->CreateQuery(&qd, &q);
        bool synced = false;
        if (SUCCEEDED(qh)) {
            ctx->End(q);
            ctx->Flush();
            const double deadline = elapsed_ms() + 5000.0;
            BOOL done = FALSE;
            while (elapsed_ms() < deadline) {
                if (ctx->GetData(q, &done, sizeof(done), 0) == S_OK && done) {
                    synced = true;
                    break;
                }
                Sleep(1);
            }
            q->Release();
        }
        if (!stage(StageFlushSync, synced, (unsigned)qh, "Flush + event query wait (5 s limit)"))
            return finish(kExitStageFailed);
    } else {
        stage_skip(StageFlushSync, "only variant V3 flushes and syncs");
    }

    // j) NGX shutdown, form chosen by variant. This is where a hang is expected.
    const bool use_old_shutdown = (variant == VariantV2);
    r = use_old_shutdown ? NVSDK_NGX_D3D11_Shutdown() : NVSDK_NGX_D3D11_Shutdown1(dev);
    if (!stage(StageShutdown, r == NVSDK_NGX_Result_Success, (unsigned)r, "%s",
               use_old_shutdown ? "NVSDK_NGX_D3D11_Shutdown()" : "NVSDK_NGX_D3D11_Shutdown1(device)"))
        return finish(kExitStageFailed);

    // k) release device (V4 keeps it alive for 2 s after shutdown first)
    if (variant == VariantV4) {
        info("holding device alive for 2000 ms after shutdown");
        Sleep(2000);
    }
    ctx->Release();
    ULONG left = dev->Release();
    stage(StageReleaseDevice, true, 0, "device released, remaining refs=%lu", left);

    info("probe finished: all stages completed");
    return finish(kExitOk);
}
