// FSR 4 engine tests. Needs a D3D11 device and, to do anything, the signed FidelityFX DLLs staged beside the
// executable (amd_fidelityfx_loader_dx12.dll, amd_fidelityfx_upscaler_dx12.dll). When the engine cannot initialize
// or the GPU/driver does not offer FSR 4 the whole test SKIPs (exit 0), so it is safe on machines without them.
//
// Feeds EDPE-style inputs: 8-bit typeless color, typeless depth/stencil with a float SRV, R16G16 motion vectors and
// an 8-bit destination. One table of cases:
//   basic            flat color survives the D3D12 bridge
//   provider         the context runs a 4.x provider (never a 3.x one under the FSR 4 name)
//   resize           render/output size changes mid-run (context and shared textures are recreated)
//   frame-order      a different flat color every frame with a history reset: the output must be the current
//                    frame's color, across more frames than the 4-slot ring (fence and slot reuse ordering)
//   depth-r24g8      the second depth format seen in the game log works or fails cleanly
//   reinit           shutdown and initialize three times with frames in between
//   fail-open        invalid inputs return false with an error and do not break the next frame
//   hdr-refused      HDR color input is refused with an explanation (FSR 4 does not support it yet)
//   agility-fallback the bridge falls back to the system D3D12 runtime when the Agility SDK directory is bad
//   agility-staged   the bridge uses the staged Agility SDK runtime (SKIP when it is not staged)
#include "amd_fsr4_engine.h"
#include "d3d12_bridge.h"
#include "log.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace edpe;

namespace {

enum class Result { Pass, Skip, Fail };

struct CaseOutcome {
    Result result = Result::Pass;
    std::string note;
};

CaseOutcome pass() { return {Result::Pass, ""}; }
CaseOutcome skip(std::string why) { return {Result::Skip, std::move(why)}; }
CaseOutcome fail(std::string why) { return {Result::Fail, std::move(why)}; }

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += (c < 128) ? static_cast<char>(c) : '?';
    return s;
}

float halton(int index, int base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) { f /= static_cast<float>(base); r += f * static_cast<float>(index % base); index /= base; }
    return r;
}

struct Rgb { int r, g, b; };

bool closeTo(const Rgb& got, const Rgb& want, int tol) {
    return std::abs(got.r - want.r) <= tol && std::abs(got.g - want.g) <= tol && std::abs(got.b - want.b) <= tol;
}

// One set of EDPE-style input textures and the destination for a given render/output size.
struct Frame {
    UINT rw = 0, rh = 0, ow = 0, oh = 0;
    DXGI_FORMAT depthTex = DXGI_FORMAT_R32G8X24_TYPELESS;
    DXGI_FORMAT dsvFmt = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    DXGI_FORMAT srvFmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    ComPtr<ID3D11Texture2D> color, depth, motion, dest;
    ComPtr<ID3D11RenderTargetView> colorRtv, motionRtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> depthSrv;
    bool valid = false;
};

struct Env {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    Fsr4Engine engine;

