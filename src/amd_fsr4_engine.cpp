#include "amd_fsr4_engine.h"
#include "fsr4_logic.h"

#include "d3d12_bridge.h"
#include "log.h"

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(EDPE_ENABLE_FFX_FSR4) && EDPE_ENABLE_FFX_FSR4
#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>
#endif

namespace edpe {

namespace {

std::wstring iniPathNextToExecutable() {
    wchar_t modulePath[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (!len || len >= MAX_PATH) return L"edpe.ini";
    return (std::filesystem::path(modulePath).parent_path() / L"edpe.ini").wstring();
}

// Directory of the module that contains this code (dxgi.dll in the game, the test exe in tests).
std::filesystem::path moduleDirectory() {
    wchar_t path[MAX_PATH]{};
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&moduleDirectory), &self) &&
        self && GetModuleFileNameW(self, path, MAX_PATH) > 0) {
        return std::filesystem::path(path).parent_path();
    }
    return std::filesystem::path();
}

HMODULE loadSiblingDll(const wchar_t* dllName) {
    if (HMODULE already = GetModuleHandleW(dllName)) return already;
    const auto dir = moduleDirectory();
    if (!dir.empty()) {
        const auto candidate = dir / dllName;
        if (HMODULE loaded = LoadLibraryExW(candidate.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
            return loaded;
        }
    }
    return LoadLibraryW(dllName);
}

// Concrete (non-typeless, non-sRGB) format for shared textures. sRGB maps to UNORM: CopyResource between
// the two is a bit copy within one typeless family, and FSR is told about the encoding through its flags.
DXGI_FORMAT concreteColorFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return f;
    }
}

DXGI_FORMAT concreteMotionFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    default: return f;
    }
}

// Shader resource view format that exposes the depth plane as a float.
DXGI_FORMAT depthSrvFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// Converts the scene depth (typeless depth/stencil copy) into an R32_FLOAT texture FSR can read from D3D12.
const char kDepthConvertCs[] = R"(
Texture2D<float> gDepth : register(t0);
RWTexture2D<float> gOut : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    gOut[id.xy] = gDepth.Load(int3(id.xy, 0));
}
)";

} // namespace

#if defined(EDPE_ENABLE_FFX_FSR4) && EDPE_ENABLE_FFX_FSR4

namespace {

std::wstring widenUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int size = static_cast<int>(text.size());
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), size, nullptr, 0);
    if (count <= 0) return {};
    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), size, wide.data(), count);
    return wide;
}

void ffxMessageToLog(uint32_t type, const wchar_t* message) {
    wchar_t buf[512]{};
    std::swprintf(buf, 512, L"EDPE FSR4: [ffx %ls] %ls",
                  type == FFX_API_MESSAGE_TYPE_ERROR ? L"error" : L"warning", message ? message : L"");
    EdpeLog(buf);
}

struct Fsr4Slot {
    SharedBridgeTexture color;
    SharedBridgeTexture depth;
    SharedBridgeTexture motion;
    SharedBridgeTexture output;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> depthUav;
};

// The shared textures rest in COMMON between uses. For the dispatch FSR reads color, depth and motion and writes
// output; `forDispatch` false returns all four to COMMON.
void transitionSlot(ID3D12GraphicsCommandList* cl, Fsr4Slot& slot, bool forDispatch) {
    constexpr D3D12_RESOURCE_STATES kCommon = D3D12_RESOURCE_STATE_COMMON;
    constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr D3D12_RESOURCE_STATES kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    for (ID3D12Resource* input : {slot.color.d3d12.Get(), slot.depth.d3d12.Get(), slot.motion.d3d12.Get()}) {
        D3D11D3D12Bridge::transitionResource(cl, input, forDispatch ? kCommon : kRead, forDispatch ? kRead : kCommon);
    }
    D3D11D3D12Bridge::transitionResource(cl, slot.output.d3d12.Get(), forDispatch ? kCommon : kWrite,
                                         forDispatch ? kWrite : kCommon);
}

} // namespace

