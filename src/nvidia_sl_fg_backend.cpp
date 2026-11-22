#include "nvidia_sl_fg_backend.h"

#include "log.h"
#include "renderdoc_support.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#if defined(EDPE_ENABLE_STREAMLINE) && EDPE_ENABLE_STREAMLINE
#include <sl.h>
#include <sl_consts.h>
#include <sl_core_api.h>
#include <sl_core_types.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_result.h>
#include <sl_security.h>
#include <sl_version.h>
#endif

#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace edpe {

namespace {

// EDPE-owned Project ID (also used for NGX DLSS SR; never reused from EDVR).
constexpr const char* kEdpeProjectId = "48d353f3-d07b-4048-876b-09f8622f5a27";

bool isRunningUnderWine() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && (GetProcAddress(ntdll, "wine_get_version") != nullptr);
}

std::wstring directoryOfModule(HMODULE mod) {
    wchar_t buf[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(mod, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    std::wstring s(buf, len);
    const size_t slash = s.find_last_of(L"\\/");
    return (slash != std::wstring::npos) ? s.substr(0, slash) : std::wstring{};
}

std::wstring findInterposerPath(std::wstring* outPluginDir) {
    HMODULE selfMod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&findInterposerPath),
        &selfMod);

    const std::wstring selfDir = directoryOfModule(selfMod);
    const std::wstring exeDir = directoryOfModule(nullptr);

    const std::wstring candidates[] = {selfDir, exeDir};
    for (const auto& dir : candidates) {
        if (dir.empty()) continue;
        const std::wstring dllPath = dir + L"\\sl.interposer.dll";
        const DWORD attrs = GetFileAttributesW(dllPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            if (outPluginDir) *outPluginDir = dir;
            return dllPath;
        }
    }
    return {};
}

#if defined(EDPE_ENABLE_STREAMLINE) && EDPE_ENABLE_STREAMLINE

const char* slResultToString(sl::Result r) {
    switch (r) {
    case sl::Result::eOk: return "eOk";
    case sl::Result::eErrorIO: return "eErrorIO";
    case sl::Result::eErrorDriverOutOfDate: return "eErrorDriverOutOfDate";
    case sl::Result::eErrorOSOutOfDate: return "eErrorOSOutOfDate";
    case sl::Result::eErrorOSDisabledHWS: return "eErrorOSDisabledHWS";
    case sl::Result::eErrorDeviceNotCreated: return "eErrorDeviceNotCreated";
    case sl::Result::eErrorNoSupportedAdapterFound: return "eErrorNoSupportedAdapterFound";
    case sl::Result::eErrorAdapterNotSupported: return "eErrorAdapterNotSupported";
    case sl::Result::eErrorNoPlugins: return "eErrorNoPlugins";
    case sl::Result::eErrorVulkanAPI: return "eErrorVulkanAPI";
    case sl::Result::eErrorDXGIAPI: return "eErrorDXGIAPI";
    case sl::Result::eErrorD3DAPI: return "eErrorD3DAPI";
    case sl::Result::eErrorNVAPI: return "eErrorNVAPI";
    case sl::Result::eErrorReflexAPI: return "eErrorReflexAPI";
    case sl::Result::eErrorNGXFailed: return "eErrorNGXFailed";
    case sl::Result::eErrorJSONParsing: return "eErrorJSONParsing";
    case sl::Result::eErrorMissingProxy: return "eErrorMissingProxy";
    case sl::Result::eErrorMissingResourceState: return "eErrorMissingResourceState";
    case sl::Result::eErrorInvalidIntegration: return "eErrorInvalidIntegration";
    case sl::Result::eErrorMissingInputParameter: return "eErrorMissingInputParameter";
    case sl::Result::eErrorNotInitialized: return "eErrorNotInitialized";
    case sl::Result::eErrorComputeFailed: return "eErrorComputeFailed";
    case sl::Result::eErrorInitNotCalled: return "eErrorInitNotCalled";
    case sl::Result::eErrorInvalidParameter: return "eErrorInvalidParameter";
    case sl::Result::eErrorMissingConstants: return "eErrorMissingConstants";
    case sl::Result::eErrorDuplicatedConstants: return "eErrorDuplicatedConstants";
    case sl::Result::eErrorMissingOrInvalidAPI: return "eErrorMissingOrInvalidAPI";
    case sl::Result::eErrorCommonConstantsMissing: return "eErrorCommonConstantsMissing";
    case sl::Result::eErrorUnsupportedInterface: return "eErrorUnsupportedInterface";
    case sl::Result::eErrorFeatureMissing: return "eErrorFeatureMissing";
    case sl::Result::eErrorFeatureNotSupported: return "eErrorFeatureNotSupported";
    case sl::Result::eErrorFeatureMissingHooks: return "eErrorFeatureMissingHooks";
    case sl::Result::eErrorFeatureFailedToLoad: return "eErrorFeatureFailedToLoad";
    case sl::Result::eErrorFeatureWrongPriority: return "eErrorFeatureWrongPriority";
    case sl::Result::eErrorFeatureMissingDependency: return "eErrorFeatureMissingDependency";
    case sl::Result::eErrorFeatureManagerInvalidState: return "eErrorFeatureManagerInvalidState";
    case sl::Result::eErrorInvalidState: return "eErrorInvalidState";
    case sl::Result::eWarnOutOfVRAM: return "eWarnOutOfVRAM";
    default: return "eUnknownError";
    }
}

void slApiErrorCallback(const sl::APIError& err) {
    wchar_t buf[160]{};
    std::swprintf(buf, 160, L"[EDPE][Streamline] Async APIError HRESULT=0x%08X",
                  static_cast<unsigned>(err.hres));
    EdpeLog(buf);
}

sl::float4x4 toSlMatrix(const float (&m)[16]) {
    sl::float4x4 out{};
    out.setRow(0, {m[0], m[1], m[2], m[3]});
    out.setRow(1, {m[4], m[5], m[6], m[7]});
    out.setRow(2, {m[8], m[9], m[10], m[11]});
    out.setRow(3, {m[12], m[13], m[14], m[15]});
    return out;
}

