// CLSID_D3D12SDKConfiguration is defined here so test targets that do not link dxguid.lib resolve it
// (DEFINE_GUID is selectany under MSVC, so duplicate definitions are harmless).
#ifndef INITGUID
#define INITGUID
#endif
#include "d3d12_bridge.h"

#include "log.h"
#include "renderdoc_support.h"
#include "system_dll.h"

#include <cstdio>

namespace edpe {

namespace {

// Microsoft.Direct3D.D3D12 (Agility SDK) version whose D3D12Core.dll is staged beside the proxy.
constexpr UINT kAgilitySdkVersion = 616;

using PFN_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown* pAdapter,
                                               D3D_FEATURE_LEVEL MinimumFeatureLevel,
                                               REFIID riid,
                                               void** ppDevice);

using PFN_D3D12SerializeRootSignature = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC* pRootSignature,
                                                          D3D_ROOT_SIGNATURE_VERSION Version,
                                                          ID3DBlob** ppBlob,
                                                          ID3DBlob** ppErrorBlob);

const char* getWineVersionOrNull() {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return nullptr;
    using PFN_WineGetVersion = const char*(CDECL*)();
    const auto fn = reinterpret_cast<PFN_WineGetVersion>(GetProcAddress(ntdll, "wine_get_version"));
    return fn ? fn() : nullptr;
}

void logBridgeFailure(const wchar_t* stage, HRESULT hr) {
    wchar_t buf[384]{};
    if (const char* wineVer = getWineVersionOrNull()) {
        // Under Wine/Proton, DXVK <-> vkd3d-proton shared handles/fences require
        // VK_KHR_external_memory_win32 and VK_KHR_external_semaphore_win32 host support.
        std::swprintf(buf, 384,
                      L"EDPE D3D12Bridge: %ls failed (hr=0x%08X, wine=%S; check DXVK/vkd3d-proton external memory/semaphore support)",
                      stage, static_cast<unsigned>(hr), wineVer);
    } else {
        std::swprintf(buf, 384, L"EDPE D3D12Bridge: %ls failed (hr=0x%08X)",
                      stage, static_cast<unsigned>(hr));
    }
    EdpeLog(buf);
}

HMODULE loadPinnedSystemD3D12() {
    // Load from System32 (Windows native d3d12.dll, or vkd3d-proton d3d12.dll under Wine/Proton)
    // so a third-party d3d12.dll next to the game executable cannot hijack or recurse into the bridge.
    HMODULE mod = LoadSystemDll(L"d3d12.dll");
    if (!mod) return nullptr;
    // Pin d3d12.dll for process lifetime so reinitializing the bridge never unloads D3D12Core/WDDM
    // state while D3D11/DXGI still holds shared handles or adapters.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(mod),
        &pinned);
    return mod;
}

} // namespace

D3D11D3D12Bridge::~D3D11D3D12Bridge() {
    shutdown();
}