struct Fsr4Engine::Impl {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    D3D11D3D12Bridge bridge;
    HMODULE loaderDll = nullptr;
    HMODULE providerDll = nullptr;
    ffxFunctions ffx{};
    ffxContext upscaleCtx = nullptr;
    bool ready = false;
    bool resetPending = true;
    wchar_t error[256] = L"";
    char providerVer[64] = "unavailable";
    uint64_t providerVersionId = 0;  // the 4.x provider chosen from those AMD's runtime offers (ffxOverrideVersion)

    // Active configuration; any change recreates the resources and the FFX context.
    uint32_t renderW = 0, renderH = 0, outputW = 0, outputH = 0;
    DXGI_FORMAT colorFmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT motionFmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT outputFmt = DXGI_FORMAT_UNKNOWN;
    bool hdr = false;
    bool depthInverted = true;
    bool depthInfinite = true;
    std::array<Fsr4Slot, D3D11D3D12Bridge::kAllocatorCount> slots;

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> depthCs;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ownDepthSrv;
    ID3D11Texture2D* ownDepthSrvSource = nullptr;
    unsigned evalCount = 0;

    void setError(const wchar_t* msg) {
        // Log only when the message changes so a persistent failure does not flood edpe.log.
        const bool changed = std::wcscmp(error, msg) != 0;
        std::swprintf(error, 256, L"%ls", msg);
        if (!changed) return;
        wchar_t buf[320]{};
        std::swprintf(buf, 320, L"EDPE FSR4: %ls", msg);
        EdpeLog(buf);
    }

    void setErrorF(const wchar_t* what, long code) {
        wchar_t msg[256]{};
        std::swprintf(msg, 256, L"%ls (0x%08lX)", what, code);
        setError(msg);
    }

    bool loadDlls() {
        if (ffx.CreateContext && ffx.Dispatch && ffx.Query && ffx.Configure && ffx.DestroyContext) return true;
        if (!providerDll) providerDll = loadSiblingDll(L"amd_fidelityfx_upscaler_dx12.dll");
        if (!loaderDll) loaderDll = loadSiblingDll(L"amd_fidelityfx_loader_dx12.dll");
        if (!loaderDll) { setError(L"amd_fidelityfx_loader_dx12.dll not found beside dxgi.dll"); return false; }
        if (!providerDll) { setError(L"amd_fidelityfx_upscaler_dx12.dll not found beside dxgi.dll"); return false; }
        ffxLoadFunctions(&ffx, loaderDll);
        if (!ffx.CreateContext || !ffx.DestroyContext || !ffx.Configure || !ffx.Query || !ffx.Dispatch) {
            setError(L"FidelityFX loader is missing ffx_api entry points");
            return false;
        }
        ffxConfigureDescGlobalDebug dbg{};
        dbg.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
        dbg.effectId = FFX_API_EFFECT_ID_UPSCALE;
        dbg.fpMessage = ffxMessageToLog;
        dbg.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
        ffx.Configure(nullptr, &dbg.header);
        return true;
    }

    // Asks AMD's runtime which upscaler providers it offers for this D3D12 device and picks the highest 4.x one.
    // When there is none (RDNA 2, other vendors' GPUs, a runtime too old for the provider) FSR 4 is unavailable and
    // the error says what was offered. It never runs a 3.x provider under the FSR 4 name.
    bool selectProvider() {
        providerVersionId = 0;
        uint64_t count = 0;
        ffxQueryDescGetVersions query{};
        query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        query.device = bridge.d3d12Device();
        query.outputCount = &count;
        if (ffx.Query(nullptr, &query.header) != FFX_API_RETURN_OK) {
            setError(L"querying the FSR versions AMD's runtime offers failed");
            return false;
        }
        std::vector<uint64_t> ids(static_cast<size_t>(count));
        std::vector<const char*> names(static_cast<size_t>(count));
        if (count > 0) {
            query.outputCount = &count;
            query.versionIds = ids.data();
            query.versionNames = names.data();
            if (ffx.Query(nullptr, &query.header) != FFX_API_RETURN_OK) {
                setError(L"reading the FSR versions AMD's runtime offers failed");
                return false;
            }
        }
        std::vector<std::string> offered;
        for (size_t i = 0; i < static_cast<size_t>(count) && i < names.size(); ++i) {
            offered.emplace_back(names[i] ? names[i] : "");
        }
        fsr4::Version picked;
        const int index = fsr4::pickFsr4Provider(offered, &picked);
        const std::string list = fsr4::formatProviderList(offered);
        if (index < 0) {
            const std::string text = fsr4::providerRefusalText(offered, bridge.usingAgilitySdk());
            setError(widenUtf8(text).c_str());
            return false;
        }
        providerVersionId = ids[static_cast<size_t>(index)];
        wchar_t buf[256]{};
        std::swprintf(buf, 256, L"EDPE FSR4: AMD's runtime offers FSR %hs; using %u.%u.%u", list.c_str(), picked.major,
                      picked.minor, picked.patch);
        EdpeLog(buf);
        return true;
    }

