#include "ui_composite_pass.h"

#include <d3d11.h>
#include <d3dcompiler.h>
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

    bool ok = pass.composite(context.Get(), dlssSrv.Get(), gameSrv.Get(), lowSrv.Get(),
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

    return 0;
}