struct SlApiTable {
    HMODULE interposer = nullptr;
    PFun_slInit* slInit = nullptr;
    PFun_slShutdown* slShutdown = nullptr;
    PFun_slIsFeatureSupported* slIsFeatureSupported = nullptr;
    PFun_slIsFeatureLoaded* slIsFeatureLoaded = nullptr;
    PFun_slSetFeatureLoaded* slSetFeatureLoaded = nullptr;
    PFun_slGetFeatureRequirements* slGetFeatureRequirements = nullptr;
    PFun_slGetFeatureVersion* slGetFeatureVersion = nullptr;
    PFun_slUpgradeInterface* slUpgradeInterface = nullptr;
    PFun_slGetNativeInterface* slGetNativeInterface = nullptr;
    PFun_slGetFeatureFunction* slGetFeatureFunction = nullptr;
    PFun_slGetNewFrameToken* slGetNewFrameToken = nullptr;
    PFun_slSetD3DDevice* slSetD3DDevice = nullptr;
    PFun_slSetConstants* slSetConstants = nullptr;
    PFun_slSetTagForFrame* slSetTagForFrame = nullptr;

    PFun_slDLSSGGetState* slDLSSGGetState = nullptr;
    PFun_slDLSSGSetOptions* slDLSSGSetOptions = nullptr;

    PFun_slReflexGetState* slReflexGetState = nullptr;
    PFun_slReflexSetOptions* slReflexSetOptions = nullptr;
    PFun_slReflexSleep* slReflexSleep = nullptr;

    PFun_slPCLSetOptions* slPCLSetOptions = nullptr;
    PFun_slPCLSetMarker* slPCLSetMarker = nullptr;

    bool loadInterposer(StreamlineStatusSnapshot& snap) {
        if (interposer) return true;

        std::wstring pluginDir;
        const std::wstring dllPath = findInterposerPath(&pluginDir);
        if (dllPath.empty()) {
            std::wcsncpy(snap.unavailableReason,
                         L"sl.interposer.dll not found alongside binary.",
                         255);
            return false;
        }
        snap.interposerFound = true;

        if (!sl::security::verifyEmbeddedSignature(dllPath.c_str())) {
            if (isRunningUnderWine()) {
                std::wcsncpy(
                    snap.unavailableReason,
                    L"sl.interposer.dll embedded signature verification failed under Wine/Proton.",
                    255);
            } else {
                std::wcsncpy(
                    snap.unavailableReason,
                    L"sl.interposer.dll failed NVIDIA embedded digital signature verification.",
                    255);
            }
            return false;
        }
        snap.signatureVerified = true;

        HMODULE mod = LoadLibraryW(dllPath.c_str());
        if (!mod) {
            std::swprintf(snap.unavailableReason, 255,
                          L"LoadLibraryW(sl.interposer.dll) failed (err=%lu).", GetLastError());
            return false;
        }

        auto resolve = [&](auto& fn, const char* sym) -> bool {
            fn = reinterpret_cast<std::decay_t<decltype(fn)>>(GetProcAddress(mod, sym));
            return fn != nullptr;
        };

        const bool coreOk =
            resolve(slInit, "slInit") &&
            resolve(slShutdown, "slShutdown") &&
            resolve(slIsFeatureSupported, "slIsFeatureSupported") &&
            resolve(slIsFeatureLoaded, "slIsFeatureLoaded") &&
            resolve(slSetFeatureLoaded, "slSetFeatureLoaded") &&
            resolve(slGetFeatureRequirements, "slGetFeatureRequirements") &&
            resolve(slGetFeatureVersion, "slGetFeatureVersion") &&
            resolve(slUpgradeInterface, "slUpgradeInterface") &&
            resolve(slGetNativeInterface, "slGetNativeInterface") &&
            resolve(slGetFeatureFunction, "slGetFeatureFunction") &&
            resolve(slGetNewFrameToken, "slGetNewFrameToken") &&
            resolve(slSetD3DDevice, "slSetD3DDevice") &&
            resolve(slSetConstants, "slSetConstants") &&
            resolve(slSetTagForFrame, "slSetTagForFrame");

        if (!coreOk) {
            FreeLibrary(mod);
            std::wcsncpy(snap.unavailableReason,
                         L"sl.interposer.dll is missing required Streamline v2 exports.",
                         255);
            return false;
        }

        interposer = mod;
        return true;
    }

    bool resolveFeatureFunctions() {
        void* fn = nullptr;
        if (slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", fn) == sl::Result::eOk) {
            slDLSSGGetState = reinterpret_cast<PFun_slDLSSGGetState*>(fn);
        }
        if (slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fn) == sl::Result::eOk) {
            slDLSSGSetOptions = reinterpret_cast<PFun_slDLSSGSetOptions*>(fn);
        }
        if (slGetFeatureFunction(sl::kFeatureReflex, "slReflexGetState", fn) == sl::Result::eOk) {
            slReflexGetState = reinterpret_cast<PFun_slReflexGetState*>(fn);
        }
        if (slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", fn) == sl::Result::eOk) {
            slReflexSetOptions = reinterpret_cast<PFun_slReflexSetOptions*>(fn);
        }
        if (slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", fn) == sl::Result::eOk) {
            slReflexSleep = reinterpret_cast<PFun_slReflexSleep*>(fn);
        }
        if (slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetOptions", fn) == sl::Result::eOk) {
            slPCLSetOptions = reinterpret_cast<PFun_slPCLSetOptions*>(fn);
        }
        if (slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", fn) == sl::Result::eOk) {
            slPCLSetMarker = reinterpret_cast<PFun_slPCLSetMarker*>(fn);
        }
        return (slReflexSetOptions != nullptr);
    }

    void unload() {
        slDLSSGGetState = nullptr;
        slDLSSGSetOptions = nullptr;
        slReflexGetState = nullptr;
        slReflexSetOptions = nullptr;
        slReflexSleep = nullptr;
        slPCLSetOptions = nullptr;
        slPCLSetMarker = nullptr;

        slInit = nullptr;
        slShutdown = nullptr;
        slIsFeatureSupported = nullptr;
        slIsFeatureLoaded = nullptr;
        slSetFeatureLoaded = nullptr;
        slGetFeatureRequirements = nullptr;
        slGetFeatureVersion = nullptr;
        slUpgradeInterface = nullptr;
        slGetNativeInterface = nullptr;
        slGetFeatureFunction = nullptr;
        slGetNewFrameToken = nullptr;
        slSetD3DDevice = nullptr;
        slSetConstants = nullptr;
        slSetTagForFrame = nullptr;

        if (interposer) {
            FreeLibrary(interposer);
            interposer = nullptr;
        }
    }
};