    bool ensureDepthShader() {
        if (depthCs) return true;
        Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
        HRESULT hr = D3DCompile(kDepthConvertCs, sizeof(kDepthConvertCs) - 1, "edpe_fsr4_depth", nullptr, nullptr,
                                "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
        if (FAILED(hr)) { setErrorF(L"depth convert shader compile failed", hr); return false; }
        hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &depthCs);
        if (FAILED(hr)) { setErrorF(L"CreateComputeShader(depth convert) failed", hr); return false; }
        return true;
    }

    void destroyContext() {
        if (upscaleCtx) {
            bridge.waitGpuIdle();
            ffx.DestroyContext(&upscaleCtx, nullptr);
            upscaleCtx = nullptr;
        }
    }

    void destroyResources() {
        destroyContext();
        for (auto& s : slots) {
            s.depthUav.Reset();
            s.color.reset();
            s.depth.reset();
            s.motion.reset();
            s.output.reset();
        }
        renderW = renderH = outputW = outputH = 0;
        colorFmt = motionFmt = outputFmt = DXGI_FORMAT_UNKNOWN;
    }

    // Recreates the shared textures and the FFX context when sizes, formats or depth flags change.
    bool ensureResources(const TemporalFrameInputs& in, const D3D11_TEXTURE2D_DESC& colorDesc,
                         const D3D11_TEXTURE2D_DESC& motionDesc, const D3D11_TEXTURE2D_DESC& destDesc) {
        const DXGI_FORMAT cf = concreteColorFormat(colorDesc.Format);
        const DXGI_FORMAT mf = concreteMotionFormat(motionDesc.Format);
        const DXGI_FORMAT of = concreteColorFormat(destDesc.Format);
        if (renderW == in.renderWidth && renderH == in.renderHeight && outputW == in.outputWidth &&
            outputH == in.outputHeight && colorFmt == cf && motionFmt == mf && outputFmt == of && hdr == in.colorIsHdr &&
            depthInverted == in.depthInverted && depthInfinite == in.depthInfinite && slots[0].color.isValid()) {
            return true;
        }

        destroyResources();
        const auto t0 = std::chrono::steady_clock::now();
        for (auto& s : slots) {
            if (!bridge.createSharedTexture(in.renderWidth, in.renderHeight, cf, D3D11_BIND_SHADER_RESOURCE, s.color) ||
                !bridge.createSharedTexture(in.renderWidth, in.renderHeight, DXGI_FORMAT_R32_FLOAT,
                                            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, s.depth) ||
                !bridge.createSharedTexture(in.renderWidth, in.renderHeight, mf, D3D11_BIND_SHADER_RESOURCE, s.motion) ||
                !bridge.createSharedTexture(in.outputWidth, in.outputHeight, of,
                                            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, s.output)) {
                setError(L"failed to create shared textures (format or UAV not supported for D3D12 sharing)");
                destroyResources();
                return false;
            }
            const HRESULT hr = device->CreateUnorderedAccessView(s.depth.d3d11.Get(), nullptr, &s.depthUav);
            if (FAILED(hr)) { setErrorF(L"CreateUnorderedAccessView(depth) failed", hr); destroyResources(); return false; }
        }

        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = bridge.d3d12Device();

        // Select the 4.x provider chosen at initialization instead of the runtime's default.
        ffxOverrideVersion providerOverride{};
        providerOverride.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        providerOverride.versionId = providerVersionId;
        backend.header.pNext = providerVersionId ? &providerOverride.header : nullptr;

        ffxCreateContextDescUpscaleVersion ver{};
        ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        ver.header.pNext = &backend.header;
        ver.version = FFX_UPSCALER_VERSION;

        ffxCreateContextDescUpscale desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.header.pNext = &ver.header;
        // fsr4_logic.h (unit tested) decides which flags the inputs need; this maps them to FidelityFX bits.
        const fsr4::ContextFlagsPlan flagsPlan = fsr4::buildContextFlagsPlan(in);
        desc.flags = 0;
        if (flagsPlan.autoExposure) desc.flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        if (flagsPlan.depthInverted) desc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
        if (flagsPlan.depthInfinite) desc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
        if (flagsPlan.hdr) desc.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
        if (flagsPlan.nonLinearColorspace) desc.flags |= FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
        desc.maxRenderSize = {in.renderWidth, in.renderHeight};
        desc.maxUpscaleSize = {in.outputWidth, in.outputHeight};
        desc.fpMessage = ffxMessageToLog;

        const ffxReturnCode_t rc = ffx.CreateContext(&upscaleCtx, &desc.header, nullptr);
        if (rc != FFX_API_RETURN_OK || !upscaleCtx) {
            setErrorF(L"ffxCreateContext(upscale) failed", static_cast<long>(rc));
            upscaleCtx = nullptr;
            destroyResources();
            return false;
        }

        ffxQueryGetProviderVersion pv{};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (ffx.Query(&upscaleCtx, &pv.header) == FFX_API_RETURN_OK && pv.versionName) {
            std::snprintf(providerVer, sizeof(providerVer), "%s", pv.versionName);
        }
        // Belt and braces after the override: a context that reports anything but a 4.x provider is not FSR 4.
        fsr4::Version reported;
        if (!fsr4::parseVersion(providerVer, &reported) || reported.major < 4) {
            wchar_t msg[200]{};
            std::swprintf(msg, 200, L"AMD's runtime created a non-FSR 4 provider (%hs); FSR 4 is unavailable", providerVer);
            destroyContext();
            setError(msg);
            destroyResources();
            return false;
        }

        renderW = in.renderWidth; renderH = in.renderHeight;
        outputW = in.outputWidth; outputH = in.outputHeight;
        colorFmt = cf; motionFmt = mf; outputFmt = of;
        hdr = in.colorIsHdr; depthInverted = in.depthInverted; depthInfinite = in.depthInfinite;
        resetPending = true;

        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        wchar_t buf[320]{};
        std::swprintf(buf, 320,
                      L"EDPE FSR4: context ready provider=%S render=%ux%u output=%ux%u colorFmt=%u hdr=%d agility=%d (%.0f ms)",
                      providerVer, renderW, renderH, outputW, outputH, static_cast<unsigned>(cf), hdr ? 1 : 0,
                      bridge.usingAgilitySdk() ? 1 : 0, ms);
        EdpeLog(buf);
        return true;
    }

