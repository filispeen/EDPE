#include "fg_backend.h"
#include "fg_ui_pack_pass.h"

#include <DirectXPackedVector.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace {

int fail(const char* msg) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

class MockFgBackend : public edpe::IFrameGenerationBackend {
public:
    edpe::FrameGenerationBackend backendType() const override { return edpe::FrameGenerationBackend::AmdFsrFg; }
    const wchar_t* name() const override { return L"Mock FG"; }
    bool isAvailable() const override { return available; }
    const wchar_t* unavailableReason() const override { return L"mock unavailable"; }
    uint32_t maxFramesToGenerate() const override { return maxGen; }
    bool isDynamicMfgSupported() const override { return false; }
    bool initialize(edpe::D3D12ProxyPresenter* /*presenter*/) override { return available; }
    void shutdown() override { shutdownCalled = true; }
    bool evaluate(ID3D12GraphicsCommandList* /*cmdList*/,
                  const edpe::FrameGenerationInputs& inputs,
                  bool enabled,
                  uint32_t requestedExtraFrames,
                  uint32_t* outGeneratedFrames) override {
        lastReset = inputs.reset;
        if (shouldFail) {
            if (outGeneratedFrames) *outGeneratedFrames = 0;
            return false;
        }
        const uint32_t gen = enabled ? std::min(requestedExtraFrames, maxGen) : 0u;
        if (outGeneratedFrames) *outGeneratedFrames = gen;
        return true;
    }
    void resetHistory() override {}
    void setDebugView(bool enabled) override { debugView = enabled; }
    bool isDebugViewEnabled() const override { return debugView; }
    bool presentsRealFrame() const override { return !debugView; }

    bool available = true;
    bool shouldFail = false;
    bool debugView = false;
    bool lastReset = false;
    bool shutdownCalled = false;
    uint32_t maxGen = 1;
};

int testCameraMatricesAgainstMotion() {
    // Odyssey captured camera matrices + delta.
    edpe::CameraProjection prev{{-.965338f, -.0708149f, -.251214f, 19.5594f,
                                 -.00114341f, .963628f, -.267244f, -39.2084f,
                                 .261002f, -.257694f, -.930307f, -99.7282f},
                                .974278629f, 1.73205078f, .0250000004f};
    edpe::CameraProjection now = prev;
    now.worldFromView[3] += 2.5f;
    now.worldFromView[7] -= 1.25f;

    edpe::FgCameraMatrices mats{};
    if (!edpe::computeFgCameraMatrices(now, prev, &mats)) {
        return fail("computeFgCameraMatrices failed");
    }

    const float expectedFov = 2.0f * std::atan(1.0f / now.scaleY);
    if (std::fabs(mats.verticalFovRad - expectedFov) > 1e-5f) {
        return fail("verticalFovRad mismatch");
    }

    // Reproject pixel (800, 500) at rawDepth = 0.0025 via clipToPrevClip and compare to cameraDepthMotion().
    const unsigned W = 2560, H = 1440;
    const float px = 800.0f, py = 500.0f, rawDepth = 0.0025f;
    float refMx = 0.0f, refMy = 0.0f;
    if (!edpe::cameraDepthMotion(now, prev, W, H, px, py, rawDepth, &refMx, &refMy)) {
        return fail("cameraDepthMotion failed");
    }

    const float ndcX = 2.0f * (px + 0.5f) / W - 1.0f;
    const float ndcY = 1.0f - 2.0f * (py + 0.5f) / H;
    const float* m = mats.clipToPrevClip;
    const double cx = ndcX * m[0] + ndcY * m[4] + rawDepth * m[8] + m[12];
    const double cy = ndcX * m[1] + ndcY * m[5] + rawDepth * m[9] + m[13];
    const double cw = ndcX * m[3] + ndcY * m[7] + rawDepth * m[11] + m[15];
    if (cw <= 0.0) return fail("clipToPrevClip produced non-positive W");

    const float prevNdcX = static_cast<float>(cx / cw);
    const float prevNdcY = static_cast<float>(cy / cw);
    const float matMx = (prevNdcX + 1.0f) * W * 0.5f - 0.5f - px;
    const float matMy = (1.0f - prevNdcY) * H * 0.5f - 0.5f - py;

    if (std::fabs(matMx - refMx) > 1e-2f || std::fabs(matMy - refMy) > 1e-2f) {
        std::fprintf(stderr, "clipToPrevClip (%f,%f) != cameraDepthMotion (%f,%f)\n",
                     matMx, matMy, refMx, refMy);
        return fail("clipToPrevClip reprojection mismatch");
    }
    return 0;
}

