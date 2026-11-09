#include "ui_composite_pass.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

using Microsoft::WRL::ComPtr;

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &context))) {
        return 1;
    }

    edpe::UiCompositePass pass;
    if (!pass.initialize(device.Get(), context.Get())) {
        return 2;
    }

    // Create 4x4 output backbuffer and RTV
    D3D11_TEXTURE2D_DESC outDesc{};
    outDesc.Width = outDesc.Height = 4;
    outDesc.MipLevels = outDesc.ArraySize = 1;
    outDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    outDesc.SampleDesc.Count = 1;
    outDesc.Usage = D3D11_USAGE_DEFAULT;
    outDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> targetTex, dlssTex, gameTex, lowTex;
    ComPtr<ID3D11RenderTargetView> targetRtv;
    ComPtr<ID3D11ShaderResourceView> dlssSrv, gameSrv, lowSrv;

    // dlss: pure green (0, 255, 0, 255)
    std::array<uint32_t, 16> dlssPixels{};
    dlssPixels.fill(0xFF00FF00); // 0xAABBGGRR -> RGBA(0, 255, 0, 255)
    D3D11_SUBRESOURCE_DATA dlssInit{dlssPixels.data(), 4 * sizeof(uint32_t), 0};

    // game: pure green for top 2 rows, pure white for bottom 2 rows (UI)
    std::array<uint32_t, 16> gamePixels{};
    for (int i = 0; i < 8; ++i) gamePixels[i] = 0xFF00FF00;
    for (int i = 8; i < 16; ++i) gamePixels[i] = 0xFFFFFFFF;
    D3D11_SUBRESOURCE_DATA gameInit{gamePixels.data(), 4 * sizeof(uint32_t), 0};

    // low: 2x2 pure green
    D3D11_TEXTURE2D_DESC lowDesc = outDesc;
    lowDesc.Width = lowDesc.Height = 2;
    std::array<uint32_t, 4> lowPixels{};
    lowPixels.fill(0xFF00FF00);
    D3D11_SUBRESOURCE_DATA lowInit{lowPixels.data(), 2 * sizeof(uint32_t), 0};

    if (FAILED(device->CreateTexture2D(&outDesc, nullptr, &targetTex)) ||
        FAILED(device->CreateRenderTargetView(targetTex.Get(), nullptr, &targetRtv)) ||
        FAILED(device->CreateTexture2D(&outDesc, &dlssInit, &dlssTex)) ||
        FAILED(device->CreateShaderResourceView(dlssTex.Get(), nullptr, &dlssSrv)) ||
        FAILED(device->CreateTexture2D(&outDesc, &gameInit, &gameTex)) ||
        FAILED(device->CreateShaderResourceView(gameTex.Get(), nullptr, &gameSrv)) ||
        FAILED(device->CreateTexture2D(&lowDesc, &lowInit, &lowTex)) ||
        FAILED(device->CreateShaderResourceView(lowTex.Get(), nullptr, &lowSrv))) {
        return 3;
    }

    const D3D11_VIEWPORT prior_viewport{0.0f, 0.0f, 2.0f, 2.0f, 0.0f, 1.0f};
    context->RSSetViewports(1, &prior_viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

    bool ok = pass.composite(context.Get(), dlssSrv.Get(), gameSrv.Get(), lowSrv.Get(), nullptr,
        targetRtv.Get(), 4, 4, 2, 2);
    if (!ok) return 4;

    // Verify context state restoration
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    UINT viewport_count = 1;
    D3D11_VIEWPORT viewport{};
    context->IAGetPrimitiveTopology(&topology);
    context->RSGetViewports(&viewport_count, &viewport);
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_LINELIST ||
        viewport_count != 1 || viewport.Width != 2.0f) {
        return 5;
    }

    // Read back target texture
    D3D11_TEXTURE2D_DESC readDesc = outDesc;
    readDesc.Usage = D3D11_USAGE_STAGING;
    readDesc.BindFlags = 0;
    readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&readDesc, nullptr, &staging))) return 6;

    context->CopyResource(staging.Get(), targetTex.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 7;

    const auto* rows = static_cast<const uint8_t*>(mapped.pData);
    // Row 0 (world): should be green (R ~ 0, G ~ 255, B ~ 0)
    const uint8_t r0 = rows[0], g0 = rows[1], b0 = rows[2];
    // Row 3 (UI): should be white (R ~ 255, G ~ 255, B ~ 255)
    const auto* lastRow = rows + 3 * mapped.RowPitch;
    const uint8_t r3 = lastRow[0], g3 = lastRow[1], b3 = lastRow[2];
    context->Unmap(staging.Get(), 0);

    if (g0 < 200 || r0 > 50 || b0 > 50) return 8; // world pixel mismatch
    if (r3 < 200 || g3 < 200 || b3 < 200) return 9; // UI pixel mismatch

    // Test 2: Native resolution composite (renderSize == outputSize == 4x4)
    // Low-res world is 4x4 pure green (dlssSrv), game backbuffer has UI on bottom rows
    ok = pass.composite(context.Get(), dlssSrv.Get(), gameSrv.Get(), dlssSrv.Get(), nullptr,
        targetRtv.Get(), 4, 4, 4, 4);
    if (!ok) return 10;

    context->CopyResource(staging.Get(), targetTex.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 11;

    rows = static_cast<const uint8_t*>(mapped.pData);
    const uint8_t nr0 = rows[0], ng0 = rows[1], nb0 = rows[2];
    lastRow = rows + 3 * mapped.RowPitch;
    const uint8_t nr3 = lastRow[0], ng3 = lastRow[1], nb3 = lastRow[2];
    context->Unmap(staging.Get(), 0);

    if (ng0 < 200 || nr0 > 50 || nb0 > 50) return 12; // native world pixel mismatch
    if (nr3 < 200 || ng3 < 200 || nb3 < 200) return 13; // native UI pixel mismatch

    // Test 3: HDR native resolution composite (isHdr = true)
    ok = pass.composite(context.Get(), dlssSrv.Get(), gameSrv.Get(), dlssSrv.Get(), nullptr,
        targetRtv.Get(), 4, 4, 4, 4, true);
    if (!ok) return 14;

    context->CopyResource(staging.Get(), targetTex.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 15;

    rows = static_cast<const uint8_t*>(mapped.pData);
    const uint8_t hr0 = rows[0], hg0 = rows[1], hb0 = rows[2];
    lastRow = rows + 3 * mapped.RowPitch;
    const uint8_t hr3 = lastRow[0], hg3 = lastRow[1], hb3 = lastRow[2];
    context->Unmap(staging.Get(), 0);

    if (hg0 < 150 || hr0 > 50 || hb0 > 50) return 16; // HDR world pixel mismatch
    if (hr3 < 150 || hg3 < 150 || hb3 < 150) return 17; // HDR UI pixel mismatch

    // Test 4: HUD mask composite path
    // In this test, row 1 of low-res world (2x2) and rows 2-3 of game backbuffer (4x4)
    // both contain orange (255, 128, 0). Because diff is 0, normal UI diff would output DLSS (green).
    // The HUD mask is set to 255 for row 1 of the 2x2 mask (covering bottom half of output).
    // The composite pass must output cGame (orange) for row 3, and DLSS (green) for row 0.
    D3D11_TEXTURE2D_DESC maskDesc{};
    maskDesc.Width = maskDesc.Height = 2;
    maskDesc.MipLevels = maskDesc.ArraySize = 1;
    maskDesc.Format = DXGI_FORMAT_R8_UNORM;
    maskDesc.SampleDesc.Count = 1;
    maskDesc.Usage = D3D11_USAGE_DEFAULT;
    maskDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    std::array<uint8_t, 4> maskPixels = {0, 0, 255, 255}; // top row 0, bottom row 255
    D3D11_SUBRESOURCE_DATA maskInit{maskPixels.data(), 2 * sizeof(uint8_t), 0};

    // Low-res world with orange in bottom row (matches game orange, so diff is 0!)
    // 0xAABBGGRR: Orange is R=255, G=128, B=0 -> 0xFF0080FF
    std::array<uint32_t, 4> lowHudPixels = {0xFF00FF00, 0xFF00FF00, 0xFF0080FF, 0xFF0080FF};
    D3D11_SUBRESOURCE_DATA lowHudInit{lowHudPixels.data(), 2 * sizeof(uint32_t), 0};

    // Game backbuffer: top rows green, bottom rows orange
    std::array<uint32_t, 16> gameHudPixels{};
    for (int i = 0; i < 8; ++i) gameHudPixels[i] = 0xFF00FF00;
    for (int i = 8; i < 16; ++i) gameHudPixels[i] = 0xFF0080FF;
    D3D11_SUBRESOURCE_DATA gameHudInit{gameHudPixels.data(), 4 * sizeof(uint32_t), 0};

    ComPtr<ID3D11Texture2D> maskTex, lowHudTex, gameHudTex;
    ComPtr<ID3D11ShaderResourceView> maskSrv, lowHudSrv, gameHudSrv;

    if (FAILED(device->CreateTexture2D(&maskDesc, &maskInit, &maskTex)) ||
        FAILED(device->CreateShaderResourceView(maskTex.Get(), nullptr, &maskSrv)) ||
        FAILED(device->CreateTexture2D(&lowDesc, &lowHudInit, &lowHudTex)) ||
        FAILED(device->CreateShaderResourceView(lowHudTex.Get(), nullptr, &lowHudSrv)) ||
        FAILED(device->CreateTexture2D(&outDesc, &gameHudInit, &gameHudTex)) ||
        FAILED(device->CreateShaderResourceView(gameHudTex.Get(), nullptr, &gameHudSrv))) {
        return 18;
    }

    ok = pass.composite(context.Get(), dlssSrv.Get(), gameHudSrv.Get(), lowHudSrv.Get(),
        maskSrv.Get(), targetRtv.Get(), 4, 4, 2, 2);
    if (!ok) return 19;

    context->CopyResource(staging.Get(), targetTex.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 20;

    rows = static_cast<const uint8_t*>(mapped.pData);
    const uint8_t mr0 = rows[0], mg0 = rows[1], mb0 = rows[2];
    lastRow = rows + 3 * mapped.RowPitch;
    const uint8_t mr3 = lastRow[0], mg3 = lastRow[1], mb3 = lastRow[2];
    context->Unmap(staging.Get(), 0);

    // Row 0 should be DLSS green (R ~ 0, G ~ 255, B ~ 0)
    if (mg0 < 200 || mr0 > 50 || mb0 > 50) return 21; // HUD mask world pixel mismatch
    // Row 3 should be game orange (R ~ 255, G ~ 128, B ~ 0) preserved by hudMask!
    if (mr3 < 200 || mg3 < 100 || mb3 > 50) return 22; // HUD mask pixel mismatch (not preserved)

    // Test 5: HUD mask composite path at native resolution (renderSize == outputSize == 4x4)
    // Game backbuffer and low-res world both contain orange in rows 2-3 (diff == 0!).
    // DLSS reconstruction is green. Without hudMask, native path would output DLSS green.
    // With hudMask enabled for bottom half (rows 2-3), orange pixels from cGame must be preserved.
    D3D11_TEXTURE2D_DESC mask4Desc = outDesc;
    mask4Desc.Format = DXGI_FORMAT_R8_UNORM;
    std::array<uint8_t, 16> mask4Pixels{};
    for (int i = 0; i < 8; ++i) mask4Pixels[i] = 0;
    for (int i = 8; i < 16; ++i) mask4Pixels[i] = 255;
    D3D11_SUBRESOURCE_DATA mask4Init{mask4Pixels.data(), 4 * sizeof(uint8_t), 0};

    ComPtr<ID3D11Texture2D> mask4Tex;
    ComPtr<ID3D11ShaderResourceView> mask4Srv;
    if (FAILED(device->CreateTexture2D(&mask4Desc, &mask4Init, &mask4Tex)) ||
        FAILED(device->CreateShaderResourceView(mask4Tex.Get(), nullptr, &mask4Srv))) {
        return 23;
    }

    ok = pass.composite(context.Get(), dlssSrv.Get(), gameHudSrv.Get(), gameHudSrv.Get(),
        mask4Srv.Get(), targetRtv.Get(), 4, 4, 4, 4);
    if (!ok) return 24;

    context->CopyResource(staging.Get(), targetTex.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 25;

    rows = static_cast<const uint8_t*>(mapped.pData);
    const uint8_t nmr0 = rows[0], nmg0 = rows[1], nmb0 = rows[2];
    lastRow = rows + 3 * mapped.RowPitch;
    const uint8_t nmr3 = lastRow[0], nmg3 = lastRow[1], nmb3 = lastRow[2];
    context->Unmap(staging.Get(), 0);

    // Row 0: DLSS green (R ~ 0, G ~ 255, B ~ 0)
    if (nmg0 < 200 || nmr0 > 50 || nmb0 > 50) return 26; // Native HUD mask world pixel mismatch
    // Row 3: Game orange (R ~ 255, G ~ 128, B ~ 0) preserved by hudMask despite diff == 0!
    if (nmr3 < 200 || nmg3 < 100 || nmb3 > 50) return 27; // Native HUD mask pixel mismatch (not preserved)

    // Test 6a: CPU verification of HUD mask threshold & boost formula
    auto smoothstepCalc = [](float edge0, float edge1, float x) {
        float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    if (smoothstepCalc(0.005f, 0.08f, 0.001f) != 0.0f) return 28; // Noise < 0.005 cut to 0.0
    if (smoothstepCalc(0.005f, 0.08f, 0.08f) != 1.0f) return 29;  // Edge reached 1.0
    if (smoothstepCalc(0.005f, 0.08f, 0.15f) != 1.0f) return 30;  // Weak linear HDR HUD (0.15) gives 1.0

    // Test 6b: GPU composite with weak linear HDR HUD signal (hudRaw ~ 0.15)
    // Mask value 38 / 255.0 = ~0.149. Under the old saturate() formula, this would only blend
    // 15% orange and 85% green. Under smoothstep(0.005, 0.08, hudRaw), hudMask reaches 1.0,
    // so row 3 must be pure game orange (R ~ 255, G ~ 128, B ~ 0).
    std::array<uint8_t, 16> weakMaskPixels{};
    for (int i = 0; i < 8; ++i) weakMaskPixels[i] = 0;
    for (int i = 8; i < 16; ++i) weakMaskPixels[i] = 38;
    D3D11_SUBRESOURCE_DATA weakMaskInit{weakMaskPixels.data(), 4 * sizeof(uint8_t), 0};

    ComPtr<ID3D11Texture2D> weakMaskTex;
    ComPtr<ID3D11ShaderResourceView> weakMaskSrv;
    if (FAILED(device->CreateTexture2D(&mask4Desc, &weakMaskInit, &weakMaskTex)) ||
        FAILED(device->CreateShaderResourceView(weakMaskTex.Get(), nullptr, &weakMaskSrv))) {
        return 31;
    }

    ok = pass.composite(context.Get(), dlssSrv.Get(), gameHudSrv.Get(), gameHudSrv.Get(),
        weakMaskSrv.Get(), targetRtv.Get(), 4, 4, 4, 4);
    if (!ok) return 32;

    context->CopyResource(staging.Get(), targetTex.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 33;

    rows = static_cast<const uint8_t*>(mapped.pData);
    const uint8_t wmr0 = rows[0], wmg0 = rows[1], wmb0 = rows[2];
    lastRow = rows + 3 * mapped.RowPitch;
    const uint8_t wmr3 = lastRow[0], wmg3 = lastRow[1], wmb3 = lastRow[2];
    context->Unmap(staging.Get(), 0);

    // Row 0: DLSS green (R ~ 0, G ~ 255, B ~ 0)
    if (wmg0 < 200 || wmr0 > 50 || wmb0 > 50) return 34;
    // Row 3: Game orange (R ~ 255, G ~ 128, B ~ 0) fully boosted to weight 1.0 despite weak signal
    if (wmr3 < 200 || wmg3 < 100 || wmb3 > 50) return 35;

    // Test 7: preserveUi = false (LDR, nullptr game backbuffer and low-res world)
    ok = pass.composite(context.Get(), dlssSrv.Get(), nullptr, nullptr, nullptr,
        targetRtv.Get(), 4, 4, 4, 4, /*isHdr=*/false, /*preserveUi=*/false);
    if (!ok) return 36;

    context->CopyResource(staging.Get(), targetTex.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 37;

    rows = static_cast<const uint8_t*>(mapped.pData);
    const uint8_t noUiR0 = rows[0], noUiG0 = rows[1], noUiB0 = rows[2];
    lastRow = rows + 3 * mapped.RowPitch;
    const uint8_t noUiR3 = lastRow[0], noUiG3 = lastRow[1], noUiB3 = lastRow[2];
    context->Unmap(staging.Get(), 0);

    // Both row 0 and row 3 must be pure DLSS green (no UI overlay)
    if (noUiG0 < 200 || noUiR0 > 50 || noUiB0 > 50) return 38;
    if (noUiG3 < 200 || noUiR3 > 50 || noUiB3 > 50) return 39;

    // Test 8: preserveUi = false (HDR tone-mapping path)
    ok = pass.composite(context.Get(), dlssSrv.Get(), nullptr, nullptr, nullptr,
        targetRtv.Get(), 4, 4, 4, 4, /*isHdr=*/true, /*preserveUi=*/false);
    if (!ok) return 40;

    context->CopyResource(staging.Get(), targetTex.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 41;

    rows = static_cast<const uint8_t*>(mapped.pData);
    const uint8_t noUiHdrR0 = rows[0], noUiHdrG0 = rows[1], noUiHdrB0 = rows[2];
    lastRow = rows + 3 * mapped.RowPitch;
    const uint8_t noUiHdrR3 = lastRow[0], noUiHdrG3 = lastRow[1], noUiHdrB3 = lastRow[2];
    context->Unmap(staging.Get(), 0);

    // Both row 0 and row 3 must be DLSS green tone-mapped to LDR
    if (noUiHdrG0 < 150 || noUiHdrR0 > 50 || noUiHdrB0 > 50) return 42;
    if (noUiHdrG3 < 150 || noUiHdrR3 > 50 || noUiHdrB3 > 50) return 43;

    // Test 9: preserveUi = false ignores gameHudSrv and mask4Srv even if supplied
    ok = pass.composite(context.Get(), dlssSrv.Get(), gameHudSrv.Get(), gameHudSrv.Get(),
        mask4Srv.Get(), targetRtv.Get(), 4, 4, 4, 4, /*isHdr=*/false, /*preserveUi=*/false);
    if (!ok) return 44;

    context->CopyResource(staging.Get(), targetTex.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 45;

    rows = static_cast<const uint8_t*>(mapped.pData);
    lastRow = rows + 3 * mapped.RowPitch;
    const uint8_t ignoreR3 = lastRow[0], ignoreG3 = lastRow[1], ignoreB3 = lastRow[2];
    context->Unmap(staging.Get(), 0);

    // Row 3 must still be DLSS green, NOT game orange
    if (ignoreG3 < 200 || ignoreR3 > 50 || ignoreB3 > 50) return 46;

    // Test 10: preserveUi = true must fail when gameBackbufferWithUi is null
    if (pass.composite(context.Get(), dlssSrv.Get(), nullptr, lowSrv.Get(), nullptr,
            targetRtv.Get(), 4, 4, 2, 2, false, true)) {
        return 47;
    }

    // Test 11: isNativeHud path (flags.w == 1.0) with native R11G11B10_FLOAT HUD texture
    // In this test, dlssReconstruction, gameBackbuffer, and lowResWorld are all pure DLSS green.
    // gameBackbuffer has NO orange (diff == 0 everywhere).
    // nativeHudTex has orange HUD in rows 2-3 (DXGI_FORMAT_R11G11B10_FLOAT).
    // Under native HUD path, orange HUD must be composited cleanly without needing game orange!
    D3D11_TEXTURE2D_DESC nativeHudDesc = outDesc;
    nativeHudDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    const uint32_t orangeFloatPacked = (0x3C0u) | (0x380u << 11);
    std::array<uint32_t, 16> nativeHudPixels{};
    for (int i = 0; i < 8; ++i) nativeHudPixels[i] = 0;
    for (int i = 8; i < 16; ++i) nativeHudPixels[i] = orangeFloatPacked;
    D3D11_SUBRESOURCE_DATA nativeHudInit{nativeHudPixels.data(), 4 * sizeof(uint32_t), 0};

    ComPtr<ID3D11Texture2D> nativeHudTex;
    ComPtr<ID3D11ShaderResourceView> nativeHudSrv;
    if (FAILED(device->CreateTexture2D(&nativeHudDesc, &nativeHudInit, &nativeHudTex)) ||
        FAILED(device->CreateShaderResourceView(nativeHudTex.Get(), nullptr, &nativeHudSrv))) {
        return 48;
    }

    ok = pass.composite(context.Get(), dlssSrv.Get(), dlssSrv.Get(), dlssSrv.Get(),
        nativeHudSrv.Get(), targetRtv.Get(), 4, 4, 4, 4, /*isHdr=*/true, /*preserveUi=*/true);
    if (!ok) return 50;

    context->CopyResource(staging.Get(), targetTex.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 51;

    rows = static_cast<const uint8_t*>(mapped.pData);
    const uint8_t nhR0 = rows[0], nhG0 = rows[1], nhB0 = rows[2];
    lastRow = rows + 3 * mapped.RowPitch;
    const uint8_t nhR3 = lastRow[0], nhG3 = lastRow[1], nhB3 = lastRow[2];
    context->Unmap(staging.Get(), 0);

    // Row 0 must be DLSS green (no HUD)
    if (nhG0 < 150 || nhR0 > 50 || nhB0 > 50) return 52;
    // Row 3 must contain composited orange HUD from nativeHudTex (even though blackGameTex had 0!)
    if (nhR3 < 100 || nhG3 < 50 || nhB3 > 50) return 53;

    // Test 12: Direct native HUD composite path with isHdr = false
    ok = pass.composite(context.Get(), dlssSrv.Get(), dlssSrv.Get(), dlssSrv.Get(),
        nativeHudSrv.Get(), targetRtv.Get(), 4, 4, 4, 4, /*isHdr=*/false, /*preserveUi=*/true);
    if (!ok) return 54;

    context->CopyResource(staging.Get(), targetTex.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 55;

    rows = static_cast<const uint8_t*>(mapped.pData);
    const uint8_t ldrR0 = rows[0], ldrG0 = rows[1], ldrB0 = rows[2];
    lastRow = rows + 3 * mapped.RowPitch;
    const uint8_t ldrR3 = lastRow[0], ldrG3 = lastRow[1], ldrB3 = lastRow[2];
    context->Unmap(staging.Get(), 0);

    // Row 0 must be DLSS green (no HUD)
    if (ldrG0 < 150 || ldrR0 > 50 || ldrB0 > 50) return 56;
    // Row 3 must contain composited orange HUD from nativeHudTex
    if (ldrR3 < 100 || ldrG3 < 50 || ldrB3 > 50) return 57;

    // Test 13: R16G16B16A16_FLOAT HUD backing panel (transmittance = 0.20) dims background
    D3D11_TEXTURE2D_DESC rgba16Desc = outDesc;
    rgba16Desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    // Half-float: 0.0f = 0x0000, 1.0f = 0x3C00, 0.20f = 0x3266
    struct Half4 { uint16_t r, g, b, a; };
    std::array<Half4, 16> rgba16Pixels{};
    for (int i = 0; i < 8; ++i) rgba16Pixels[i] = {0, 0, 0, 0x3C00};  // Clear: T = 1.0
    for (int i = 8; i < 16; ++i) rgba16Pixels[i] = {0, 0, 0, 0x3266}; // Backing quad: T = 0.20
    D3D11_SUBRESOURCE_DATA rgba16Init{rgba16Pixels.data(), 4 * sizeof(Half4), 0};

    ComPtr<ID3D11Texture2D> rgba16Tex;
    ComPtr<ID3D11ShaderResourceView> rgba16Srv;
    if (FAILED(device->CreateTexture2D(&rgba16Desc, &rgba16Init, &rgba16Tex)) ||
        FAILED(device->CreateShaderResourceView(rgba16Tex.Get(), nullptr, &rgba16Srv))) {
        return 58;
    }

    ok = pass.composite(context.Get(), dlssSrv.Get(), dlssSrv.Get(), dlssSrv.Get(),
        rgba16Srv.Get(), targetRtv.Get(), 4, 4, 4, 4, /*isHdr=*/false, /*preserveUi=*/true);
    if (!ok) return 59;

    context->CopyResource(staging.Get(), targetTex.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 60;

    rows = static_cast<const uint8_t*>(mapped.pData);
    const uint8_t dimR0 = rows[0], dimG0 = rows[1], dimB0 = rows[2];
    lastRow = rows + 3 * mapped.RowPitch;
    const uint8_t dimR3 = lastRow[0], dimG3 = lastRow[1], dimB3 = lastRow[2];
    context->Unmap(staging.Get(), 0);

    // Row 0 must be full DLSS green (T = 1.0 -> G ~ 255)
    if (dimG0 < 200 || dimR0 > 50 || dimB0 > 50) return 61;
    // Row 3 must be dimmed by 20% linear transmittance in sRGB space (0.20^(1/2.2) * 255 ~ 123)
    if (dimG3 > 140 || dimG3 < 95 || dimR3 > 20 || dimB3 > 20) return 62;

    // Test 14: R16G16B16A16_FLOAT HUD panel with both HUD radiance (orange) and transmittance (T = 0.20)
    std::array<Half4, 16> rgba16HudPixels{};
    for (int i = 0; i < 8; ++i) rgba16HudPixels[i] = {0, 0, 0, 0x3C00};            // Clear: (0, 0, 0, 1.0)
    for (int i = 8; i < 16; ++i) rgba16HudPixels[i] = {0x3C00, 0x3800, 0, 0x3266}; // Orange (1.0, 0.5, 0.0) + T = 0.20
    D3D11_SUBRESOURCE_DATA rgba16HudInit{rgba16HudPixels.data(), 4 * sizeof(Half4), 0};

    ComPtr<ID3D11Texture2D> rgba16HudTex;
    ComPtr<ID3D11ShaderResourceView> rgba16HudSrv;
    if (FAILED(device->CreateTexture2D(&rgba16Desc, &rgba16HudInit, &rgba16HudTex)) ||
        FAILED(device->CreateShaderResourceView(rgba16HudTex.Get(), nullptr, &rgba16HudSrv))) {
        return 63;
    }

    ok = pass.composite(context.Get(), dlssSrv.Get(), dlssSrv.Get(), lowSrv.Get(),
        rgba16HudSrv.Get(), targetRtv.Get(), 4, 4, 2, 2, /*isHdr=*/false, /*preserveUi=*/true);
    if (!ok) return 64;

    context->CopyResource(staging.Get(), targetTex.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 65;

    rows = static_cast<const uint8_t*>(mapped.pData);
    const uint8_t combR0 = rows[0], combG0 = rows[1], combB0 = rows[2];
    lastRow = rows + 3 * mapped.RowPitch;
    const uint8_t combR3 = lastRow[0], combG3 = lastRow[1], combB3 = rows[3 * mapped.RowPitch + 2];
    context->Unmap(staging.Get(), 0);

    if (combG0 < 200 || combR0 > 50 || combB0 > 50) return 66;
    if (combR3 < 150 || combG3 < 100 || combB3 > 50) return 67;

    // Test 15: Combined Pass 35 float HUD (row 1) + Pass 54 2D ESC/Main Menu (rows 2-3)
    // Verify that when validHudSrv is bound (flags.w > 0.5), 2D menus drawn in Pass 54 (gameSrv)
    // are still extracted and composited over cScene!
    std::array<Half4, 16> row1HudPixels{};
    for (int i = 0; i < 16; ++i) row1HudPixels[i] = {0, 0, 0, 0x3C00};
    for (int i = 4; i < 8; ++i) row1HudPixels[i] = {0x3C00, 0x3800, 0, 0x0000}; // Row 1: opaque orange HUD
    D3D11_SUBRESOURCE_DATA row1HudInit{row1HudPixels.data(), 4 * sizeof(Half4), 0};
    ComPtr<ID3D11Texture2D> row1HudTex;
    ComPtr<ID3D11ShaderResourceView> row1HudSrv;
    if (FAILED(device->CreateTexture2D(&rgba16Desc, &row1HudInit, &row1HudTex)) ||
        FAILED(device->CreateShaderResourceView(row1HudTex.Get(), nullptr, &row1HudSrv))) {
        return 68;
    }

    ok = pass.composite(context.Get(), dlssSrv.Get(), gameSrv.Get(), dlssSrv.Get(),
        row1HudSrv.Get(), targetRtv.Get(), 4, 4, 4, 4, /*isHdr=*/false, /*preserveUi=*/true);
    if (!ok) return 69;

    context->CopyResource(staging.Get(), targetTex.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 70;

    rows = static_cast<const uint8_t*>(mapped.pData);
    const uint8_t m15R0 = rows[0], m15G0 = rows[1], m15B0 = rows[2];
    const auto* row1Ptr = rows + 1 * mapped.RowPitch;
    const uint8_t m15R1 = row1Ptr[0], m15G1 = row1Ptr[1], m15B1 = row1Ptr[2];
    lastRow = rows + 3 * mapped.RowPitch;
    const uint8_t m15R3 = lastRow[0], m15G3 = lastRow[1], m15B3 = lastRow[2];
    context->Unmap(staging.Get(), 0);

    if (m15G0 < 200 || m15R0 > 50 || m15B0 > 50) return 71; // Row 0: pure DLSS green
    if (m15R1 < 150 || m15G1 < 100 || m15B1 > 50) return 72; // Row 1: Pass 35 orange HUD
    if (m15R3 < 200 || m15G3 < 200 || m15B3 < 200) return 73; // Row 3: Pass 54 white ESC menu

    // Test 16: Pass 52 post-processed HUD SRV (hudTonemapped at t4, flags.w == 3.0)
    // Verify that when hudTonemapped is supplied (e.g. cyan 3D-LUT-graded HUD in row 1),
    // the composite pass uses the game-tonemapped color from hudTonemapped on HUD pixels.
    std::array<uint32_t, 16> toneHudPixels{};
    for (int i = 0; i < 16; ++i) toneHudPixels[i] = 0xFF000000; // Corner (0,0) black floor = 0
    for (int i = 4; i < 8; ++i) toneHudPixels[i] = 0xFFFFFF00;  // Row 1: cyan (R=0, G=255, B=255)
    D3D11_SUBRESOURCE_DATA toneHudInit{toneHudPixels.data(), 4 * sizeof(uint32_t), 0};
    ComPtr<ID3D11Texture2D> toneHudTex;
    ComPtr<ID3D11ShaderResourceView> toneHudSrv;
    if (FAILED(device->CreateTexture2D(&outDesc, &toneHudInit, &toneHudTex)) ||
        FAILED(device->CreateShaderResourceView(toneHudTex.Get(), nullptr, &toneHudSrv))) {
        return 74;
    }

    ok = pass.composite(context.Get(), dlssSrv.Get(), dlssSrv.Get(), dlssSrv.Get(),
        row1HudSrv.Get(), targetRtv.Get(), 4, 4, 4, 4, /*isHdr=*/false, /*preserveUi=*/true,
        toneHudSrv.Get());
    if (!ok) return 75;

    context->CopyResource(staging.Get(), targetTex.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 76;

    rows = static_cast<const uint8_t*>(mapped.pData);
    row1Ptr = rows + 1 * mapped.RowPitch;
    const uint8_t m16R1 = row1Ptr[0], m16G1 = row1Ptr[1], m16B1 = row1Ptr[2];
    context->Unmap(staging.Get(), 0);

    if (m16B1 < 200 || m16G1 < 200) return 77; // Row 1 must reflect cyan from toneHudSrv

    // Test 17: hasTonemappedHud (flags.w == 3.0) with translucent dark HUD backing (RGB = 0, T = 0.20)
    // Verify that linear transmittance T = 0.20 dims the sRGB background in linear light
    // (0.20^(1/2.2) * 255 ~ 123), NOT in gamma space (0.20 * 255 = 51).
    std::array<uint32_t, 16> toneBackingPixels{};
    for (int i = 0; i < 8; ++i) toneBackingPixels[i] = 0xFF00FF00;  // Clear rows 0-1: tonemapped green (0, 255, 0)
    for (int i = 8; i < 16; ++i) toneBackingPixels[i] = 0xFF007B00; // Backing rows 2-3: tonemapped green * 0.20^(1/2.2) -> G = 123 (0x7B)
    D3D11_SUBRESOURCE_DATA toneBackingInit{toneBackingPixels.data(), 4 * sizeof(uint32_t), 0};
    ComPtr<ID3D11Texture2D> toneBackingTex;
    ComPtr<ID3D11ShaderResourceView> toneBackingSrv;
    if (FAILED(device->CreateTexture2D(&outDesc, &toneBackingInit, &toneBackingTex)) ||
        FAILED(device->CreateShaderResourceView(toneBackingTex.Get(), nullptr, &toneBackingSrv))) {
        return 78;
    }

    ok = pass.composite(context.Get(), dlssSrv.Get(), dlssSrv.Get(), dlssSrv.Get(),
        rgba16Srv.Get(), targetRtv.Get(), 4, 4, 4, 4, /*isHdr=*/false, /*preserveUi=*/true,
        toneBackingSrv.Get());
    if (!ok) return 79;

    context->CopyResource(staging.Get(), targetTex.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 80;

    rows = static_cast<const uint8_t*>(mapped.pData);
    lastRow = rows + 3 * mapped.RowPitch;
    const uint8_t m17G3 = lastRow[1];
    context->Unmap(staging.Get(), 0);

    // Must match linear-light dimmed green (~123), never gamma-darkened (~51)
    if (m17G3 < 110 || m17G3 > 135) return 81;

    // Test 18: Pass 54 Starport Services / modal 2D menu overlapping Pass 35 Cockpit HUD on the same pixels
    // Row 1 has Pass 35 orange HUD only (tGame == tLowWorld == green).
    // Rows 2-3 have BOTH Pass 35 orange HUD in tHudMask AND Pass 54 white Starport Services menu in tGame (gameSrv).
    // Verify that Row 1 outputs Pass 35 orange HUD, while Row 3 outputs Pass 54 white Starport Services ON TOP OF the HUD!
    std::array<Half4, 16> overlapHudPixels{};
    for (int i = 0; i < 4; ++i) overlapHudPixels[i] = {0, 0, 0, 0x3C00};             // Row 0: clear
    for (int i = 4; i < 16; ++i) overlapHudPixels[i] = {0x3C00, 0x3800, 0, 0x0000};  // Rows 1-3: opaque orange HUD
    D3D11_SUBRESOURCE_DATA overlapHudInit{overlapHudPixels.data(), 4 * sizeof(Half4), 0};
    ComPtr<ID3D11Texture2D> overlapHudTex;
    ComPtr<ID3D11ShaderResourceView> overlapHudSrv;
    if (FAILED(device->CreateTexture2D(&rgba16Desc, &overlapHudInit, &overlapHudTex)) ||
        FAILED(device->CreateShaderResourceView(overlapHudTex.Get(), nullptr, &overlapHudSrv))) {
        return 82;
    }

    ok = pass.composite(context.Get(), dlssSrv.Get(), gameSrv.Get(), dlssSrv.Get(),
        overlapHudSrv.Get(), targetRtv.Get(), 4, 4, 4, 4, /*isHdr=*/false, /*preserveUi=*/true);
    if (!ok) return 83;

    context->CopyResource(staging.Get(), targetTex.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 84;

    rows = static_cast<const uint8_t*>(mapped.pData);
    const uint8_t m18R0 = rows[0], m18G0 = rows[1], m18B0 = rows[2];
    row1Ptr = rows + 1 * mapped.RowPitch;
    const uint8_t m18R1 = row1Ptr[0], m18G1 = row1Ptr[1], m18B1 = row1Ptr[2];
    lastRow = rows + 3 * mapped.RowPitch;
    const uint8_t m18R3 = lastRow[0], m18G3 = lastRow[1], m18B3 = lastRow[2];
    context->Unmap(staging.Get(), 0);

    if (m18G0 < 200 || m18R0 > 50 || m18B0 > 50) return 85;   // Row 0: pure DLSS green
    if (m18R1 < 150 || m18G1 < 100 || m18B1 > 50) return 86;  // Row 1: non-overlapped Pass 35 orange HUD preserved
    if (m18R3 < 200 || m18G3 < 200 || m18B3 < 200) return 87; // Row 3: Pass 54 Starport Services white menu covers Pass 35 HUD

    return 0;
}