    // The caller's depth SRV, or one created over the depth texture when none was provided.
    ID3D11ShaderResourceView* depthSrvFor(const TemporalFrameInputs& in) {
        if (in.depthSrv) return in.depthSrv;
        if (!in.depth) return nullptr;
        if (ownDepthSrv && ownDepthSrvSource == in.depth) return ownDepthSrv.Get();
        D3D11_TEXTURE2D_DESC d{};
        in.depth->GetDesc(&d);
        const DXGI_FORMAT sf = depthSrvFormat(d.Format);
        if (sf == DXGI_FORMAT_UNKNOWN) return nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = sf;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        ownDepthSrv.Reset();
        if (FAILED(device->CreateShaderResourceView(in.depth, &sd, &ownDepthSrv))) return nullptr;
        ownDepthSrvSource = in.depth;
        return ownDepthSrv.Get();
    }

    // Checks the inputs, makes the persistent resources match them and fetches the D3D11.4 context. On failure it
    // sets the error and returns false. `depthSrv` is the shader view of the game's depth.
    bool prepareFrame(ID3D11DeviceContext* context, const TemporalFrameInputs& in, ID3D11Texture2D* dest,
                      ID3D11ShaderResourceView*& depthSrv, Microsoft::WRL::ComPtr<ID3D11DeviceContext4>& ctx4) {
        const wchar_t* reason = nullptr;
        if (!in.isValid(&reason)) { setError(reason ? reason : L"invalid temporal inputs"); return false; }
        if (!in.color || !in.motionVectors) { setError(L"color or motion vector texture missing"); return false; }
        if (in.colorIsHdr) { setError(L"HDR color input is not supported by FSR 4 yet"); return false; }

        Microsoft::WRL::ComPtr<ID3D11Device> evalDevice;
        context->GetDevice(&evalDevice);
        if (evalDevice.Get() != device.Get()) { setError(L"evaluation device differs from bridge device"); return false; }

        D3D11_TEXTURE2D_DESC colorDesc{}, motionDesc{}, destDesc{};
        in.color->GetDesc(&colorDesc);
        in.motionVectors->GetDesc(&motionDesc);
        dest->GetDesc(&destDesc);
        if (colorDesc.Width != in.renderWidth || colorDesc.Height != in.renderHeight ||
            motionDesc.Width != in.renderWidth || motionDesc.Height != in.renderHeight ||
            destDesc.Width != in.outputWidth || destDesc.Height != in.outputHeight) {
            setError(L"input or destination texture size does not match the reported render/output size");
            return false;
        }
        if (colorDesc.SampleDesc.Count != 1 || destDesc.SampleDesc.Count != 1) { setError(L"multisampled texture"); return false; }

        depthSrv = depthSrvFor(in);
        if (!depthSrv) { setError(L"depth shader resource view unavailable"); return false; }
        if (!ensureDepthShader()) return false;
        if (!ensureResources(in, colorDesc, motionDesc, destDesc)) return false;

        if (FAILED(context->QueryInterface(IID_PPV_ARGS(&ctx4)))) { setError(L"ID3D11DeviceContext4 unavailable"); return false; }
        return true;
    }

