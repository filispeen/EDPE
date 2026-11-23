#include "d3d12_bridge.h"
#include "renderdoc_support.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// D3D11 compute shader extracting .r from R32G8X24_TYPELESS (R32_FLOAT_X8X24_TYPELESS SRV) to R32_FLOAT UAV.
constexpr char kDepthExtractCs[] = R"(
Texture2D<float2> srcDepth : register(t0);
RWTexture2D<float> dstDepth : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    dstDepth.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    dstDepth[id.xy] = srcDepth.Load(int3(id.xy, 0)).r;
}
)";

// D3D12 compute shader reading 4 shared D3D11 textures and writing deterministic result to shared output.
constexpr char kBridgeRoundtripCs[] = R"(
Texture2D<float4> inColor  : register(t0);
Texture2D<float>  inDepth  : register(t1);
Texture2D<float2> inMotion : register(t2);
Texture2D<float4> inUi     : register(t3);
RWTexture2D<float4> outTex : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    outTex.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float4 c = inColor.Load(int3(id.xy, 0));
    float  d = inDepth.Load(int3(id.xy, 0));
    float2 m = inMotion.Load(int3(id.xy, 0));
    float4 u = inUi.Load(int3(id.xy, 0));
    // Deterministic encode combining all 4 inputs:
    outTex[id.xy] = float4(
        saturate(c.r * 0.5f + u.r * 0.5f),
        saturate(d),
        saturate(m.x * 0.25f + 0.5f),
        1.0f);
}
)";

bool createD3D11Device(bool preferHardware,
                       ComPtr<ID3D11Device>& outDevice,
                       ComPtr<ID3D11DeviceContext>& outContext,
                       ComPtr<IDXGIAdapter3>& outAdapter3,
                       std::wstring& outAdapterName,
                       bool& outIsHardware) {
    outIsHardware = false;
    if (preferHardware) {
        ComPtr<IDXGIFactory4> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> adapter;
                if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC1 desc{};
                adapter->GetDesc1(&desc);
                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

                D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
                if (SUCCEEDED(D3D11CreateDevice(
                        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1,
                        D3D11_SDK_VERSION, &outDevice, nullptr, &outContext))) {
                    // Verify D3D12 can also open this adapter.
                    edpe::D3D11D3D12Bridge probe;
                    if (probe.initialize(outDevice.Get())) {
                        probe.shutdown();
                        adapter.As(&outAdapter3);
                        outAdapterName = desc.Description;
                        outIsHardware = true;
                        return true;
                    }
                    outContext.Reset();
                    outDevice.Reset();
                }
            }
        }
    }

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1,
                                 D3D11_SDK_VERSION, &outDevice, nullptr, &outContext))) {
        return false;
    }
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(outDevice.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
        adapter.As(&outAdapter3);
    }
    outAdapterName = L"WARP Software Adapter";
    return true;
}

uint64_t queryLocalVramBytes(IDXGIAdapter3* adapter3) {
    if (!adapter3) return 0;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        return 0;
    }
    return info.CurrentUsage;
}

struct D3D12ComputeTestState {
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> srvUavHeap;
    UINT descSize = 0;

    bool init(ID3D12Device* dev) {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 4;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 2;
        param.DescriptorTable.pDescriptorRanges = ranges;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 1;
        rsDesc.pParameters = &param;

        ComPtr<ID3DBlob> sigBlob, errBlob;
        if (FAILED(edpe::D3D11D3D12Bridge::serializeRootSignature(
                &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob))) {
            return false;
        }
        if (FAILED(dev->CreateRootSignature(
                0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                IID_PPV_ARGS(&rootSig)))) {
            return false;
        }

        ComPtr<ID3DBlob> csBlob;
        if (FAILED(D3DCompile(kBridgeRoundtripCs, sizeof(kBridgeRoundtripCs) - 1, nullptr,
                              nullptr, nullptr, "main", "cs_5_0", 0, 0, &csBlob, nullptr))) {
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = rootSig.Get();
        psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
        if (FAILED(dev->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)))) {
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 5;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvUavHeap)))) {
            return false;
        }
        descSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    }

    void bindTextures(ID3D12Device* dev,
                      ID3D12Resource* color,
                      ID3D12Resource* depth,
                      ID3D12Resource* motion,
                      ID3D12Resource* ui,
                      ID3D12Resource* output) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srvUavHeap->GetCPUDescriptorHandleForHeapStart();

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;

        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dev->CreateShaderResourceView(color, &srv, h);
        h.ptr += descSize;

        srv.Format = DXGI_FORMAT_R32_FLOAT;
        dev->CreateShaderResourceView(depth, &srv, h);
        h.ptr += descSize;

        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        dev->CreateShaderResourceView(motion, &srv, h);
        h.ptr += descSize;

        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dev->CreateShaderResourceView(ui, &srv, h);
        h.ptr += descSize;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(output, nullptr, &uav, h);
    }

    void reset() {
        srvUavHeap.Reset();
        pso.Reset();
        rootSig.Reset();
    }
};