    ComPtr<ID3D11Texture2D> makeTex(UINT w, UINT h, DXGI_FORMAT fmt, UINT bind) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
        ComPtr<ID3D11Texture2D> t;
        return SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t)) ? t : ComPtr<ID3D11Texture2D>();
    }

    bool makeFrame(Frame& f, UINT rw, UINT rh, UINT ow, UINT oh, bool depthR24 = false) {
        f = Frame{};
        f.rw = rw; f.rh = rh; f.ow = ow; f.oh = oh;
        if (depthR24) {
            f.depthTex = DXGI_FORMAT_R24G8_TYPELESS;
            f.dsvFmt = DXGI_FORMAT_D24_UNORM_S8_UINT;
            f.srvFmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        }
        f.color = makeTex(rw, rh, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        f.depth = makeTex(rw, rh, f.depthTex, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
        f.motion = makeTex(rw, rh, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        f.dest = makeTex(ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        if (!f.color || !f.depth || !f.motion || !f.dest) return false;

        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateRenderTargetView(f.color.Get(), &rtvDesc, &f.colorRtv))) return false;
        if (FAILED(dev->CreateRenderTargetView(f.motion.Get(), nullptr, &f.motionRtv))) return false;
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = f.dsvFmt;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateDepthStencilView(f.depth.Get(), &dd, &f.dsv))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = f.srvFmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(f.depth.Get(), &sd, &f.depthSrv))) return false;
        f.valid = true;
        return true;
    }

    TemporalFrameInputs inputs(const Frame& f, int frameIndex, bool reset) const {
        TemporalFrameInputs in{};
        in.color = f.color.Get();
        in.depth = f.depth.Get();
        in.depthSrv = f.depthSrv.Get();
        in.motionVectors = f.motion.Get();
        in.renderWidth = f.rw; in.renderHeight = f.rh; in.outputWidth = f.ow; in.outputHeight = f.oh;
        in.frameTimeMs = 16.6f;
        in.colorIsHdr = false;
        in.depthInverted = true;
        in.depthInfinite = true;
        in.cameraNow.scaleY = 1.7320508f;
        in.cameraNow.depthB = 0.025f;
        in.jitterX = halton((frameIndex % 8) + 1, 2) - 0.5f;
        in.jitterY = halton((frameIndex % 8) + 1, 3) - 0.5f;
        in.reset = reset;
        return in;
    }

    // Clears the inputs to a flat color, evaluates, returns the engine result.
    bool runFrame(const Frame& f, const float clr[4], int frameIndex, bool reset) {
        const float zero[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(f.colorRtv.Get(), clr);
        ctx->ClearRenderTargetView(f.motionRtv.Get(), zero);
        ctx->ClearDepthStencilView(f.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.25f, 0);
        return engine.evaluate(ctx.Get(), inputs(f, frameIndex, reset), f.dest.Get());
    }

    bool centerPixel(const Frame& f, Rgb& out) {
        D3D11_TEXTURE2D_DESC sd{};
        f.dest->GetDesc(&sd);
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) return false;
        ctx->CopyResource(staging.Get(), f.dest.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        const unsigned char* px = static_cast<const unsigned char*>(m.pData) + m.RowPitch * (f.oh / 2) + 4 * (f.ow / 2);
        out = {px[0], px[1], px[2]};
        ctx->Unmap(staging.Get(), 0);
        return true;
    }

    // Runs `count` frames of one flat color (reset on the first). Returns an empty string on success.
    std::string runFlat(const Frame& f, const float clr[4], const Rgb& want, int count, int tol = 10) {
        for (int i = 0; i < count; ++i) {
            if (!runFrame(f, clr, i, i == 0)) return "evaluate returned false: " + narrow(engine.lastError());
        }
        Rgb got{};
        if (!centerPixel(f, got)) return "center pixel readback failed";
        if (!closeTo(got, want, tol)) {
            char b[160];
            std::snprintf(b, sizeof(b), "center pixel %d %d %d, expected about %d %d %d", got.r, got.g, got.b, want.r,
                          want.g, want.b);
            return b;
        }
        return "";
    }
};

constexpr float kFlat[4] = {0.3f, 0.4f, 0.5f, 1.0f};  // about 77 102 128 after the round trip
const Rgb kFlatRgb{77, 102, 128};

// ------------------------------------------------------------------------------------------------------------

CaseOutcome caseBasic(Env& e) {
    Frame f;
    if (!e.makeFrame(f, 960, 540, 1920, 1080)) return fail("test texture creation");
    const std::string err = e.runFlat(f, kFlat, kFlatRgb, 40);
    if (!err.empty()) return fail(err);

    std::printf("      provider %s\n", e.engine.providerVersion());
    return pass();
}

CaseOutcome caseProvider(Env& e) {
    Frame f;
    if (!e.makeFrame(f, 960, 540, 1920, 1080)) return fail("test texture creation");
    const std::string err = e.runFlat(f, kFlat, kFlatRgb, 4);  // makes the context, which reports its provider
    if (!err.empty()) return fail(err);
    const std::string version = e.engine.providerVersion();
    if (version.empty() || version[0] < '4' || version[0] > '9') {
        return fail("the context reports provider '" + version + "', not a 4.x provider");
    }
    std::printf("      provider %s\n", version.c_str());
    return pass();
}

CaseOutcome caseResize(Env& e) {
    struct Size { UINT rw, rh, ow, oh; };
    const Size sizes[] = {{960, 540, 1920, 1080}, {1280, 720, 1920, 1080}, {1280, 720, 2560, 1440},
                          {960, 540, 1920, 1080}};
    for (const Size& s : sizes) {
        Frame f;
        if (!e.makeFrame(f, s.rw, s.rh, s.ow, s.oh)) return fail("test texture creation");
        const std::string err = e.runFlat(f, kFlat, kFlatRgb, 12);
        if (!err.empty()) {
            return fail(std::to_string(s.rw) + "x" + std::to_string(s.rh) + " -> " + std::to_string(s.ow) + "x" +
                        std::to_string(s.oh) + ": " + err);
        }
    }
    return pass();
}