bool D3D11D3D12Bridge::initialize(ID3D11Device* d3d11Device, const wchar_t* agilitySdkDir) {
    shutdown();
    if (!d3d11Device) return false;

    HRESULT hr = d3d11Device->QueryInterface(IID_PPV_ARGS(&device11_5_));
    if (FAILED(hr)) {
        logBridgeFailure(L"QueryInterface(ID3D11Device5)", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDev));
    if (FAILED(hr) || FAILED(hr = dxgiDev->GetAdapter(&adapter))) {
        logBridgeFailure(L"IDXGIDevice::GetAdapter", hr);
        shutdown();
        return false;
    }

    if (GetModuleHandleW(L"renderdoc.dll") != nullptr) {
        // Windows RenderDoc does not track mid-frame GPU updates across D3D11<->D3D12
        // OpenSharedHandle/OpenSharedFence; Phase 13B+ FG should fail-open under renderdoc.dll.
        EdpeLog(L"EDPE D3D12Bridge: WARNING: renderdoc.dll detected; cross-API shared handles are not tracked mid-frame by Windows RenderDoc");
    }

    d3d12Module_ = loadPinnedSystemD3D12();
    if (!d3d12Module_) {
        logBridgeFailure(L"LoadSystemDll(d3d12.dll)", HRESULT_FROM_WIN32(GetLastError()));
        shutdown();
        return false;
    }

    auto createDevice = reinterpret_cast<PFN_D3D12CreateDevice>(
        GetProcAddress(d3d12Module_, "D3D12CreateDevice"));
    if (!createDevice) {
        logBridgeFailure(L"GetProcAddress(D3D12CreateDevice)", HRESULT_FROM_WIN32(GetLastError()));
        shutdown();
        return false;
    }

    // Optional: create the device from a side-loaded Agility SDK runtime (D3D12Core.dll in
    // agilitySdkDir). Falls back to the system runtime below if any step fails.
    if (agilitySdkDir && agilitySdkDir[0]) {
        using PFN_D3D12GetInterface = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
        auto getInterface = reinterpret_cast<PFN_D3D12GetInterface>(
            GetProcAddress(d3d12Module_, "D3D12GetInterface"));
        Microsoft::WRL::ComPtr<ID3D12SDKConfiguration1> sdkConfig;
        char sdkPathUtf8[MAX_PATH * 3]{};
        if (getInterface &&
            WideCharToMultiByte(CP_UTF8, 0, agilitySdkDir, -1, sdkPathUtf8,
                                static_cast<int>(sizeof(sdkPathUtf8)), nullptr, nullptr) > 0 &&
            SUCCEEDED(getInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&sdkConfig)))) {
            const HRESULT fhr = sdkConfig->CreateDeviceFactory(
                kAgilitySdkVersion, sdkPathUtf8, IID_PPV_ARGS(&deviceFactory_));
            if (SUCCEEDED(fhr)) {
                const HRESULT dhr = deviceFactory_->CreateDevice(
                    adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device12_));
                if (SUCCEEDED(dhr)) {
                    usingAgilitySdk_ = true;
                    EdpeLog(L"EDPE D3D12Bridge: D3D12 device created from side-loaded Agility SDK runtime");
                } else {
                    logBridgeFailure(L"ID3D12DeviceFactory::CreateDevice", dhr);
                    device12_.Reset();
                    deviceFactory_.Reset();
                }
            } else {
                logBridgeFailure(L"ID3D12SDKConfiguration1::CreateDeviceFactory", fhr);
            }
        } else {
            EdpeLog(L"EDPE D3D12Bridge: Agility SDK configuration interface unavailable; using system D3D12");
        }
    }

    if (!device12_) {
        hr = createDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device12_));
        if (FAILED(hr)) {
            logBridgeFailure(L"D3D12CreateDevice(FL_12_0)", hr);
            shutdown();
            return false;
        }
    }

    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    hr = device12_->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&queue_));
    if (FAILED(hr)) {
        qDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        hr = device12_->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&queue_));
        if (FAILED(hr)) {
            logBridgeFailure(L"CreateCommandQueue", hr);
            shutdown();
            return false;
        }
    }

    queue_->GetTimestampFrequency(&timestampFrequency_);

    for (uint32_t i = 0; i < kAllocatorCount; ++i) {
        hr = device12_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[i]));
        if (FAILED(hr)) {
            logBridgeFailure(L"CreateCommandAllocator", hr);
            shutdown();
            return false;
        }
    }

    hr = device12_->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr, IID_PPV_ARGS(&cmdList_));
    if (FAILED(hr)) {
        logBridgeFailure(L"CreateCommandList", hr);
        shutdown();
        return false;
    }
    cmdList_->Close();

    hr = device12_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12_));
    if (FAILED(hr)) {
        logBridgeFailure(L"CreateFence(SHARED)", hr);
        shutdown();
        return false;
    }

    HANDLE sharedFenceHandle = nullptr;
    hr = device12_->CreateSharedHandle(
        fence12_.Get(), nullptr, GENERIC_ALL, nullptr, &sharedFenceHandle);
    if (FAILED(hr) || !sharedFenceHandle) {
        if (sharedFenceHandle) CloseHandle(sharedFenceHandle);
        logBridgeFailure(L"CreateSharedHandle(fence)", hr);
        shutdown();
        return false;
    }

    const HRESULT openHr = device11_5_->OpenSharedFence(sharedFenceHandle, IID_PPV_ARGS(&fence11_));
    CloseHandle(sharedFenceHandle);
    if (FAILED(openHr)) {
        logBridgeFailure(L"OpenSharedFence", openHr);
        shutdown();
        return false;
    }

    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) {
        logBridgeFailure(L"CreateEventW", HRESULT_FROM_WIN32(GetLastError()));
        shutdown();
        return false;
    }

    // Label core D3D12 bridge objects so both Windows RenderDoc and Proton vkd3d-proton
    // (VK_EXT_debug_utils) display readable names when GPU annotations are active.
    SetD3D12DebugName(device12_.Get(), "EDPE_D3D12Bridge_Device");
    SetD3D12DebugName(queue_.Get(), "EDPE_D3D12Bridge_Queue");
    SetD3D12DebugName(cmdList_.Get(), "EDPE_D3D12Bridge_CmdList");
    SetD3D12DebugName(fence12_.Get(), "EDPE_D3D12Bridge_SharedFence");

    device11_ = d3d11Device;
    initialized_ = true;
    return true;
}