    // Records the FSR 4 dispatch for this slot. Closes the command list and returns false when AMD's runtime rejects it.
    bool recordDispatch(ID3D12GraphicsCommandList* cl, Fsr4Slot& slot, const TemporalFrameInputs& in) {
        transitionSlot(cl, slot, true);

        ffxDispatchDescUpscale du{};
        du.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        du.commandList = cl;
        du.color = ffxApiGetResourceDX12(slot.color.d3d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        du.depth = ffxApiGetResourceDX12(slot.depth.d3d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        du.motionVectors = ffxApiGetResourceDX12(slot.motion.d3d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        du.output = ffxApiGetResourceDX12(slot.output.d3d12.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        // Dispatch values come from fsr4_logic.h (unit tested).
        const fsr4::DispatchPlan plan = fsr4::buildDispatchPlan(in, resetPending);
        du.jitterOffset = {plan.jitterX, plan.jitterY};
        du.motionVectorScale = {plan.motionScaleX, plan.motionScaleY};
        du.renderSize = {in.renderWidth, in.renderHeight};
        du.upscaleSize = {in.outputWidth, in.outputHeight};
        du.enableSharpening = plan.enableSharpening;
        du.sharpness = plan.sharpness;
        du.frameTimeDelta = plan.frameTimeMs;
        du.preExposure = plan.preExposure;
        du.reset = plan.reset;
        du.cameraNear = plan.cameraNear;
        du.cameraFar = plan.cameraFar;
        du.cameraFovAngleVertical = plan.fovVertical;
        du.viewSpaceToMetersFactor = plan.viewSpaceToMeters;
        du.flags = plan.nonLinearSrgb ? static_cast<uint32_t>(FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB) : 0u;

        const ffxReturnCode_t rc = ffx.Dispatch(&upscaleCtx, &du.header);
        if (rc != FFX_API_RETURN_OK) {
            cl->Close();
            setErrorF(L"ffxDispatch(upscale) failed", static_cast<long>(rc));
            return false;
        }

        transitionSlot(cl, slot, false);
        return true;
    }

    // D3D11 side: copies color and motion into the slot's shared textures and converts depth to R32_FLOAT with a small
    // compute pass. All of it executes before the fence signal in submitAfterD3D11Signal.
    void fillSharedInputs(ID3D11DeviceContext* context, const TemporalFrameInputs& in, Fsr4Slot& slot,
                          ID3D11ShaderResourceView* depthSrv) {
        context->CopyResource(slot.color.d3d11.Get(), in.color);
        context->CopyResource(slot.motion.d3d11.Get(), in.motionVectors);
        {
            // Preserve the compute state the game or EDPE may have bound.
            Microsoft::WRL::ComPtr<ID3D11ComputeShader> prevCs;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> prevSrv;
            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> prevUav;
            context->CSGetShader(&prevCs, nullptr, nullptr);
            context->CSGetShaderResources(0, 1, &prevSrv);
            context->CSGetUnorderedAccessViews(0, 1, &prevUav);

            context->CSSetShader(depthCs.Get(), nullptr, 0);
            ID3D11ShaderResourceView* srv = depthSrv;
            context->CSSetShaderResources(0, 1, &srv);
            ID3D11UnorderedAccessView* uav = slot.depthUav.Get();
            const UINT initial = 0;
            context->CSSetUnorderedAccessViews(0, 1, &uav, &initial);
            context->Dispatch((in.renderWidth + 7) / 8, (in.renderHeight + 7) / 8, 1);

            ID3D11UnorderedAccessView* nullUav = nullptr;
            context->CSSetUnorderedAccessViews(0, 1, &nullUav, &initial);
            ID3D11ShaderResourceView* nullSrv = nullptr;
            context->CSSetShaderResources(0, 1, &nullSrv);
            const UINT restoreInitial = static_cast<UINT>(-1);
            ID3D11UnorderedAccessView* restoreUav = prevUav.Get();
            context->CSSetShader(prevCs.Get(), nullptr, 0);
            ID3D11ShaderResourceView* restoreSrv = prevSrv.Get();
            context->CSSetShaderResources(0, 1, &restoreSrv);
            context->CSSetUnorderedAccessViews(0, 1, &restoreUav, &restoreInitial);
        }
    }

    // One frame: validates the inputs, copies them into the slot's shared textures, dispatches FSR 4 on the D3D12
    // queue and copies the result into `dest`. Returns false, with an error message, without touching `dest`.
    bool evaluate(ID3D11DeviceContext* context, const TemporalFrameInputs& in, ID3D11Texture2D* dest) {
        if (!ready) return false;
        ID3D11ShaderResourceView* depthSrv = nullptr;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext4> ctx4;
        if (!prepareFrame(context, in, dest, depthSrv, ctx4)) return false;

        uint32_t slotIndex = 0;
        ID3D12GraphicsCommandList* cl = bridge.beginCommandList(slotIndex);
        if (!cl) { setError(L"bridge command list unavailable"); return false; }
        Fsr4Slot& slot = slots[slotIndex];

        if (!recordDispatch(cl, slot, in)) return false;
        fillSharedInputs(context, in, slot, depthSrv);

        const uint64_t done = bridge.submitAfterD3D11Signal(ctx4.Get(), slotIndex);
        if (done == 0) { setError(L"bridge submit failed"); return false; }
        if (!bridge.waitOnD3D11(ctx4.Get(), done)) { setError(L"D3D11 wait on D3D12 fence failed"); return false; }

        context->CopyResource(dest, slot.output.d3d11.Get());
        resetPending = false;
        ++evalCount;
        if (evalCount == 1) {
            EdpeLog(L"EDPE FSR4: first frame upscaled through the D3D12 bridge");
        }
        return true;
    }
};

Fsr4Engine::Fsr4Engine() : impl_(std::make_unique<Impl>()) {}
Fsr4Engine::~Fsr4Engine() { shutdown(); }

bool Fsr4Engine::selectedByConfig() {
    return GetPrivateProfileIntW(L"Upscaler", L"Fsr4", 0, iniPathNextToExecutable().c_str()) != 0;
}

bool Fsr4Engine::filesPresent() {
    const auto dir = moduleDirectory();
    std::error_code ec;
    return std::filesystem::exists(dir / L"amd_fidelityfx_loader_dx12.dll", ec) &&
           std::filesystem::exists(dir / L"amd_fidelityfx_upscaler_dx12.dll", ec);
}

bool Fsr4Engine::initialize(ID3D11Device* device) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    shutdown();
    if (!device) { impl_->setError(L"D3D11 device is null"); return false; }
    impl_->device = device;
    if (!impl_->loadDlls()) return false;

    // Use the side-loaded Agility SDK runtime if D3D12\D3D12Core.dll is staged next to the proxy.
    std::wstring agilityDir;
    const auto dir = moduleDirectory() / L"D3D12";
    std::error_code ec;
    if (std::filesystem::exists(dir / L"D3D12Core.dll", ec)) {
        agilityDir = dir.wstring();
        if (agilityDir.back() != L'\\') agilityDir += L'\\';
    }
    if (!impl_->bridge.initialize(device, agilityDir.empty() ? nullptr : agilityDir.c_str())) {
        impl_->setError(L"D3D11/D3D12 bridge initialization failed");
        return false;
    }
    if (!impl_->selectProvider()) {
        impl_->bridge.shutdown();
        impl_->device.Reset();
        return false;
    }
    impl_->ready = true;
    wchar_t buf[256]{};
    std::swprintf(buf, 256, L"EDPE FSR4: engine initialized (agilitySdk=%d)", impl_->bridge.usingAgilitySdk() ? 1 : 0);
    EdpeLog(buf);
    return true;
}

void Fsr4Engine::shutdown() {
    if (!impl_) return;
    impl_->ready = false;
    impl_->destroyResources();
    impl_->depthCs.Reset();
    impl_->ownDepthSrv.Reset();
    impl_->ownDepthSrvSource = nullptr;
    impl_->bridge.shutdown();
    impl_->device.Reset();
    impl_->evalCount = 0;
    impl_->resetPending = true;
}

bool Fsr4Engine::isReady() const { return impl_ && impl_->ready; }
const wchar_t* Fsr4Engine::lastError() const { return impl_ ? impl_->error : L""; }
const char* Fsr4Engine::providerVersion() const { return impl_ ? impl_->providerVer : "unavailable"; }

bool Fsr4Engine::evaluate(ID3D11DeviceContext* context, const TemporalFrameInputs& inputs,
                          ID3D11Texture2D* destination) {
    if (!impl_) return false;
    if (!context) { impl_->setError(L"D3D11 device context is null"); return false; }
    if (!destination) { impl_->setError(L"destination texture is null"); return false; }
    return impl_->evaluate(context, inputs, destination);
}

void Fsr4Engine::resetHistory() {
    if (impl_) impl_->resetPending = true;
}

#else // FSR 4 support not compiled in: inert stub so callers need no #if.

struct Fsr4Engine::Impl {
    wchar_t error[64] = L"FSR 4 support not compiled into EDPE";
};
Fsr4Engine::Fsr4Engine() : impl_(std::make_unique<Impl>()) {}
Fsr4Engine::~Fsr4Engine() = default;
bool Fsr4Engine::selectedByConfig() { return false; }
bool Fsr4Engine::filesPresent() { return false; }
bool Fsr4Engine::initialize(ID3D11Device*) { return false; }
void Fsr4Engine::shutdown() {}
bool Fsr4Engine::isReady() const { return false; }
const wchar_t* Fsr4Engine::lastError() const { return impl_ ? impl_->error : L""; }
const char* Fsr4Engine::providerVersion() const { return "unavailable"; }
bool Fsr4Engine::evaluate(ID3D11DeviceContext*, const TemporalFrameInputs&, ID3D11Texture2D*) { return false; }
void Fsr4Engine::resetHistory() {}

#endif

} // namespace edpe