CaseOutcome caseFrameOrder(Env& e) {
    Frame f;
    if (!e.makeFrame(f, 960, 540, 1920, 1080)) return fail("test texture creation");
    const float colors[2][4] = {{0.8f, 0.2f, 0.1f, 1.0f}, {0.1f, 0.3f, 0.9f, 1.0f}};
    const Rgb wants[2] = {{204, 51, 26}, {26, 77, 230}};
    // 24 frames is six full turns of the 4-slot ring; every frame is a history reset with a different color.
    for (int i = 0; i < 24; ++i) {
        const int c = i & 1;
        if (!e.runFrame(f, colors[c], i, true)) return fail("evaluate returned false: " + narrow(e.engine.lastError()));
        Rgb got{};
        if (!e.centerPixel(f, got)) return fail("center pixel readback failed");
        if (!closeTo(got, wants[c], 16)) {
            char b[200];
            std::snprintf(b, sizeof(b), "frame %d: output %d %d %d does not match this frame's color %d %d %d", i,
                          got.r, got.g, got.b, wants[c].r, wants[c].g, wants[c].b);
            return fail(b);
        }
    }
    return pass();
}

CaseOutcome caseDepthR24(Env& e) {
    Frame f;
    if (!e.makeFrame(f, 960, 540, 1920, 1080, /*depthR24=*/true)) return skip("R24G8 depth texture or views not supported");
    bool anyOk = false;
    for (int i = 0; i < 10; ++i) {
        if (!e.runFrame(f, kFlat, i, i == 0)) {
            // A clean failure is acceptable (the caller falls back to FSR 3.1) as long as it explains itself.
            const std::wstring err = e.engine.lastError();
            if (err.empty()) return fail("evaluate failed without an error message");
            std::printf("      R24G8 depth rejected cleanly: %s\n", narrow(err).c_str());
            return pass();
        }
        anyOk = true;
    }
    Rgb got{};
    if (!anyOk || !e.centerPixel(f, got)) return fail("no result");
    if (!closeTo(got, kFlatRgb, 10)) return fail("color wrong with R24G8 depth");
    return pass();
}

CaseOutcome caseReinit(Env& e) {
    Frame f;
    if (!e.makeFrame(f, 960, 540, 1920, 1080)) return fail("test texture creation");
    for (int cycle = 0; cycle < 3; ++cycle) {
        e.engine.shutdown();
        if (!e.engine.initialize(e.dev.Get())) {
            return fail("initialize failed on cycle " + std::to_string(cycle) + ": " + narrow(e.engine.lastError()));
        }
        const std::string err = e.runFlat(f, kFlat, kFlatRgb, 8);
        if (!err.empty()) return fail("cycle " + std::to_string(cycle) + ": " + err);
    }
    return pass();
}

CaseOutcome caseFailOpen(Env& e) {
    Frame f;
    if (!e.makeFrame(f, 960, 540, 1920, 1080)) return fail("test texture creation");
    // Warm up with good frames.
    std::string err = e.runFlat(f, kFlat, kFlatRgb, 6);
    if (!err.empty()) return fail(err);

    // 1. Missing destination.
    if (e.engine.evaluate(e.ctx.Get(), e.inputs(f, 0, false), nullptr)) return fail("null destination was accepted");
    if (std::wstring(e.engine.lastError()).empty()) return fail("null destination gave no error message");

    // 2. Destination whose size does not match the reported output size.
    Frame wrong;
    if (!e.makeFrame(wrong, 960, 540, 1280, 720)) return fail("test texture creation");
    if (e.engine.evaluate(e.ctx.Get(), e.inputs(f, 0, false), wrong.dest.Get())) {
        return fail("destination with the wrong size was accepted");
    }

    // 3. Missing depth view.
    TemporalFrameInputs noDepth = e.inputs(f, 0, false);
    noDepth.depthSrv = nullptr;
    noDepth.depth = nullptr;
    if (e.engine.evaluate(e.ctx.Get(), noDepth, f.dest.Get())) return fail("missing depth was accepted");

    // The engine must still work on the next valid frame.
    err = e.runFlat(f, kFlat, kFlatRgb, 6);
    if (!err.empty()) return fail("engine did not recover after rejected inputs: " + err);
    return pass();
}