#endif // EDPE_ENABLE_STREAMLINE

} // namespace

struct NvidiaStreamlineFgBackend::Impl {
    std::mutex mutex;
    D3D12ProxyPresenter* presenter = nullptr;
    bool initialized = false;
    bool lowLatencyBoost = false;
    bool dynamicMfg = false;
    float dynamicTargetFps = 0.0f;
    bool forceResetNext = true;
    bool activeInLastEvaluate = false;
    bool probeAttempted = false;
    uint32_t lastAppliedReflexMode = 0;

    StreamlineStatusSnapshot snapshot{};

#if defined(EDPE_ENABLE_STREAMLINE) && EDPE_ENABLE_STREAMLINE
    SlApiTable sl;
    sl::ViewportHandle viewport{0};
    sl::FrameToken* currentToken = nullptr;
    Microsoft::WRL::ComPtr<IDXGIFactory4> slProxyFactory;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> slProxyQueue;
    bool swapChainWrapped = false;

    static void onPrePresent(void* userCtx) {
        auto* self = static_cast<Impl*>(userCtx);
        if (!self || !self->initialized || !self->currentToken) return;
        if (self->sl.slPCLSetMarker && self->snapshot.pclSupported) {
            self->sl.slPCLSetMarker(sl::PCLMarker::ePresentStart, *self->currentToken);
        }
    }

    static void onPostPresent(void* userCtx) {
        auto* self = static_cast<Impl*>(userCtx);
        if (!self || !self->initialized) return;
        if (self->currentToken && self->sl.slPCLSetMarker && self->snapshot.pclSupported) {
            self->sl.slPCLSetMarker(sl::PCLMarker::ePresentEnd, *self->currentToken);
        }
        if (self->sl.slDLSSGGetState && self->snapshot.dlssgSupported) {
            sl::DLSSGState state{};
            if (self->sl.slDLSSGGetState(self->viewport, state, nullptr) == sl::Result::eOk) {
                self->snapshot.lastPresentedFrames = state.numFramesActuallyPresented;
                self->snapshot.dlssgStatusFlags = static_cast<uint32_t>(state.status);
                self->snapshot.maxFramesToGenerate =
                    (std::max)(1u, state.numFramesToGenerateMax);
                self->snapshot.dynamicMfgSupported =
                    (state.bIsDynamicMFGSupported == sl::Boolean::eTrue);
                self->snapshot.vsyncSupported =
                    (state.bIsVsyncSupportAvailable == sl::Boolean::eTrue);

                if (state.status != sl::DLSSGStatus::eOk && self->activeInLastEvaluate) {
                    wchar_t logBuf[256]{};
                    std::swprintf(logBuf, 255,
                                  L"[EDPE][Streamline] DLSS-G status error (%S); failing open to Off.",
                                  NvidiaStreamlineFgBackend::dlssgStatusToString(
                                      static_cast<uint32_t>(state.status)));
                    EdpeLog(logBuf);
                    if (self->sl.slDLSSGSetOptions) {
                        sl::DLSSGOptions offOpts{};
                        offOpts.mode = sl::DLSSGMode::eOff;
                        offOpts.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
                        self->sl.slDLSSGSetOptions(self->viewport, offOpts);
                    }
                    self->activeInLastEvaluate = false;
                }
            }
        }
    }

    static sl::Preferences buildBasePreferences(const wchar_t* const* pluginPaths,
                                              uint32_t numPaths,
                                              const sl::Feature* features,
                                              uint32_t numFeatures) {
        sl::Preferences pref{};
        pref.showConsole = false;
        // CRITICAL: pref.logLevel MUST remain eOff with logMessageCallback = nullptr.
        // Streamline's sl.common.dll installs its own ngxLog function pointer into _nvngx.dll's
        // process-global logging hook whenever logLevel != eOff and NGX initializes during
        // slSetD3DDevice(). Unloading sl.common.dll in slShutdown() leaves _nvngx.dll with a
        // dangling callback pointer and crashes (0xC0000005) on subsequent D3D11 NGX calls (#19).
        pref.logLevel = sl::LogLevel::eOff;
        pref.pathsToPlugins = const_cast<const wchar_t**>(pluginPaths);
        pref.numPathsToPlugins = numPaths;
        pref.pathToLogsAndData = nullptr;
        pref.logMessageCallback = nullptr;
        pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                     sl::PreferenceFlags::eDisableDebugText |
                     sl::PreferenceFlags::eUseManualHooking |
                     sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        pref.featuresToLoad = features;
        pref.numFeaturesToLoad = numFeatures;
        pref.engine = sl::EngineType::eCustom;
        pref.engineVersion = "1.0";
        pref.projectId = kEdpeProjectId;
        pref.renderAPI = sl::RenderAPI::eD3D12;
        return pref;
    }

    void queryAdapterFeatureCapabilities(LUID adapterLuid) {
        sl::AdapterInfo adapterInfo{};
        adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&adapterLuid);
        adapterInfo.deviceLUIDSizeInBytes = sizeof(adapterLuid);

