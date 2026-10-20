#include "reactive_mask_pass.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <array>
#include <cstdint>
#include <iostream>

using Microsoft::WRL::ComPtr;

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &context))) {
        std::cerr << "Failed to create WARP D3D11 device\n";
        return 1;
    }

    edpe::ReactiveMaskPass pass;
    if (!pass.initialize(device.Get(), context.Get())) {
        std::cerr << "Failed to initialize ReactiveMaskPass\n";
        return 2;
    }

    // Create 4x4 input textures:
    // Format: DXGI_FORMAT_R8G8B8A8_UNORM (standard SRV accessible format for testing)
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = texDesc.Height = 4;
    texDesc.MipLevels = texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    // Emissive: pixel (0, 0) has orange/yellow glow (R=255, G=200, B=50, A=255)
    // Other pixels are black.
    std::array<uint32_t, 16> emissivePixels{};
    emissivePixels.fill(0x00000000);
    emissivePixels[0] = 0xFF32C8FF; // A=255, B=50, G=200, R=255
    D3D11_SUBRESOURCE_DATA emissiveInit{emissivePixels.data(), 4 * sizeof(uint32_t), 0};

    // Pre-HUD: pixel (2, 2) has dark background (R=30, G=30, B=30, A=255)
    // Pixel (3, 3) has identical dark background.
    std::array<uint32_t, 16> preHudPixels{};
    preHudPixels.fill(0x00000000);
    preHudPixels[2 * 4 + 2] = 0xFF1E1E1E;
    preHudPixels[3 * 4 + 3] = 0xFF1E1E1E;
    D3D11_SUBRESOURCE_DATA preHudInit{preHudPixels.data(), 4 * sizeof(uint32_t), 0};

    // Post-HUD: pixel (2, 2) has cyan HUD line drawn over it (R=30, G=230, B=230, A=255)
    // Pixel (3, 3) is unchanged (R=30, G=30, B=30, A=255).
    std::array<uint32_t, 16> postHudPixels{};
    postHudPixels.fill(0x00000000);
    postHudPixels[2 * 4 + 2] = 0xFFE6E61E; // A=255, B=230, G=230, R=30
    postHudPixels[3 * 4 + 3] = 0xFF1E1E1E; // unchanged
    D3D11_SUBRESOURCE_DATA postHudInit{postHudPixels.data(), 4 * sizeof(uint32_t), 0};

    ComPtr<ID3D11Texture2D> emissiveTex, preHudTex, postHudTex;
    ComPtr<ID3D11ShaderResourceView> emissiveSrv, preHudSrv, postHudSrv;

    if (FAILED(device->CreateTexture2D(&texDesc, &emissiveInit, &emissiveTex)) ||
        FAILED(device->CreateShaderResourceView(emissiveTex.Get(), nullptr, &emissiveSrv)) ||
        FAILED(device->CreateTexture2D(&texDesc, &preHudInit, &preHudTex)) ||
        FAILED(device->CreateShaderResourceView(preHudTex.Get(), nullptr, &preHudSrv)) ||
        FAILED(device->CreateTexture2D(&texDesc, &postHudInit, &postHudTex)) ||
        FAILED(device->CreateShaderResourceView(postHudTex.Get(), nullptr, &postHudSrv))) {
        std::cerr << "Failed to create input textures or SRVs\n";
        return 3;
    }

    // Set custom state before calling render to verify state preservation
    const D3D11_VIEWPORT prior_viewport{0.0f, 0.0f, 2.0f, 2.0f, 0.0f, 1.0f};
    context->RSSetViewports(1, &prior_viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

    const bool renderOk = pass.render(
        context.Get(),
        emissiveSrv.Get(),
        preHudSrv.Get(),
        postHudSrv.Get(),
        4, 4,
        1.0f,  // emissive weight
        1.5f,  // hud weight
        0.01f, // emissive threshold
        0.005f // hud threshold
    );

    if (!renderOk) {
        std::cerr << "ReactiveMaskPass::render failed\n";
        return 4;
    }

    if (!pass.outputTexture() || !pass.outputSrv()) {
        std::cerr << "ReactiveMaskPass missing output texture or SRV\n";
        return 5;
    }

    if (pass.width() != 4 || pass.height() != 4) {
        std::cerr << "ReactiveMaskPass dimensions mismatch\n";
        return 6;
    }

    // Verify context state restoration
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    UINT viewport_count = 1;
    D3D11_VIEWPORT viewport{};
    context->IAGetPrimitiveTopology(&topology);
    context->RSGetViewports(&viewport_count, &viewport);
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_LINELIST ||
        viewport_count != 1 || viewport.Width != 2.0f) {
        std::cerr << "Context state was not properly restored\n";
        return 7;
    }

    // Read back output texture
    D3D11_TEXTURE2D_DESC readDesc{};
    pass.outputTexture()->GetDesc(&readDesc);
    if (readDesc.Format != DXGI_FORMAT_R8_UNORM) {
        std::cerr << "Output format is not R8_UNORM\n";
        return 8;
    }

    readDesc.Usage = D3D11_USAGE_STAGING;
    readDesc.BindFlags = 0;
    readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&readDesc, nullptr, &staging))) {
        std::cerr << "Failed to create staging texture\n";
        return 9;
    }

    context->CopyResource(staging.Get(), pass.outputTexture());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        std::cerr << "Failed to map staging texture\n";
        return 10;
    }

    const auto* rows = static_cast<const uint8_t*>(mapped.pData);
    // Pixel (0, 0): emissive light -> should have high reactivity (> 0)
    const uint8_t r_0_0 = rows[0];
    // Pixel (1, 1): empty -> should have 0 reactivity
    const uint8_t r_1_1 = rows[mapped.RowPitch + 1];
    // Pixel (2, 2): HUD draw -> should have high reactivity (> 0)
    const uint8_t r_2_2 = rows[2 * mapped.RowPitch + 2];
    // Pixel (3, 3): unchanged background -> should have 0 reactivity
    const uint8_t r_3_3 = rows[3 * mapped.RowPitch + 3];

    context->Unmap(staging.Get(), 0);

    if (r_0_0 == 0) {
        std::cerr << "Emissive pixel (0,0) has zero reactivity\n";
        return 11;
    }
    if (r_1_1 != 0) {
        std::cerr << "Empty pixel (1,1) has non-zero reactivity: " << static_cast<int>(r_1_1) << "\n";
        return 12;
    }
    if (r_2_2 == 0) {
        std::cerr << "HUD pixel (2,2) has zero reactivity\n";
        return 13;
    }
    if (r_3_3 != 0) {
        std::cerr << "Unchanged background pixel (3,3) has non-zero reactivity: " << static_cast<int>(r_3_3) << "\n";
        return 14;
    }

    // Fail-open test: null context
    if (pass.render(nullptr, emissiveSrv.Get(), preHudSrv.Get(), postHudSrv.Get(), 4, 4, 1.0f, 1.5f, 0.01f, 0.005f)) {
        std::cerr << "render with null context should fail open\n";
        return 15;
    }

    // Fail-open test: zero dimensions
    if (pass.render(context.Get(), emissiveSrv.Get(), preHudSrv.Get(), postHudSrv.Get(), 0, 4, 1.0f, 1.5f, 0.01f, 0.005f)) {
        std::cerr << "render with zero width should fail open\n";
        return 16;
    }

    // -------------------------------------------------------------------------
    // Test renderDebugView (Mode 0: Pure Grayscale Mask, Mode 1: Overlay)
    // -------------------------------------------------------------------------
    D3D11_TEXTURE2D_DESC dbgTargetDesc{};
    dbgTargetDesc.Width = dbgTargetDesc.Height = 4;
    dbgTargetDesc.MipLevels = dbgTargetDesc.ArraySize = 1;
    dbgTargetDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dbgTargetDesc.SampleDesc.Count = 1;
    dbgTargetDesc.Usage = D3D11_USAGE_DEFAULT;
    dbgTargetDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> dbgTargetTex;
    ComPtr<ID3D11RenderTargetView> dbgTargetRtv;
    if (FAILED(device->CreateTexture2D(&dbgTargetDesc, nullptr, &dbgTargetTex)) ||
        FAILED(device->CreateRenderTargetView(dbgTargetTex.Get(), nullptr, &dbgTargetRtv))) {
        std::cerr << "Failed to create debug view target RTV\n";
        return 17;
    }

    const bool dbgOk = pass.renderDebugView(context.Get(), postHudSrv.Get(), dbgTargetRtv.Get(), 4, 4, 0, 1.0f, false);
    if (!dbgOk) {
        std::cerr << "renderDebugView Mode 0 failed\n";
        return 18;
    }

    // Read back debug target texture
    D3D11_TEXTURE2D_DESC dbgStagingDesc = dbgTargetDesc;
    dbgStagingDesc.Usage = D3D11_USAGE_STAGING;
    dbgStagingDesc.BindFlags = 0;
    dbgStagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> dbgStaging;
    if (FAILED(device->CreateTexture2D(&dbgStagingDesc, nullptr, &dbgStaging))) {
        std::cerr << "Failed to create debug staging texture\n";
        return 19;
    }

    context->CopyResource(dbgStaging.Get(), dbgTargetTex.Get());
    D3D11_MAPPED_SUBRESOURCE dbgMapped{};
    if (FAILED(context->Map(dbgStaging.Get(), 0, D3D11_MAP_READ, 0, &dbgMapped))) {
        std::cerr << "Failed to map debug staging texture\n";
        return 20;
    }

    const auto* dbgRows = static_cast<const uint8_t*>(dbgMapped.pData);
    // In Mode 0 (Pure Grayscale): (0,0) and (2,2) must be bright (> 0), (1,1) and (3,3) must be 0
    const uint8_t dbg_0_0 = dbgRows[0]; // R channel
    const uint8_t dbg_1_1 = dbgRows[dbgMapped.RowPitch + 4];
    const uint8_t dbg_2_2 = dbgRows[2 * dbgMapped.RowPitch + 8];
    const uint8_t dbg_3_3 = dbgRows[3 * dbgMapped.RowPitch + 12];
    context->Unmap(dbgStaging.Get(), 0);

    if (dbg_0_0 == 0 || dbg_2_2 == 0) {
        std::cerr << "Debug view Mode 0 failed to highlight reactive pixels\n";
        return 21;
    }
    if (dbg_1_1 != 0 || dbg_3_3 != 0) {
        std::cerr << "Debug view Mode 0 produced non-zero for background pixels\n";
        return 22;
    }

    // Verify Mode 1 (Overlay) runs cleanly
    if (!pass.renderDebugView(context.Get(), postHudSrv.Get(), dbgTargetRtv.Get(), 4, 4, 1, 1.0f, false)) {
        std::cerr << "renderDebugView Mode 1 failed\n";
        return 23;
    }

    // Verify fail-open for renderDebugView
    if (pass.renderDebugView(nullptr, postHudSrv.Get(), dbgTargetRtv.Get(), 4, 4, 0, 1.0f, false)) {
        std::cerr << "renderDebugView with null context should fail open\n";
        return 24;
    }
    if (pass.renderDebugView(context.Get(), postHudSrv.Get(), nullptr, 4, 4, 0, 1.0f, false)) {
        std::cerr << "renderDebugView with null RTV should fail open\n";
        return 25;
    }

    return 0; // All reactive mask tests passed
}