int testManagerContracts() {
    edpe::FrameGenerationManager mgr;
    if (mgr.selectedBackend() != edpe::FrameGenerationBackend::Off) {
        return fail("Initial FG backend must be Off");
    }

    // Default un-staged backends must fail-open cleanly.
    if (mgr.selectBackend(edpe::FrameGenerationBackend::AmdFsrFg, nullptr)) {
        return fail("Unregistered/unavailable FSR FG should not activate");
    }
    if (mgr.selectedBackend() != edpe::FrameGenerationBackend::Off) {
        return fail("Failed selectBackend should revert to Off");
    }

    auto mock = std::make_unique<MockFgBackend>();
    MockFgBackend* mockPtr = mock.get();
    mgr.registerBackend(std::move(mock));

    // Selecting with null presenter must fail-open cleanly.
    if (mgr.selectBackend(edpe::FrameGenerationBackend::AmdFsrFg, nullptr)) {
        return fail("selectBackend with null presenter must fail");
    }

    // Non-null dummy presenter pointer test for manager logic without creating a swapchain:
    edpe::D3D12ProxyPresenter dummyPresenter;
    if (!mgr.selectBackend(edpe::FrameGenerationBackend::AmdFsrFg, &dummyPresenter)) {
        return fail("selectBackend with mock backend failed");
    }

    // Invalid inputs (null resources) fail open.
    edpe::FrameGenerationInputs inputs{};
    inputs.renderWidth = 1706;
    inputs.renderHeight = 960;
    inputs.outputWidth = 2560;
    inputs.outputHeight = 1440;
    inputs.frameTimeMs = 10.0f;
    inputs.frameIndex = 1;
    inputs.cameraNow = {{1,0,0,0, 0,1,0,0, 0,0,1,0}, 1.0f, 1.0f, 0.025f};
    inputs.cameraPrevious = inputs.cameraNow;
    if (mgr.evaluate(nullptr, inputs, true)) {
        return fail("evaluate with null D3D12 resources must fail-open");
    }

    // Non-null sentinel pointers validate monotonic frameIndex + Rendered vs Presented FPS.
    auto* fakeRes = reinterpret_cast<ID3D12Resource*>(0x1000);
    inputs.hudlessColor = fakeRes;
    inputs.depth = fakeRes;
    inputs.motionVectors = fakeRes;

    // Frame 1 -> forces reset=true on first frame.
    if (!mgr.evaluate(nullptr, inputs, true) || !mockPtr->lastReset) {
        return fail("First FG frame must force reset=true");
    }
    if (std::fabs(mgr.diagnostics().renderedFps - 100.0f) > 0.1f ||
        std::fabs(mgr.diagnostics().presentedFps - 200.0f) > 0.1f ||
        mgr.diagnostics().generatedFramesThisCycle != 1) {
        return fail("Rendered vs Presented FPS mismatch on 2x FG");
    }

    // Frame 2 (monotonic +1) -> reset=false.
    inputs.frameIndex = 2;
    if (!mgr.evaluate(nullptr, inputs, true) || mockPtr->lastReset) {
        return fail("Monotonic frameIndex +1 should keep reset=false");
    }

    // Debug view (interp-only) at 2x: 1 generated, 1 presented (no real frame).
    mockPtr->setDebugView(true);
    inputs.frameIndex = 3;
    if (!mgr.evaluate(nullptr, inputs, true) ||
        mgr.diagnostics().generatedFramesThisCycle != 1 ||
        mgr.diagnostics().presentedFramesThisCycle != 1 ||
        std::fabs(mgr.diagnostics().presentedFps - 100.0f) > 0.1f ||
        std::wcsstr(mgr.diagnostics().statusMessage, L"interp-only") == nullptr) {
        return fail("2x interp-only debug view telemetry mismatch");
    }

    // Debug view (interp-only) at 3x: 2 generated, 2 presented.
    mockPtr->maxGen = 2;
    mgr.setRequestedExtraFrames(2);
    inputs.frameIndex = 4;
    if (!mgr.evaluate(nullptr, inputs, true) ||
        mgr.diagnostics().generatedFramesThisCycle != 2 ||
        mgr.diagnostics().presentedFramesThisCycle != 2 ||
        std::fabs(mgr.diagnostics().presentedFps - 200.0f) > 0.1f) {
        return fail("3x interp-only debug view telemetry mismatch");
    }

    // Disable debug view -> restores 3x (1 real + 2 generated).
    mockPtr->setDebugView(false);
    inputs.frameIndex = 5;
    if (!mgr.evaluate(nullptr, inputs, true) ||
        mgr.diagnostics().generatedFramesThisCycle != 2 ||
        mgr.diagnostics().presentedFramesThisCycle != 3 ||
        std::fabs(mgr.diagnostics().presentedFps - 300.0f) > 0.1f) {
        return fail("Restored 3x telemetry mismatch after disabling debug view");
    }
    mockPtr->maxGen = 1;
    mgr.setRequestedExtraFrames(1);

    // Frame 8 (skipped!) -> must auto-set reset=true.
    inputs.frameIndex = 8;
    if (!mgr.evaluate(nullptr, inputs, true) || !mockPtr->lastReset) {
        return fail("Skipped frameIndex must auto-set reset=true");
    }

    // Pause menu -> stand down without failing.
    inputs.isPauseOrMenu = true;
    inputs.frameIndex = 9;
    if (!mgr.evaluate(nullptr, inputs, true) || mgr.diagnostics().generatedFramesThisCycle != 0) {
        return fail("Pause menu should stand down FG cleanly");
    }
    inputs.isPauseOrMenu = false;

    // Backend failure -> fail-open (0 generated frames, false).
    mockPtr->shouldFail = true;
    inputs.frameIndex = 10;
    if (mgr.evaluate(nullptr, inputs, true) || mgr.diagnostics().generatedFramesThisCycle != 0) {
        return fail("Backend failure must fail-open");
    }

    mgr.shutdown();
    if (!mockPtr->shutdownCalled) return fail("shutdown() did not shut down backend");
    return 0;
}