        const sl::Result reflexRc = sl.slIsFeatureSupported(sl::kFeatureReflex, adapterInfo);
        const sl::Result pclRc = sl.slIsFeatureSupported(sl::kFeaturePCL, adapterInfo);
        const sl::Result dlssgRc = sl.slIsFeatureSupported(sl::kFeatureDLSS_G, adapterInfo);

        snapshot.lastReflexSupportCode = static_cast<int32_t>(reflexRc);
        snapshot.lastDlssgSupportCode = static_cast<int32_t>(dlssgRc);
        snapshot.reflexSupported = (reflexRc == sl::Result::eOk);
        snapshot.pclSupported = (pclRc == sl::Result::eOk);
        snapshot.dlssgSupported = (dlssgRc == sl::Result::eOk);

        sl::FeatureVersion fv{};
        if (sl.slGetFeatureVersion(sl::kFeatureDLSS_G, fv) == sl::Result::eOk && fv.versionSL) {
            std::snprintf(snapshot.dlssgVersion, sizeof(snapshot.dlssgVersion), "%u.%u.%u",
                          fv.versionSL.major, fv.versionSL.minor, fv.versionSL.build);
        }
        fv = {};
        if (sl.slGetFeatureVersion(sl::kFeatureReflex, fv) == sl::Result::eOk && fv.versionSL) {
            std::snprintf(snapshot.reflexVersion, sizeof(snapshot.reflexVersion), "%u.%u.%u",
                          fv.versionSL.major, fv.versionSL.minor, fv.versionSL.build);
        }
    }

    bool probeAdapterLuid(LUID adapterLuid) {
        if (D3D12ProxyPresenter::isCrossApiPresentBlockedByRenderDoc()) {
            std::wcsncpy(snapshot.unavailableReason,
                         L"Blocked: Windows renderdoc.dll in-process.",
                         255);
            return false;
        }

        std::wstring pluginDir;
        findInterposerPath(&pluginDir);
        if (!sl.loadInterposer(snapshot)) {
            return false;
        }

        const wchar_t* pluginPaths[] = {pluginDir.c_str()};
        const sl::Feature features[] = {
            sl::kFeatureDLSS_G,
            sl::kFeatureReflex,
            sl::kFeaturePCL,
        };
        const sl::Preferences pref = buildBasePreferences(
            pluginPaths, pluginDir.empty() ? 0u : 1u,
            features, static_cast<uint32_t>(std::size(features)));

        const sl::Result res = sl.slInit(pref, sl::kSDKVersion);
        snapshot.lastSlResult = static_cast<int32_t>(res);
        if (res != sl::Result::eOk) {
            std::swprintf(snapshot.unavailableReason, 255,
                          L"slInit failed: %S", slResultToString(res));
            sl.unload();
            return false;
        }
        snapshot.slInitialized = true;

        std::snprintf(snapshot.slVersion, sizeof(snapshot.slVersion), "%u.%u.%u",
                      SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);

        queryAdapterFeatureCapabilities(adapterLuid);

        const sl::Result dlssgRc = static_cast<sl::Result>(snapshot.lastDlssgSupportCode);
        const sl::Result reflexRc = static_cast<sl::Result>(snapshot.lastReflexSupportCode);
        if (!snapshot.dlssgSupported) {
            std::swprintf(snapshot.unavailableReason, 255,
                          L"DLSS-G unsupported on active GPU/OS (%S; Reflex=%S).",
                          slResultToString(dlssgRc), slResultToString(reflexRc));
        } else if (!snapshot.reflexSupported) {
            std::wcsncpy(snapshot.unavailableReason,
                         L"Mandatory sl.reflex unavailable for DLSS-G.",
                         255);
        }

        sl.slShutdown();
        snapshot.slInitialized = false;
        sl.unload();

        return snapshot.dlssgSupported && snapshot.reflexSupported;
    }

    bool initAndProbe(ID3D12Device* d3d12Device, bool keepActive) {
        if (!d3d12Device) {
            std::wcsncpy(snapshot.unavailableReason, L"Null ID3D12Device.", 255);
            return false;
        }
        if (D3D12ProxyPresenter::isCrossApiPresentBlockedByRenderDoc()) {
            std::wcsncpy(snapshot.unavailableReason,
                         L"Blocked: Windows renderdoc.dll in-process.",
                         255);
            return false;
        }

        std::wstring pluginDir;
        findInterposerPath(&pluginDir);
        if (!sl.loadInterposer(snapshot)) {
            return false;
        }

        const wchar_t* pluginPaths[] = {pluginDir.c_str()};
        const sl::Feature features[] = {
            sl::kFeatureDLSS_G,
            sl::kFeatureReflex,
            sl::kFeaturePCL,
        };
        const sl::Preferences pref = buildBasePreferences(
            pluginPaths, pluginDir.empty() ? 0u : 1u,
            features, static_cast<uint32_t>(std::size(features)));

        sl::Result res = sl.slInit(pref, sl::kSDKVersion);
        snapshot.lastSlResult = static_cast<int32_t>(res);
        if (res != sl::Result::eOk) {
            std::swprintf(snapshot.unavailableReason, 255,
                          L"slInit failed: %S", slResultToString(res));
            sl.unload();
            return false;
        }
        snapshot.slInitialized = true;

        std::snprintf(snapshot.slVersion, sizeof(snapshot.slVersion), "%u.%u.%u",
                      SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);

        res = sl.slSetD3DDevice(d3d12Device);
        snapshot.lastSlResult = static_cast<int32_t>(res);
        if (res != sl::Result::eOk) {
            std::swprintf(snapshot.unavailableReason, 255,
                          L"slSetD3DDevice failed: %S", slResultToString(res));
            sl.slShutdown();
            snapshot.slInitialized = false;
            sl.unload();
            return false;
        }

        queryAdapterFeatureCapabilities(d3d12Device->GetAdapterLuid());
        sl.resolveFeatureFunctions();

        if (!snapshot.dlssgSupported) {
            std::swprintf(snapshot.unavailableReason, 255,
                          L"DLSS-G unsupported on active GPU/OS (%S; Reflex=%S).",
                          slResultToString(static_cast<sl::Result>(snapshot.lastDlssgSupportCode)),
                          slResultToString(static_cast<sl::Result>(snapshot.lastReflexSupportCode)));
            if (!keepActive) {
                sl.slShutdown();
                snapshot.slInitialized = false;
                sl.unload();
            }
            return false;
        }

        if (!sl.slDLSSGGetState || !sl.slDLSSGSetOptions) {
            std::wcsncpy(snapshot.unavailableReason,
                         L"slDLSSGGetState/SetOptions unavailable after slInit.",
                         255);
            if (!keepActive) {
                sl.slShutdown();
                snapshot.slInitialized = false;
                sl.unload();
            }
            return false;
        }

        if (!snapshot.reflexSupported || !sl.slReflexSetOptions) {
            std::wcsncpy(snapshot.unavailableReason,
                         L"Mandatory sl.reflex unavailable for DLSS-G.",
                         255);
            if (!keepActive) {
                sl.slShutdown();
                snapshot.slInitialized = false;
                sl.unload();
            }
            return false;
        }

        return true;
    }

    bool applyReflexOptions(sl::ReflexMode mode) {
        if (!sl.slReflexSetOptions || !snapshot.reflexSupported) return false;
        sl::ReflexOptions ro{};
        ro.mode = mode;
        const sl::Result r = sl.slReflexSetOptions(ro);
        if (r == sl::Result::eOk) {
            lastAppliedReflexMode = static_cast<uint32_t>(mode);
            if (sl.slPCLSetOptions && snapshot.pclSupported) {
                sl::PCLOptions po{};
                po.virtualKey = sl::PCLHotKey::eUsePingMessage;
                sl.slPCLSetOptions(po);
            }
            return true;
        }
        return false;
    }