void D3D11D3D12Bridge::shutdown() {
    if (initialized_) {
        waitGpuIdle();
    }
    initialized_ = false;
    if (fenceEvent_) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
    fence11_.Reset();
    fence12_.Reset();
    cmdList_.Reset();
    for (auto& alloc : allocators_) {
        alloc.Reset();
    }
    allocatorFence_.fill(0);
    overrideQueue_.Reset();
    queue_.Reset();
    device12_.Reset();
    deviceFactory_.Reset();
    usingAgilitySdk_ = false;
    device11_5_.Reset();
    device11_ = nullptr;
    fenceValue_ = 0;
    frameCounter_ = 0;
    timestampFrequency_ = 0;
    d3d12Module_ = nullptr;
}

bool D3D11D3D12Bridge::setOverrideCommandQueue(ID3D12CommandQueue* overrideQueue) {
    if (!initialized_) return false;
    waitGpuIdle();
    overrideQueue_ = overrideQueue;
    timestampFrequency_ = 0;
    if (auto* q = activeQueue()) {
        q->GetTimestampFrequency(&timestampFrequency_);
    }
    return true;
}

bool D3D11D3D12Bridge::createSharedTexture(uint32_t width,
                                           uint32_t height,
                                           DXGI_FORMAT format,
                                           UINT d3d11BindFlags,
                                           SharedBridgeTexture& out) const {
    out.reset();
    if (!initialized_ || !device11_5_ || !device12_ || !width || !height ||
        format == DXGI_FORMAT_UNKNOWN) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = d3d11BindFlags;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    HRESULT hr = device11_5_->CreateTexture2D(&desc, nullptr, &out.d3d11);
    if (FAILED(hr)) {
        logBridgeFailure(L"CreateTexture2D(shared)", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIResource1> res1;
    hr = out.d3d11.As(&res1);
    if (FAILED(hr)) {
        logBridgeFailure(L"As(IDXGIResource1)", hr);
        out.reset();
        return false;
    }

    HANDLE ntHandle = nullptr;
    hr = res1->CreateSharedHandle(
        nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &ntHandle);
    if (FAILED(hr) || !ntHandle) {
        if (ntHandle) CloseHandle(ntHandle);
        logBridgeFailure(L"CreateSharedHandle(texture)", hr);
        out.reset();
        return false;
    }

    const HRESULT openHr = device12_->OpenSharedHandle(ntHandle, IID_PPV_ARGS(&out.d3d12));
    CloseHandle(ntHandle);
    if (FAILED(openHr)) {
        logBridgeFailure(L"OpenSharedHandle(texture)", openHr);
        out.reset();
        return false;
    }

    out.width = width;
    out.height = height;
    out.format = format;
    SetD3D11DebugName(out.d3d11.Get(), "EDPE_BridgeSharedTex_D3D11");
    SetD3D12DebugName(out.d3d12.Get(), "EDPE_BridgeSharedTex_D3D12");
    return true;
}

void D3D11D3D12Bridge::transitionResource(ID3D12GraphicsCommandList* cmdList,
                                          ID3D12Resource* resource,
                                          D3D12_RESOURCE_STATES before,
                                          D3D12_RESOURCE_STATES after) {
    if (!cmdList || !resource || before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    cmdList->ResourceBarrier(1, &barrier);
}

ID3D12GraphicsCommandList* D3D11D3D12Bridge::beginCommandList(uint32_t& outSlot,
                                                              bool* outDidCpuWait) {
    if (outDidCpuWait) *outDidCpuWait = false;
    if (!initialized_) return nullptr;

    outSlot = frameCounter_++ % kAllocatorCount;
    const uint64_t waitVal = allocatorFence_[outSlot];
    if (waitVal > 0 && fence12_->GetCompletedValue() < waitVal) {
        if (outDidCpuWait) *outDidCpuWait = true;
        if (SUCCEEDED(fence12_->SetEventOnCompletion(waitVal, fenceEvent_))) {
            WaitForSingleObject(fenceEvent_, 5000);
        }
    }

    if (FAILED(allocators_[outSlot]->Reset())) return nullptr;
    if (FAILED(cmdList_->Reset(allocators_[outSlot].Get(), nullptr))) return nullptr;
    return cmdList_.Get();
}

uint64_t D3D11D3D12Bridge::submitAfterD3D11Signal(ID3D11DeviceContext4* ctx4,
                                                  uint32_t slot,
                                                  bool flushD11) {
    if (!initialized_ || !ctx4 || slot >= kAllocatorCount) return 0;

    ID3D12CommandQueue* q = activeQueue();
    if (!q) {
        cmdList_->Close();
        return 0;
    }

    const uint64_t d11Done = ++fenceValue_;
    if (FAILED(ctx4->Signal(fence11_.Get(), d11Done))) {
        cmdList_->Close();
        return 0;
    }
    if (flushD11) {
        ctx4->Flush();
    }

    if (FAILED(cmdList_->Close())) return 0;

    if (FAILED(q->Wait(fence12_.Get(), d11Done))) return 0;
    ID3D12CommandList* lists[] = {cmdList_.Get()};
    q->ExecuteCommandLists(1, lists);

    const uint64_t d12Done = ++fenceValue_;
    if (FAILED(q->Signal(fence12_.Get(), d12Done))) return 0;
    allocatorFence_[slot] = d12Done;
    return d12Done;
}

uint64_t D3D11D3D12Bridge::signalD3D12Queue() {
    ID3D12CommandQueue* q = activeQueue();
    if (!initialized_ || !q || !fence12_) return 0;
    const uint64_t val = ++fenceValue_;
    if (FAILED(q->Signal(fence12_.Get(), val))) return 0;
    return val;
}

bool D3D11D3D12Bridge::waitOnD3D11(ID3D11DeviceContext4* ctx4, uint64_t d12DoneFence) const {
    if (!initialized_ || !ctx4 || !fence11_ || d12DoneFence == 0) return false;
    return SUCCEEDED(ctx4->Wait(fence11_.Get(), d12DoneFence));
}

void D3D11D3D12Bridge::waitGpuIdle() {
    ID3D12CommandQueue* q = activeQueue();
    if (!q || !fence12_ || !fenceEvent_) return;
    const uint64_t target = ++fenceValue_;
    if (SUCCEEDED(q->Signal(fence12_.Get(), target)) &&
        fence12_->GetCompletedValue() < target) {
        if (SUCCEEDED(fence12_->SetEventOnCompletion(target, fenceEvent_))) {
            WaitForSingleObject(fenceEvent_, 5000);
        }
    }
}

uint64_t D3D11D3D12Bridge::completedFenceValue() const {
    return fence12_ ? fence12_->GetCompletedValue() : 0;
}

HRESULT D3D11D3D12Bridge::serializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc,
                                                 D3D_ROOT_SIGNATURE_VERSION version,
                                                 ID3DBlob** blob,
                                                 ID3DBlob** errorBlob) {
    // Resolve from System32 and pin so root-signature serialization uses the same runtime
    // (Windows d3d12.dll or Wine/Proton vkd3d-proton) as initialize().
    HMODULE mod = loadPinnedSystemD3D12();
    if (!mod) return HRESULT_FROM_WIN32(GetLastError());

    auto fn = reinterpret_cast<PFN_D3D12SerializeRootSignature>(
        GetProcAddress(mod, "D3D12SerializeRootSignature"));
    return fn ? fn(desc, version, blob, errorBlob)
              : HRESULT_FROM_WIN32(GetLastError());
}

} // namespace edpe