int testFgUiPackGpu() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, &context))) {
        return fail("D3D11CreateDevice(WARP) failed");
    }

    edpe::FgUiPackPass packPass;
    if (!packPass.initialize(device.Get(), context.Get())) {
        return fail("FgUiPackPass::initialize failed");
    }

    // 4x1 test row:
    // px0: background (hdr=(0,0,0,1), ldr=(0.01,0.01,0.01,1), mult=(1,1,1)) -> (0,0,0,0)
    // px1: opaque HUD (hdr=(1,0.5,0,0), ldr=(1,0.5,0,1), mult=(1,1,1)) -> rgb~=(1,0.5,0), a=1.0
    // px2: dark mult panel (hdr=(0,0,0,1), ldr=(0.01,0.01,0.01,1), mult=(0.25,0.25,0.25)) -> rgb~=0, a~=0.75
    // px3: corner blackFloor reference (hdr=(0,0,0,1), ldr=(0.01,0.01,0.01,1), mult=(1,1,1)) -> (0,0,0,0)
    constexpr uint32_t W = 4, H = 1;
    auto makeHalf = [](float f) -> uint16_t {
        return DirectX::PackedVector::XMConvertFloatToHalf(f);
    };

    std::array<uint16_t, W * H * 4> hdrData{
        makeHalf(0.0f), makeHalf(0.0f), makeHalf(0.0f), makeHalf(1.0f),
        makeHalf(1.0f), makeHalf(0.5f), makeHalf(0.0f), makeHalf(0.0f),
        makeHalf(0.0f), makeHalf(0.0f), makeHalf(0.0f), makeHalf(1.0f),
        makeHalf(0.0f), makeHalf(0.0f), makeHalf(0.0f), makeHalf(1.0f),
    };
    auto rgba8 = [](float r, float g, float b, float a) -> uint32_t {
        auto u = [](float v) -> uint32_t {
            return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        };
        return u(r) | (u(g) << 8) | (u(b) << 16) | (u(a) << 24);
    };
    std::array<uint32_t, W * H> ldrData{
        rgba8(0.01f, 0.01f, 0.01f, 1.0f),
        rgba8(1.0f, 0.5f, 0.0f, 1.0f),
        rgba8(0.01f, 0.01f, 0.01f, 1.0f),
        rgba8(0.01f, 0.01f, 0.01f, 1.0f),
    };
    std::array<uint32_t, W * H> multData{
        rgba8(1.0f, 1.0f, 1.0f, 1.0f),
        rgba8(1.0f, 1.0f, 1.0f, 1.0f),
        rgba8(0.25f, 0.25f, 0.25f, 1.0f),
        rgba8(1.0f, 1.0f, 1.0f, 1.0f),
    };

    auto makeSrv = [&](DXGI_FORMAT fmt, const void* init, UINT pitch,
                       ComPtr<ID3D11ShaderResourceView>& outSrv) -> bool {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = W;
        td.Height = H;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA srd{init, pitch, 0};
        ComPtr<ID3D11Texture2D> tex;
        return SUCCEEDED(device->CreateTexture2D(&td, &srd, &tex)) &&
               SUCCEEDED(device->CreateShaderResourceView(tex.Get(), nullptr, &outSrv));
    };

    ComPtr<ID3D11ShaderResourceView> hdrSrv, ldrSrv, multSrv;
    if (!makeSrv(DXGI_FORMAT_R16G16B16A16_FLOAT, hdrData.data(), W * 8, hdrSrv) ||
        !makeSrv(DXGI_FORMAT_R8G8B8A8_UNORM, ldrData.data(), W * 4, ldrSrv) ||
        !makeSrv(DXGI_FORMAT_R8G8B8A8_UNORM, multData.data(), W * 4, multSrv)) {
        return fail("Failed to create FgUiPackPass input SRVs");
    }

    if (!packPass.pack(hdrSrv.Get(), ldrSrv.Get(), multSrv.Get(), true, W, H) ||
        !packPass.outputTexture()) {
        return fail("FgUiPackPass::pack failed");
    }

    D3D11_TEXTURE2D_DESC stDesc{};
    stDesc.Width = W;
    stDesc.Height = H;
    stDesc.MipLevels = 1;
    stDesc.ArraySize = 1;
    stDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    stDesc.SampleDesc.Count = 1;
    stDesc.Usage = D3D11_USAGE_STAGING;
    stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stDesc, nullptr, &staging))) {
        return fail("Failed to create staging readback");
    }
    context->CopyResource(staging.Get(), packPass.outputTexture());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return fail("Failed to map staging texture");
    }
    const auto* px = static_cast<const uint8_t*>(mapped.pData);
    // px0: untouched -> (0,0,0,0)
    const bool ok0 = (px[0] == 0 && px[1] == 0 && px[2] == 0 && px[3] == 0);
    // px1: opaque HUD -> R=255, G~128, B=0, A=255
    const bool ok1 = (px[4] == 255 && std::abs(int(px[5]) - 128) <= 2 && px[6] == 0 && px[7] == 255);
    // px2: dark mult 0.25 -> RGB=0, A~191 (1 - 0.25)
    const bool ok2 = (px[8] == 0 && px[9] == 0 && px[10] == 0 && std::abs(int(px[11]) - 191) <= 2);
    context->Unmap(staging.Get(), 0);

    if (!ok0 || !ok1 || !ok2) {
        std::fprintf(stderr, "Packed UI mismatch: px0=(%u,%u,%u,%u) px1=(%u,%u,%u,%u) px2=(%u,%u,%u,%u)\n",
                     px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7], px[8], px[9], px[10], px[11]);
        return fail("FgUiPackPass pixel output mismatch");
    }

    packPass.shutdown();
    return 0;
}

} // namespace

int main() {
    if (int rc = testCameraMatricesAgainstMotion(); rc != 0) return rc;
    if (int rc = testManagerContracts(); rc != 0) return rc;
    if (int rc = testFgUiPackGpu(); rc != 0) return rc;
    std::printf("fg_backend_test PASSED (camera math, manager contracts, FgUiPackPass GPU).\n");
    return 0;
}