#endif
};

NvidiaStreamlineFgBackend::NvidiaStreamlineFgBackend()
    : impl_(std::make_unique<Impl>()) {
#if defined(EDPE_ENABLE_STREAMLINE) && EDPE_ENABLE_STREAMLINE
    std::wstring dir;
    impl_->snapshot.interposerFound = !findInterposerPath(&dir).empty();
    if (!impl_->snapshot.interposerFound) {
        std::wcsncpy(impl_->snapshot.unavailableReason,
                     L"sl.interposer.dll not staged alongside binary.",
                     255);
    }
#else
    std::wcsncpy(impl_->snapshot.unavailableReason,
                 L"Built without Streamline SDK headers (EDPE_ENABLE_STREAMLINE=0).",
                 255);
#endif
}

NvidiaStreamlineFgBackend::~NvidiaStreamlineFgBackend() {
    shutdown();
}

bool NvidiaStreamlineFgBackend::isAvailable() const {
#if defined(EDPE_ENABLE_STREAMLINE) && EDPE_ENABLE_STREAMLINE
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (D3D12ProxyPresenter::isCrossApiPresentBlockedByRenderDoc()) return false;
    if (impl_->probeAttempted) return impl_->snapshot.dlssgSupported;
    if (!impl_->snapshot.interposerFound) {
        std::wstring dir;
        if (findInterposerPath(&dir).empty()) return false;
    }
    return true;
#else
    return false;
#endif
}

const wchar_t* NvidiaStreamlineFgBackend::unavailableReason() const {
    return impl_->snapshot.unavailableReason;
}

uint32_t NvidiaStreamlineFgBackend::maxFramesToGenerate() const {
    return impl_->snapshot.maxFramesToGenerate;
}

bool NvidiaStreamlineFgBackend::isDynamicMfgSupported() const {
    return impl_->snapshot.dynamicMfgSupported;
}