struct BridgeSharedSet {
    edpe::SharedBridgeTexture color;
    edpe::SharedBridgeTexture depth;
    edpe::SharedBridgeTexture motion;
    edpe::SharedBridgeTexture ui;
    edpe::SharedBridgeTexture output;

    bool create(const edpe::D3D11D3D12Bridge& bridge, uint32_t w, uint32_t h) {
        return bridge.createSharedTexture(
                   w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, color) &&
               bridge.createSharedTexture(
                   w, h, DXGI_FORMAT_R32_FLOAT,
                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, depth) &&
               bridge.createSharedTexture(
                   w, h, DXGI_FORMAT_R16G16_FLOAT,
                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, motion) &&
               bridge.createSharedTexture(
                   w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, ui) &&
               bridge.createSharedTexture(
                   w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, output);
    }

    void reset() {
        output.reset();
        ui.reset();
        motion.reset();
        depth.reset();
        color.reset();
    }
};

int runCorrectnessTest(ID3D11Device* d3d11Dev, ID3D11DeviceContext* d3d11Ctx) {
    ComPtr<ID3D11DeviceContext4> ctx4;
    if (FAILED(d3d11Ctx->QueryInterface(IID_PPV_ARGS(&ctx4)))) return 10;

    // Verify fail-open with null device.
    {
        edpe::D3D11D3D12Bridge nullBridge;
        if (nullBridge.initialize(nullptr)) return 11;
        if (nullBridge.isInitialized()) return 12;
    }

    // An Agility SDK directory without D3D12Core.dll must fall back to the system D3D12 runtime.
    {
        edpe::D3D11D3D12Bridge fallback;
        if (!fallback.initialize(d3d11Dev, L"C:\\edpe-no-such-agility-dir\\")) return 140;
        if (fallback.usingAgilitySdk()) return 141;
    }

    edpe::D3D11D3D12Bridge bridge;
    if (!bridge.initialize(d3d11Dev)) return 13;

    constexpr uint32_t W = 16;
    constexpr uint32_t H = 16;

    // Source D3D11 textures (simulating Elite's scene color, planar DSV #2 depth, motion, UI).
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;

    std::vector<uint32_t> colorInit(W * H, 0xFF0000CCu); // R=0xCC (~0.8), G=0, B=0, A=1
    std::vector<uint32_t> uiInit(W * H, 0xFF000034u);    // R=0x34 (~0.2), G=0, B=0, A=1
    // R16G16_FLOAT: half(1.0) = 0x3C00 -> x=1.0 -> 0.25 + 0.5 = 0.75 (~191)
    std::vector<uint32_t> motionInit(W * H, 0x00003C00u);
    // R32G8X24_TYPELESS: 8 bytes per texel; first float = 0.5f (0x3F000000) -> ~128
    std::vector<uint64_t> depthInit(W * H, 0x000000003F000000ull);

    auto makeTex = [&](DXGI_FORMAT fmt, UINT bind, const void* data, UINT pitch,
                       ComPtr<ID3D11Texture2D>& outTex) -> bool {
        td.Format = fmt;
        td.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA srd{data, pitch, 0};
        return SUCCEEDED(d3d11Dev->CreateTexture2D(&td, data ? &srd : nullptr, &outTex));
    };

    ComPtr<ID3D11Texture2D> srcColor, srcDepthPlanar, srcMotion, srcUi, dstReturn;
    if (!makeTex(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, colorInit.data(), W * 4, srcColor) ||
        !makeTex(DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL,
                 depthInit.data(), W * 8, srcDepthPlanar) ||
        !makeTex(DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, motionInit.data(), W * 4, srcMotion) ||
        !makeTex(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, uiInit.data(), W * 4, srcUi) ||
        !makeTex(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, nullptr, 0, dstReturn)) {
        return 14;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
    depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    depthSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    depthSrvDesc.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> depthSrv;
    if (FAILED(d3d11Dev->CreateShaderResourceView(srcDepthPlanar.Get(), &depthSrvDesc, &depthSrv))) {
        return 15;
    }

    // D3D11 depth extraction shader.
    ComPtr<ID3DBlob> depthCsBlob;
    ComPtr<ID3D11ComputeShader> depthCs;
    if (FAILED(D3DCompile(kDepthExtractCs, sizeof(kDepthExtractCs) - 1, nullptr, nullptr, nullptr,
                          "main", "cs_5_0", 0, 0, &depthCsBlob, nullptr)) ||
        FAILED(d3d11Dev->CreateComputeShader(depthCsBlob->GetBufferPointer(),
                                             depthCsBlob->GetBufferSize(), nullptr, &depthCs))) {
        return 16;
    }

    BridgeSharedSet shared;
    if (!shared.create(bridge, W, H)) return 17;

    D3D11_UNORDERED_ACCESS_VIEW_DESC depthUavDesc{};
    depthUavDesc.Format = DXGI_FORMAT_R32_FLOAT;
    depthUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11UnorderedAccessView> sharedDepthUav;
    if (FAILED(d3d11Dev->CreateUnorderedAccessView(shared.depth.d3d11.Get(), &depthUavDesc,
                                                   &sharedDepthUav))) {
        return 18;
    }

    D3D12ComputeTestState d12Compute;
    if (!d12Compute.init(bridge.d3d12Device())) return 19;
    d12Compute.bindTextures(bridge.d3d12Device(),
                            shared.color.d3d12.Get(),
                            shared.depth.d3d12.Get(),
                            shared.motion.d3d12.Get(),
                            shared.ui.d3d12.Get(),
                            shared.output.d3d12.Get());

    // Execute D3D11 -> shared handle -> D3D12 compute -> shared result -> D3D11 round-trip.
    d3d11Ctx->CopyResource(shared.color.d3d11.Get(), srcColor.Get());
    d3d11Ctx->CopyResource(shared.motion.d3d11.Get(), srcMotion.Get());
    d3d11Ctx->CopyResource(shared.ui.d3d11.Get(), srcUi.Get());

    ID3D11ShaderResourceView* srvs[] = {depthSrv.Get()};
    ID3D11UnorderedAccessView* uavs[] = {sharedDepthUav.Get()};
    d3d11Ctx->CSSetShader(depthCs.Get(), nullptr, 0);
    d3d11Ctx->CSSetShaderResources(0, 1, srvs);
    d3d11Ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    d3d11Ctx->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
    ID3D11UnorderedAccessView* nullUav[] = {nullptr};
    ID3D11ShaderResourceView* nullSrv[] = {nullptr};
    d3d11Ctx->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
    d3d11Ctx->CSSetShaderResources(0, 1, nullSrv);

    uint32_t slot = 0;
    auto* cmdList = bridge.beginCommandList(slot);
    if (!cmdList) return 20;

    // Exercise D3D12 debug naming & BeginEvent/EndEvent annotations safely on a live command list.
    edpe::RenderDocBridge::instance().setForceAnnotations(true);
    {
        edpe::ScopedD3D12RenderDocEvent d12Event(cmdList, L"EDPE_BridgeTest: Roundtrip");
        edpe::SetD3D12DebugName(shared.output.d3d12.Get(), "EDPE_BridgeTest_Output");
        edpe::D3D11D3D12Bridge::transitionResource(
        cmdList, shared.color.d3d12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    edpe::D3D11D3D12Bridge::transitionResource(
        cmdList, shared.depth.d3d12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    edpe::D3D11D3D12Bridge::transitionResource(
        cmdList, shared.motion.d3d12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    edpe::D3D11D3D12Bridge::transitionResource(
        cmdList, shared.ui.d3d12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    edpe::D3D11D3D12Bridge::transitionResource(
        cmdList, shared.output.d3d12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* heaps[] = {d12Compute.srvUavHeap.Get()};
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(d12Compute.rootSig.Get());
    cmdList->SetPipelineState(d12Compute.pso.Get());
    cmdList->SetComputeRootDescriptorTable(0, d12Compute.srvUavHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->Dispatch((W + 7) / 8, (H + 7) / 8, 1);

    edpe::D3D11D3D12Bridge::transitionResource(
        cmdList, shared.color.d3d12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    edpe::D3D11D3D12Bridge::transitionResource(
        cmdList, shared.depth.d3d12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    edpe::D3D11D3D12Bridge::transitionResource(
        cmdList, shared.motion.d3d12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    edpe::D3D11D3D12Bridge::transitionResource(
        cmdList, shared.ui.d3d12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    edpe::D3D11D3D12Bridge::transitionResource(
        cmdList, shared.output.d3d12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    }
    edpe::RenderDocBridge::instance().setForceAnnotations(false);

    const uint64_t d12Done = bridge.submitAfterD3D11Signal(ctx4.Get(), slot, true);
    if (d12Done == 0) return 21;
    if (!bridge.waitOnD3D11(ctx4.Get(), d12Done)) return 22;

    d3d11Ctx->CopyResource(dstReturn.Get(), shared.output.d3d11.Get());

    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(d3d11Dev->CreateTexture2D(&td, nullptr, &staging))) return 23;
    d3d11Ctx->CopyResource(staging.Get(), dstReturn.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(d3d11Ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 24;
    const auto* row = static_cast<const uint8_t*>(mapped.pData);
    // Expected: R = (0xCC + 0x34)/2 = 0x80 (~128), G = 0.5 (~128), B = 0.75 (~191), A = 255
    const uint8_t r = row[0];
    const uint8_t g = row[1];
    const uint8_t b = row[2];
    const uint8_t a = row[3];
    d3d11Ctx->Unmap(staging.Get(), 0);

    if (std::abs(int(r) - 128) > 2 || std::abs(int(g) - 128) > 2 ||
        std::abs(int(b) - 191) > 2 || a != 255) {
        std::printf("Pixel mismatch: got (%u,%u,%u,%u)\n", r, g, b, a);
        return 25;
    }

    // Verify clean resize/recreation without leaking handles.
    bridge.waitGpuIdle();
    shared.reset();
    if (!shared.create(bridge, 32, 32)) return 26;
    shared.reset();
    d12Compute.reset();
    bridge.shutdown();
    return 0;
}

void runBenchmark1440p(ID3D11Device* d3d11Dev,
                       ID3D11DeviceContext* d3d11Ctx,
                       IDXGIAdapter3* adapter3,
                       const std::wstring& adapterName) {
    ComPtr<ID3D11DeviceContext4> ctx4;
    if (FAILED(d3d11Ctx->QueryInterface(IID_PPV_ARGS(&ctx4)))) return;

    edpe::D3D11D3D12Bridge bridge;
    if (!bridge.initialize(d3d11Dev)) return;

    constexpr uint32_t W = 2560;
    constexpr uint32_t H = 1440;
    constexpr uint32_t kWarmupFrames = 10;
    constexpr uint32_t kMeasuredFrames = 120;

    // Simulate Elite source textures at 2560x1440.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;

    ComPtr<ID3D11Texture2D> srcColor, srcDepthPlanar, srcMotion, srcUi, dstBackbuffer;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d3d11Dev->CreateTexture2D(&td, nullptr, &srcColor);
    d3d11Dev->CreateTexture2D(&td, nullptr, &srcUi);
    d3d11Dev->CreateTexture2D(&td, nullptr, &dstBackbuffer);

    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    d3d11Dev->CreateTexture2D(&td, nullptr, &srcMotion);

    td.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
    d3d11Dev->CreateTexture2D(&td, nullptr, &srcDepthPlanar);

    D3D11_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
    depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    depthSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    depthSrvDesc.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> depthSrv;
    d3d11Dev->CreateShaderResourceView(srcDepthPlanar.Get(), &depthSrvDesc, &depthSrv);

    ComPtr<ID3DBlob> depthCsBlob;
    ComPtr<ID3D11ComputeShader> depthCs;
    D3DCompile(kDepthExtractCs, sizeof(kDepthExtractCs) - 1, nullptr, nullptr, nullptr,
               "main", "cs_5_0", 0, 0, &depthCsBlob, nullptr);
    d3d11Dev->CreateComputeShader(depthCsBlob->GetBufferPointer(), depthCsBlob->GetBufferSize(),
                                  nullptr, &depthCs);

    const uint64_t vramBefore = queryLocalVramBytes(adapter3);
    BridgeSharedSet shared;
    if (!shared.create(bridge, W, H)) {
        std::printf("Benchmark skipped: failed to create 1440p shared textures.\n");
        return;
    }
    const uint64_t vramAfter = queryLocalVramBytes(adapter3);

    D3D11_UNORDERED_ACCESS_VIEW_DESC depthUavDesc{};
    depthUavDesc.Format = DXGI_FORMAT_R32_FLOAT;
    depthUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11UnorderedAccessView> sharedDepthUav;
    d3d11Dev->CreateUnorderedAccessView(shared.depth.d3d11.Get(), &depthUavDesc, &sharedDepthUav);

    D3D12ComputeTestState d12Compute;
    if (!d12Compute.init(bridge.d3d12Device())) return;
    d12Compute.bindTextures(bridge.d3d12Device(),
                            shared.color.d3d12.Get(),
                            shared.depth.d3d12.Get(),
                            shared.motion.d3d12.Get(),
                            shared.ui.d3d12.Get(),
                            shared.output.d3d12.Get());

    // D3D12 timestamp query heap + readback buffer (2 timestamps per allocator slot).
    constexpr uint32_t kSlots = edpe::D3D11D3D12Bridge::kAllocatorCount;
    D3D12_QUERY_HEAP_DESC qhDesc{};
    qhDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhDesc.Count = kSlots * 2;
    ComPtr<ID3D12QueryHeap> d12QueryHeap;
    bridge.d3d12Device()->CreateQueryHeap(&qhDesc, IID_PPV_ARGS(&d12QueryHeap));

    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = sizeof(uint64_t) * kSlots * 2;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> d12Readback;
    bridge.d3d12Device()->CreateCommittedResource(
        &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&d12Readback));

    // D3D11 async timestamp ring (separate disjoint brackets before/after cross-queue Wait).
    std::array<ComPtr<ID3D11Query>, kSlots> d11InDisjoint, d11InStart, d11InEnd;
    std::array<ComPtr<ID3D11Query>, kSlots> d11OutDisjoint, d11OutStart, d11OutEnd;
    for (uint32_t i = 0; i < kSlots; ++i) {
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        d3d11Dev->CreateQuery(&qd, &d11InDisjoint[i]);
        d3d11Dev->CreateQuery(&qd, &d11OutDisjoint[i]);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        d3d11Dev->CreateQuery(&qd, &d11InStart[i]);
        d3d11Dev->CreateQuery(&qd, &d11InEnd[i]);
        d3d11Dev->CreateQuery(&qd, &d11OutStart[i]);
        d3d11Dev->CreateQuery(&qd, &d11OutEnd[i]);
    }
    std::array<bool, kSlots> slotHasQuery{};

    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);

    auto measurePass = [&](bool pacedFrame,
                           std::vector<double>& outCpuSubmitMs,
                           std::vector<double>& outTotalWallMs,
                           std::vector<double>& outD11InGpuMs,
                           std::vector<double>& outD11OutGpuMs,
                           std::vector<double>& outD12GpuMs,
                           uint32_t& outCpuWaits) {
        outCpuSubmitMs.clear();
        outTotalWallMs.clear();
        outD11InGpuMs.clear();
        outD11OutGpuMs.clear();
        outD12GpuMs.clear();
        outCpuWaits = 0;
        slotHasQuery.fill(false);

        auto collectTimestampBracket = [](ID3D11DeviceContext* ctx,
                                          ID3D11Query* disjointQ,
                                          ID3D11Query* startQ,
                                          ID3D11Query* endQ,
                                          std::vector<double>& dst) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            if (ctx->GetData(disjointQ, &dj, sizeof(dj), 0) == S_OK && !dj.Disjoint && dj.Frequency > 0) {
                uint64_t t0 = 0, t1 = 0;
                if (ctx->GetData(startQ, &t0, sizeof(t0), 0) == S_OK &&
                    ctx->GetData(endQ, &t1, sizeof(t1), 0) == S_OK && t1 >= t0) {
                    dst.push_back(static_cast<double>(t1 - t0) / (static_cast<double>(dj.Frequency) / 1000.0));
                }
            }
        };

        for (uint32_t frame = 0; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
            const uint32_t ringIdx = frame % kSlots;
            const bool measure = frame >= kWarmupFrames;

            if (measure && slotHasQuery[ringIdx]) {
                collectTimestampBracket(d3d11Ctx, d11InDisjoint[ringIdx].Get(),
                                        d11InStart[ringIdx].Get(), d11InEnd[ringIdx].Get(), outD11InGpuMs);
                collectTimestampBracket(d3d11Ctx, d11OutDisjoint[ringIdx].Get(),
                                        d11OutStart[ringIdx].Get(), d11OutEnd[ringIdx].Get(), outD11OutGpuMs);
                if (bridge.timestampFrequency() > 0) {
                    uint64_t* mappedTs = nullptr;
                    D3D12_RANGE range{sizeof(uint64_t) * ringIdx * 2, sizeof(uint64_t) * (ringIdx * 2 + 2)};
                    if (SUCCEEDED(d12Readback->Map(0, &range, reinterpret_cast<void**>(&mappedTs)))) {
                        const uint64_t ts0 = mappedTs[ringIdx * 2];
                        const uint64_t ts1 = mappedTs[ringIdx * 2 + 1];
                        D3D12_RANGE emptyRange{0, 0};
                        d12Readback->Unmap(0, &emptyRange);
                        if (ts1 >= ts0) {
                            outD12GpuMs.push_back(
                                static_cast<double>(ts1 - ts0) /
                                (static_cast<double>(bridge.timestampFrequency()) / 1000.0));
                        }
                    }
                }
            }

            if (pacedFrame) {
                // Simulate realistic ~8 ms game render cadence (125 FPS) between submissions.
                Sleep(8);
            }

            LARGE_INTEGER tFrameStart{}, tSubmitStart{}, tSubmitEnd{}, tFrameEnd{};
            QueryPerformanceCounter(&tFrameStart);

            // 1. D3D11 copies to shared input textures + planar depth extraction.
            d3d11Ctx->Begin(d11InDisjoint[ringIdx].Get());
            d3d11Ctx->End(d11InStart[ringIdx].Get());
            d3d11Ctx->CopyResource(shared.color.d3d11.Get(), srcColor.Get());
            d3d11Ctx->CopyResource(shared.motion.d3d11.Get(), srcMotion.Get());
            d3d11Ctx->CopyResource(shared.ui.d3d11.Get(), srcUi.Get());

            ID3D11ShaderResourceView* srvs[] = {depthSrv.Get()};
            ID3D11UnorderedAccessView* uavs[] = {sharedDepthUav.Get()};
            d3d11Ctx->CSSetShader(depthCs.Get(), nullptr, 0);
            d3d11Ctx->CSSetShaderResources(0, 1, srvs);
            d3d11Ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
            d3d11Ctx->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
            ID3D11UnorderedAccessView* nullUav[] = {nullptr};
            ID3D11ShaderResourceView* nullSrv[] = {nullptr};
            d3d11Ctx->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
            d3d11Ctx->CSSetShaderResources(0, 1, nullSrv);

            d3d11Ctx->End(d11InEnd[ringIdx].Get());
            d3d11Ctx->End(d11InDisjoint[ringIdx].Get());

            // 2. D3D12 command list record + fence handoff.
            QueryPerformanceCounter(&tSubmitStart);
            bool didCpuWait = false;
            uint32_t slot = 0;
            auto* cmdList = bridge.beginCommandList(slot, &didCpuWait);
            if (measure && didCpuWait) ++outCpuWaits;

            cmdList->EndQuery(d12QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
            edpe::D3D11D3D12Bridge::transitionResource(
                cmdList, shared.color.d3d12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            edpe::D3D11D3D12Bridge::transitionResource(
                cmdList, shared.depth.d3d12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            edpe::D3D11D3D12Bridge::transitionResource(
                cmdList, shared.motion.d3d12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            edpe::D3D11D3D12Bridge::transitionResource(
                cmdList, shared.ui.d3d12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            edpe::D3D11D3D12Bridge::transitionResource(
                cmdList, shared.output.d3d12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            ID3D12DescriptorHeap* heaps[] = {d12Compute.srvUavHeap.Get()};
            cmdList->SetDescriptorHeaps(1, heaps);
            cmdList->SetComputeRootSignature(d12Compute.rootSig.Get());
            cmdList->SetPipelineState(d12Compute.pso.Get());
            cmdList->SetComputeRootDescriptorTable(0, d12Compute.srvUavHeap->GetGPUDescriptorHandleForHeapStart());
            cmdList->Dispatch((W + 7) / 8, (H + 7) / 8, 1);

            edpe::D3D11D3D12Bridge::transitionResource(
                cmdList, shared.color.d3d12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            edpe::D3D11D3D12Bridge::transitionResource(
                cmdList, shared.depth.d3d12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            edpe::D3D11D3D12Bridge::transitionResource(
                cmdList, shared.motion.d3d12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            edpe::D3D11D3D12Bridge::transitionResource(
                cmdList, shared.ui.d3d12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            edpe::D3D11D3D12Bridge::transitionResource(
                cmdList, shared.output.d3d12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

            cmdList->EndQuery(d12QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
            cmdList->ResolveQueryData(
                d12QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2,
                d12Readback.Get(), sizeof(uint64_t) * slot * 2);

            const uint64_t d12Done = bridge.submitAfterD3D11Signal(ctx4.Get(), slot, true);
            bridge.waitOnD3D11(ctx4.Get(), d12Done);
            QueryPerformanceCounter(&tSubmitEnd);

            // 3. Return copy back to D3D11.
            d3d11Ctx->Begin(d11OutDisjoint[ringIdx].Get());
            d3d11Ctx->End(d11OutStart[ringIdx].Get());
            d3d11Ctx->CopyResource(dstBackbuffer.Get(), shared.output.d3d11.Get());
            d3d11Ctx->End(d11OutEnd[ringIdx].Get());
            d3d11Ctx->End(d11OutDisjoint[ringIdx].Get());
            slotHasQuery[ringIdx] = true;

            QueryPerformanceCounter(&tFrameEnd);
            if (measure) {
                const double qpcMs = static_cast<double>(qpcFreq.QuadPart) / 1000.0;
                outCpuSubmitMs.push_back(static_cast<double>(tSubmitEnd.QuadPart - tSubmitStart.QuadPart) / qpcMs);
                outTotalWallMs.push_back(static_cast<double>(tFrameEnd.QuadPart - tFrameStart.QuadPart) / qpcMs);
            }
        }
        bridge.waitGpuIdle();
        d3d11Ctx->Flush();
    };

    std::vector<double> cpuSubmitMs, totalFrameWallMs, d11CopyInGpuMs, d11ReturnCopyGpuMs, d12ComputeGpuMs;
    uint32_t cpuAllocatorWaits = 0;

    // Pass 1: Saturated GPU throughput (measures raw GPU copy + compute round-trip).
    measurePass(false, cpuSubmitMs, totalFrameWallMs, d11CopyInGpuMs, d11ReturnCopyGpuMs, d12ComputeGpuMs, cpuAllocatorWaits);

    // Pass 2: Paced ~125 Hz cadence (measures real-game steady-state CPU submit overhead without GPU backpressure).
    std::vector<double> pacedCpuSubmitMs, pacedTotalWallMs, pacedD11InMs, pacedD11OutMs, pacedD12Ms;
    uint32_t pacedCpuWaits = 0;
    measurePass(true, pacedCpuSubmitMs, pacedTotalWallMs, pacedD11InMs, pacedD11OutMs, pacedD12Ms, pacedCpuWaits);

    auto meanOf = [](const std::vector<double>& v) {
        if (v.empty()) return 0.0;
        double s = 0.0;
        for (double x : v) s += x;
        return s / static_cast<double>(v.size());
    };
    auto stddevOf = [&](const std::vector<double>& v) {
        if (v.size() < 2) return 0.0;
        const double m = meanOf(v);
        double acc = 0.0;
        for (double x : v) acc += (x - m) * (x - m);
        return std::sqrt(acc / static_cast<double>(v.size()));
    };

    const double vramDeltaMb =
        vramAfter >= vramBefore ? static_cast<double>(vramAfter - vramBefore) / (1024.0 * 1024.0) : 0.0;

    std::printf("=== Phase 13A D3D11 <-> D3D12 Bridge Benchmark (2560x1440) ===\n");
    std::printf("Adapter: %ls\n", adapterName.c_str());
    std::printf("Measured frames per pass: %u (warmup %u)\n", kMeasuredFrames, kWarmupFrames);
    std::printf("Shared textures (4 in + 1 out): VRAM delta = %.2f MB\n", vramDeltaMb);
    std::printf("[Saturated throughput] CPU wall = %.4f ms (stddev = %.4f ms), allocator waits = %u\n",
                meanOf(totalFrameWallMs), stddevOf(totalFrameWallMs), cpuAllocatorWaits);
    std::printf("[Saturated throughput] GPU D3D11 in (%zu samples) = %.4f ms | D3D12 compute (%zu) = %.4f ms | D3D11 out (%zu) = %.4f ms | total GPU = %.4f ms\n",
                d11CopyInGpuMs.size(), meanOf(d11CopyInGpuMs),
                d12ComputeGpuMs.size(), meanOf(d12ComputeGpuMs),
                d11ReturnCopyGpuMs.size(), meanOf(d11ReturnCopyGpuMs),
                meanOf(d11CopyInGpuMs) + meanOf(d12ComputeGpuMs) + meanOf(d11ReturnCopyGpuMs));
    std::printf("[Paced ~125Hz cadence] CPU submit/fence = %.4f ms (stddev = %.4f ms), allocator waits = %u\n",
                meanOf(pacedCpuSubmitMs), stddevOf(pacedCpuSubmitMs), pacedCpuWaits);
    std::printf("[Paced ~125Hz cadence] GPU D3D11 in (%zu) = %.4f ms | D3D12 compute (%zu) = %.4f ms | D3D11 out (%zu) = %.4f ms | total GPU = %.4f ms\n",
                pacedD11InMs.size(), meanOf(pacedD11InMs),
                pacedD12Ms.size(), meanOf(pacedD12Ms),
                pacedD11OutMs.size(), meanOf(pacedD11OutMs),
                meanOf(pacedD11InMs) + meanOf(pacedD12Ms) + meanOf(pacedD11OutMs));

    shared.reset();
    d12Compute.reset();
    bridge.shutdown();
}

} // namespace

int main() {
    ComPtr<ID3D11Device> d3d11Dev;
    ComPtr<ID3D11DeviceContext> d3d11Ctx;
    ComPtr<IDXGIAdapter3> adapter3;
    std::wstring adapterName;
    bool isHardware = false;

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (!createD3D11Device(true, d3d11Dev, d3d11Ctx, adapter3, adapterName, isHardware)) {
        std::printf("Failed to initialize D3D11+D3D12 test device.\n");
        return 1;
    }

    const int rc = runCorrectnessTest(d3d11Dev.Get(), d3d11Ctx.Get());
    if (rc != 0) {
        std::printf("Correctness test failed with code %d on %ls.\n", rc, adapterName.c_str());
        return rc;
    }
    std::printf("Correctness test PASSED on %ls.\n", adapterName.c_str());

    if (isHardware) {
        runBenchmark1440p(d3d11Dev.Get(), d3d11Ctx.Get(), adapter3.Get(), adapterName);
    } else {
        std::printf("Hardware D3D12 adapter unavailable; 1440p hardware benchmark skipped.\n");
    }

    return 0;
}