CaseOutcome caseHdrRefused(Env& e) {
    Frame f;
    if (!e.makeFrame(f, 960, 540, 1920, 1080)) return fail("test texture creation");
    std::string err = e.runFlat(f, kFlat, kFlatRgb, 6);
    if (!err.empty()) return fail(err);

    TemporalFrameInputs hdr = e.inputs(f, 0, false);
    hdr.colorIsHdr = true;
    if (e.engine.evaluate(e.ctx.Get(), hdr, f.dest.Get())) return fail("HDR color input was accepted");
    const std::string why = narrow(e.engine.lastError());
    if (why.find("HDR") == std::string::npos) return fail("refusal does not mention HDR: " + why);

    err = e.runFlat(f, kFlat, kFlatRgb, 6);
    if (!err.empty()) return fail("engine did not recover after the HDR refusal: " + err);
    return pass();
}

CaseOutcome caseAgilityFallback(Env& e) {
    D3D11D3D12Bridge bridge;
    // A directory without D3D12Core.dll: the bridge must fall back to the system runtime and still work.
    const std::wstring bad = (std::filesystem::temp_directory_path() / L"edpe_no_agility_sdk_here").wstring();
    if (!bridge.initialize(e.dev.Get(), bad.c_str())) {
        return fail("bridge did not fall back to the system D3D12 runtime");
    }
    // usingAgilitySdk() is not asserted: a system runtime that already provides the requested SDK version can
    // satisfy CreateDeviceFactory without the file (seen on a CI runner), so initialization succeeding is the check.
    bridge.shutdown();
    return pass();
}

CaseOutcome caseAgilityStaged(Env& e) {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::filesystem::path dir = std::filesystem::path(exePath).parent_path() / L"D3D12";
    std::error_code ec;
    if (!std::filesystem::exists(dir / L"D3D12Core.dll", ec)) return skip("D3D12\\D3D12Core.dll is not staged beside the test");
    D3D11D3D12Bridge bridge;
    const std::wstring d = dir.wstring() + L"\\";
    if (!bridge.initialize(e.dev.Get(), d.c_str())) return fail("bridge initialization with the staged Agility SDK failed");
    const bool agility = bridge.usingAgilitySdk();
    bridge.shutdown();
    if (!agility) return skip("the system runtime does not accept the staged Agility SDK version");
    return pass();
}

} // namespace

int main() {
    Env e;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &e.dev,
                                 nullptr, &e.ctx))) {
        std::printf("SKIP: no hardware D3D11 device\n");
        return 0;
    }
    if (!e.engine.initialize(e.dev.Get())) {
        std::wprintf(L"SKIP: FSR 4 engine not available: %ls\n", e.engine.lastError());
        return 0;
    }

    // Probe once: a GPU or driver that does not offer FSR 4 fails context creation on the first frame.
    {
        Frame probe;
        if (!e.makeFrame(probe, 960, 540, 1920, 1080)) { std::printf("FAIL: test texture creation\n"); return 1; }
        if (!e.runFrame(probe, kFlat, 0, true)) {
            const std::wstring err = e.engine.lastError();
            if (err.find(L"ffxCreateContext") != std::wstring::npos) {
                std::wprintf(L"SKIP: this GPU/driver does not offer FSR 4: %ls\n", err.c_str());
                return 0;
            }
            std::wprintf(L"FAIL: first evaluate failed: %ls\n", err.c_str());
            return 1;
        }
    }

    struct Case { const char* name; std::function<CaseOutcome(Env&)> run; };
    const Case cases[] = {
        {"basic", caseBasic},
        {"provider", caseProvider},
        {"resize", caseResize},
        {"frame-order", caseFrameOrder},
        {"depth-r24g8", caseDepthR24},
        {"reinit", caseReinit},
        {"fail-open", caseFailOpen},
        {"hdr-refused", caseHdrRefused},
        {"agility-fallback", caseAgilityFallback},
        {"agility-staged", caseAgilityStaged},
    };

    int failures = 0, passes = 0, skips = 0;
    for (const Case& c : cases) {
        const CaseOutcome o = c.run(e);
        const char* tag = o.result == Result::Pass ? "PASS" : (o.result == Result::Skip ? "SKIP" : "FAIL");
        std::printf("[%s] %s%s%s\n", tag, c.name, o.note.empty() ? "" : ": ", o.note.c_str());
        if (o.result == Result::Pass) ++passes;
        else if (o.result == Result::Skip) ++skips;
        else ++failures;
    }
    std::printf("%d passed, %d skipped, %d failed\n", passes, skips, failures);
    if (failures) return 1;
    std::printf("PASS\n");
    return 0;
}