bool NvidiaStreamlineFgBackend::initialize(D3D12ProxyPresenter* presenter) {
#if defined(EDPE_ENABLE_STREAMLINE) && EDPE_ENABLE_STREAMLINE
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->initialized) return true;
    if (!presenter || !presenter->isInitialized() || !presenter->bridge().d3d12Device() ||
        !presenter->dxgiFactory() || !presenter->bridge().commandQueue() ||
        !presenter->targetHwnd()) {
        std::wcsncpy(impl_->snapshot.unavailableReason,
                     L"Presenter not initialized for Streamline DLSS-G.",
                     255);
        return false;
    }

    impl_->probeAttempted = true;
    if (!impl_->initAndProbe(presenter->bridge().d3d12Device(), true)) {
        // Ensure clean teardown when probe fails or DLSS-G is unsupported.
        if (impl_->snapshot.slInitialized && impl_->sl.slShutdown) {
            impl_->sl.slShutdown();
            impl_->snapshot.slInitialized = false;
        }
        impl_->sl.unload();
        return false;
    }

    // ManualHooking requires factory + D3D12 queue upgraded BEFORE swapchain creation
    // so Streamline owns the internal swapchain and routes Present() through presentCommon().
    void* factoryPtr = presenter->dxgiFactory();
    sl::Result upFactoryRc = impl_->sl.slUpgradeInterface(&factoryPtr);
    if (upFactoryRc != sl::Result::eOk || !factoryPtr) {
        std::swprintf(impl_->snapshot.unavailableReason, 255,
                      L"slUpgradeInterface(IDXGIFactory) failed: %S",
                      slResultToString(upFactoryRc));
        impl_->sl.slShutdown();
        impl_->snapshot.slInitialized = false;
        impl_->sl.unload();
        return false;
    }
    impl_->slProxyFactory.Attach(static_cast<IDXGIFactory4*>(factoryPtr));

    void* queuePtr = presenter->bridge().commandQueue();
    sl::Result upQueueRc = impl_->sl.slUpgradeInterface(&queuePtr);
    if (upQueueRc != sl::Result::eOk || !queuePtr) {
        std::swprintf(impl_->snapshot.unavailableReason, 255,
                      L"slUpgradeInterface(ID3D12CommandQueue) failed: %S",
                      slResultToString(upQueueRc));
        impl_->slProxyFactory.Reset();
        impl_->sl.slShutdown();
        impl_->snapshot.slInitialized = false;
        impl_->sl.unload();
        return false;
    }
    impl_->slProxyQueue.Attach(static_cast<ID3D12CommandQueue*>(queuePtr));

    // Detach plain swapchain before creating Streamline's proxy swapchain on the same HWND.
    IDXGISwapChain4* rawSc = presenter->detachSwapChainForWrap();
    if (rawSc) rawSc->Release();

    // Do not request FRAME_LATENCY_WAITABLE_OBJECT per SL DLSS-G docs §12.1 (let SL pacer own it).
    UINT cleanFlags = presenter->swapChainFlags() & ~DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    const DXGI_SWAP_CHAIN_DESC1 scDesc = sanitizeFlipSwapChainDesc(
        presenter->width(), presenter->height(),
        presenter->format(), presenter->bufferCount(),
        cleanFlags, presenter->isTearingSupported());

    Microsoft::WRL::ComPtr<IDXGISwapChain1> proxySc1;
    const HRESULT hr = impl_->slProxyFactory->CreateSwapChainForHwnd(
        impl_->slProxyQueue.Get(),
        presenter->targetHwnd(),
        &scDesc,
        nullptr,
        nullptr,
        &proxySc1);

    Microsoft::WRL::ComPtr<IDXGISwapChain4> proxySc4;
    if (FAILED(hr) || FAILED(proxySc1.As(&proxySc4)) ||
        !presenter->attachWrappedSwapChain(proxySc4.Detach())) {
        std::swprintf(impl_->snapshot.unavailableReason, 255,
                      L"CreateSwapChainForHwnd(Streamline proxy) failed (0x%08X).",
                      static_cast<unsigned>(hr));
        presenter->recreatePlainSwapChain();
        impl_->slProxyQueue.Reset();
        impl_->slProxyFactory.Reset();
        impl_->sl.slShutdown();
        impl_->snapshot.slInitialized = false;
        impl_->sl.unload();
        return false;
    }

    impl_->slProxyFactory->MakeWindowAssociation(presenter->targetHwnd(), DXGI_MWA_NO_ALT_ENTER);

    // Enable mandatory SL Reflex before any DLSS-G evaluation.
    impl_->applyReflexOptions(impl_->lowLatencyBoost
                                  ? sl::ReflexMode::eLowLatencyWithBoost
                                  : sl::ReflexMode::eLowLatency);

    // Re-query DLSS-G state now that proxy swapchain exists.
    sl::DLSSGState st{};
    if (impl_->sl.slDLSSGGetState(impl_->viewport, st, nullptr) == sl::Result::eOk) {
        impl_->snapshot.maxFramesToGenerate = (std::max)(1u, st.numFramesToGenerateMax);
        impl_->snapshot.dynamicMfgSupported = (st.bIsDynamicMFGSupported == sl::Boolean::eTrue);
        impl_->snapshot.vsyncSupported = (st.bIsVsyncSupportAvailable == sl::Boolean::eTrue);
    } else {
        impl_->snapshot.maxFramesToGenerate = 1;
    }

    presenter->setPresentMarkerHooks(&Impl::onPrePresent, &Impl::onPostPresent, impl_.get());
    impl_->presenter = presenter;
    impl_->swapChainWrapped = true;
    impl_->initialized = true;
    impl_->forceResetNext = true;
    impl_->snapshot.unavailableReason[0] = L'\0';
    return true;
#else
    (void)presenter;
    return false;
#endif
}

void NvidiaStreamlineFgBackend::shutdown() {
#if defined(EDPE_ENABLE_STREAMLINE) && EDPE_ENABLE_STREAMLINE
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized) return;

    if (impl_->presenter) {
        impl_->presenter->setPresentMarkerHooks(nullptr, nullptr, nullptr);
        impl_->presenter->bridge().waitGpuIdle();
    }

    if (impl_->sl.slDLSSGSetOptions) {
        sl::DLSSGOptions offOpts{};
        offOpts.mode = sl::DLSSGMode::eOff;
        impl_->sl.slDLSSGSetOptions(impl_->viewport, offOpts);
    }
    if (impl_->sl.slReflexSetOptions && impl_->snapshot.reflexSupported) {
        sl::ReflexOptions offReflex{};
        offReflex.mode = sl::ReflexMode::eOff;
        impl_->sl.slReflexSetOptions(offReflex);
    }

    // Restore plain D3D12 swapchain BEFORE slShutdown() so SL swapchain proxy is released first.
    if (impl_->swapChainWrapped && impl_->presenter && impl_->presenter->isInitialized()) {
        impl_->presenter->recreatePlainSwapChain();
    }

    impl_->slProxyQueue.Reset();
    impl_->slProxyFactory.Reset();
    if (impl_->snapshot.slInitialized && impl_->sl.slShutdown) {
        impl_->sl.slShutdown();
        impl_->snapshot.slInitialized = false;
    }
    impl_->sl.unload();

    impl_->currentToken = nullptr;
    impl_->swapChainWrapped = false;
    impl_->presenter = nullptr;
    impl_->initialized = false;
    impl_->activeInLastEvaluate = false;
#endif
}

bool NvidiaStreamlineFgBackend::evaluate(ID3D12GraphicsCommandList* cmdList,
                                         const FrameGenerationInputs& inputs,
                                         bool enabled,
                                         uint32_t requestedExtraFrames,
                                         uint32_t* outGeneratedFrames) {
    if (outGeneratedFrames) *outGeneratedFrames = 0;
#if defined(EDPE_ENABLE_STREAMLINE) && EDPE_ENABLE_STREAMLINE
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized || !impl_->presenter || !cmdList) return false;

    const uint32_t frameIdx32 = static_cast<uint32_t>(inputs.frameIndex & 0xFFFFFFFFu);
    sl::FrameToken* token = nullptr;
    if (impl_->sl.slGetNewFrameToken(token, &frameIdx32) != sl::Result::eOk || !token) {
        return false;
    }
    impl_->currentToken = token;

    if (impl_->sl.slPCLSetMarker && impl_->snapshot.pclSupported) {
        impl_->sl.slPCLSetMarker(sl::PCLMarker::eSimulationStart, *token);
        impl_->sl.slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *token);
        impl_->sl.slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *token);
    }

    const sl::ReflexMode desiredReflex = impl_->lowLatencyBoost
                                             ? sl::ReflexMode::eLowLatencyWithBoost
                                             : sl::ReflexMode::eLowLatency;
    if (impl_->lastAppliedReflexMode != static_cast<uint32_t>(desiredReflex)) {
        impl_->applyReflexOptions(desiredReflex);
    }

    const bool effectiveEnable = enabled && !inputs.isPauseOrMenu;

    FgCameraMatrices camMats{};
    if (!computeFgCameraMatrices(inputs.cameraNow, inputs.cameraPrevious, &camMats)) {
        if (impl_->sl.slDLSSGSetOptions) {
            sl::DLSSGOptions offOpts{};
            offOpts.mode = sl::DLSSGMode::eOff;
            offOpts.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
            impl_->sl.slDLSSGSetOptions(impl_->viewport, offOpts);
        }
        impl_->activeInLastEvaluate = false;
        return false;
    }

    sl::Constants consts{};
    consts.cameraViewToClip = toSlMatrix(camMats.viewToClip);
    consts.clipToCameraView = toSlMatrix(camMats.clipToView);
    consts.clipToPrevClip = toSlMatrix(camMats.clipToPrevClip);
    consts.prevClipToClip = toSlMatrix(camMats.prevClipToClip);
    consts.jitterOffset = sl::float2(inputs.jitterX, inputs.jitterY);
    const float invW = (inputs.renderWidth > 0) ? (1.0f / static_cast<float>(inputs.renderWidth)) : 1.0f;
    const float invH = (inputs.renderHeight > 0) ? (1.0f / static_cast<float>(inputs.renderHeight)) : 1.0f;
    consts.mvecScale = sl::float2(inputs.motionScaleX * invW, inputs.motionScaleY * invH);
    consts.cameraPos = sl::float3(camMats.position.x, camMats.position.y, camMats.position.z);
    consts.cameraUp = sl::float3(camMats.up.x, camMats.up.y, camMats.up.z);
    consts.cameraRight = sl::float3(camMats.right.x, camMats.right.y, camMats.right.z);
    consts.cameraFwd = sl::float3(camMats.forward.x, camMats.forward.y, camMats.forward.z);
    consts.cameraNear = camMats.nearPlane;
    consts.cameraFar = 1.0e6f; // Infinite reversed-Z
    consts.cameraFOV = camMats.verticalFovRad;
    consts.cameraAspectRatio = camMats.aspectRatio;
    consts.depthInverted = sl::Boolean::eTrue;
    consts.cameraMotionIncluded = sl::Boolean::eTrue;
    consts.motionVectors3D = sl::Boolean::eFalse;
    consts.reset = (inputs.reset || impl_->forceResetNext) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    consts.orthographicProjection = sl::Boolean::eFalse;
    consts.motionVectorsDilated = sl::Boolean::eFalse;
    consts.motionVectorsJittered = sl::Boolean::eFalse;

    if (impl_->sl.slSetConstants(consts, *token, impl_->viewport) != sl::Result::eOk) {
        return false;
    }

    // Tag depth, motion vectors, HUD-less, and premultiplied UI color/alpha.
    // Per SL DLSS-G §5.1, UIAlpha and UIColorAndAlpha are mutually exclusive (we tag only UIColorAndAlpha).
    // Per SL DLSS-G §5.2, clear tags to null when paused/menus/disabled so stale buffers are ignored.
    const sl::Extent renderExtent{0u, 0u, inputs.renderWidth, inputs.renderHeight};
    const sl::Extent outExtent{0u, 0u, inputs.outputWidth, inputs.outputHeight};

    sl::Resource depthRes{sl::ResourceType::eTex2d,
                          effectiveEnable ? inputs.depth : nullptr,
                          D3D12_RESOURCE_STATE_COMMON};
    depthRes.width = inputs.renderWidth;
    depthRes.height = inputs.renderHeight;
    depthRes.nativeFormat = DXGI_FORMAT_R32_FLOAT;

    sl::Resource mvecRes{sl::ResourceType::eTex2d,
                         effectiveEnable ? inputs.motionVectors : nullptr,
                         D3D12_RESOURCE_STATE_COMMON};
    mvecRes.width = inputs.renderWidth;
    mvecRes.height = inputs.renderHeight;
    mvecRes.nativeFormat = DXGI_FORMAT_R16G16_FLOAT;

    sl::Resource hudlessRes{sl::ResourceType::eTex2d,
                            effectiveEnable ? inputs.hudlessColor : nullptr,
                            D3D12_RESOURCE_STATE_COMMON};
    hudlessRes.width = inputs.outputWidth;
    hudlessRes.height = inputs.outputHeight;
    hudlessRes.nativeFormat = impl_->presenter->format();

    sl::Resource uiRes{sl::ResourceType::eTex2d,
                       effectiveEnable ? inputs.packedUiColorAndAlpha : nullptr,
                       D3D12_RESOURCE_STATE_COMMON};
    uiRes.width = inputs.outputWidth;
    uiRes.height = inputs.outputHeight;
    uiRes.nativeFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    const sl::ResourceTag tags[] = {
        sl::ResourceTag{&depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent},
        sl::ResourceTag{&mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent},
        sl::ResourceTag{&hudlessRes, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &outExtent},
        sl::ResourceTag{&uiRes, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &outExtent},
    };
    if (impl_->sl.slSetTagForFrame(*token, impl_->viewport, tags,
                                   static_cast<uint32_t>(std::size(tags)), cmdList) != sl::Result::eOk) {
        return false;
    }

    sl::DLSSGOptions opts{};
    if (!effectiveEnable) {
        opts.mode = sl::DLSSGMode::eOff;
    } else if (impl_->dynamicMfg && impl_->snapshot.dynamicMfgSupported) {
        opts.mode = sl::DLSSGMode::eDynamic;
        opts.dynamicTargetFrameRate = impl_->dynamicTargetFps;
    } else {
        opts.mode = sl::DLSSGMode::eOn;
        const uint32_t maxGen = (std::max)(1u, impl_->snapshot.maxFramesToGenerate);
        opts.numFramesToGenerate = (std::min)((std::max)(1u, requestedExtraFrames), maxGen);
    }

    opts.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    if (effectiveEnable && inputs.packedUiColorAndAlpha) {
        opts.flags |= sl::DLSSGFlags::eEnableFullscreenMenuDetection;
    }
    opts.enableUserInterfaceRecomposition =
        (effectiveEnable && inputs.hudlessColor && inputs.packedUiColorAndAlpha)
            ? sl::Boolean::eTrue
            : sl::Boolean::eFalse;
    opts.onErrorCallback = &slApiErrorCallback;

    const sl::Result optRc = impl_->sl.slDLSSGSetOptions(impl_->viewport, opts);
    impl_->snapshot.lastSlResult = static_cast<int32_t>(optRc);

    if (impl_->sl.slPCLSetMarker && impl_->snapshot.pclSupported) {
        impl_->sl.slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *token);
    }

    if (optRc != sl::Result::eOk) {
        impl_->activeInLastEvaluate = false;
        return false;
    }

    impl_->forceResetNext = false;
    impl_->activeInLastEvaluate = effectiveEnable;
    if (outGeneratedFrames) {
        if (!effectiveEnable) {
            *outGeneratedFrames = 0;
        } else if (impl_->snapshot.lastPresentedFrames > 1) {
            *outGeneratedFrames = impl_->snapshot.lastPresentedFrames - 1;
        } else if (opts.mode == sl::DLSSGMode::eOn) {
            *outGeneratedFrames = opts.numFramesToGenerate;
        } else {
            *outGeneratedFrames = 1;
        }
    }
    return true;
#else
    (void)cmdList;
    (void)inputs;
    (void)enabled;
    (void)requestedExtraFrames;
    return false;
#endif
}

void NvidiaStreamlineFgBackend::resetHistory() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->forceResetNext = true;
}

void NvidiaStreamlineFgBackend::setLowLatencyBoost(bool boost) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->lowLatencyBoost = boost;
}

bool NvidiaStreamlineFgBackend::isLowLatencyBoost() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->lowLatencyBoost;
}

void NvidiaStreamlineFgBackend::setDynamicMfgMode(bool enableDynamic, float targetFps) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->dynamicMfg = enableDynamic;
    impl_->dynamicTargetFps = targetFps;
}

bool NvidiaStreamlineFgBackend::isDynamicMfgEnabled() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->dynamicMfg;
}

bool NvidiaStreamlineFgBackend::reflexSleepCurrentFrame() {
#if defined(EDPE_ENABLE_STREAMLINE) && EDPE_ENABLE_STREAMLINE
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized || !impl_->snapshot.reflexSupported || !impl_->sl.slReflexSleep) {
        return false;
    }
    if (!impl_->currentToken) {
        sl::FrameToken* token = nullptr;
        if (impl_->sl.slGetNewFrameToken(token, nullptr) == sl::Result::eOk) {
            impl_->currentToken = token;
        }
    }
    if (!impl_->currentToken) return false;
    return impl_->sl.slReflexSleep(*impl_->currentToken) == sl::Result::eOk;
#else
    return false;
#endif
}

StreamlineStatusSnapshot NvidiaStreamlineFgBackend::statusSnapshot() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->snapshot;
}

bool NvidiaStreamlineFgBackend::probeCapabilities(ID3D12Device* d3d12Device,
                                                  StreamlineStatusSnapshot* outSnapshot) {
    if (!outSnapshot) return false;
    *outSnapshot = {};
#if defined(EDPE_ENABLE_STREAMLINE) && EDPE_ENABLE_STREAMLINE
    if (!d3d12Device) {
        std::wcsncpy(outSnapshot->unavailableReason, L"Null ID3D12Device.", 255);
        return false;
    }
    return probeCapabilitiesForLuid(d3d12Device->GetAdapterLuid(), outSnapshot);
#else
    (void)d3d12Device;
    std::wcsncpy(outSnapshot->unavailableReason,
                 L"Built without Streamline SDK headers (EDPE_ENABLE_STREAMLINE=0).",
                 255);
    return false;
#endif
}

bool NvidiaStreamlineFgBackend::probeCapabilitiesForLuid(LUID adapterLuid,
                                                         StreamlineStatusSnapshot* outSnapshot) {
    if (!outSnapshot) return false;
    *outSnapshot = {};
#if defined(EDPE_ENABLE_STREAMLINE) && EDPE_ENABLE_STREAMLINE
    Impl probeImpl;
    const bool dlssgOk = probeImpl.probeAdapterLuid(adapterLuid);
    *outSnapshot = probeImpl.snapshot;
    return dlssgOk;
#else
    (void)adapterLuid;
    std::wcsncpy(outSnapshot->unavailableReason,
                 L"Built without Streamline SDK headers (EDPE_ENABLE_STREAMLINE=0).",
                 255);
    return false;
#endif
}

const char* NvidiaStreamlineFgBackend::dlssgStatusToString(uint32_t statusMask) {
    if (statusMask == 0) return "eOk";
    if (statusMask & (1u << 1)) return "eFailReflexNotDetectedAtRuntime";
    if (statusMask & (1u << 0)) return "eFailResolutionTooLow";
    if (statusMask & (1u << 2)) return "eFailHDRFormatNotSupported";
    if (statusMask & (1u << 3)) return "eFailCommonConstantsInvalid";
    if (statusMask & (1u << 4)) return "eFailGetCurrentBackBufferIndexNotCalled";
    return "eFailUnknown";
}

} // namespace edpe
