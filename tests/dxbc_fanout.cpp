#include "dxbc_fanout.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

int main() {
    const char* vs_source =
        "float4 main(uint i:SV_VertexID):SV_Position {"
        "float2 p=float2(i==2?3:-1,i==1?3:-1);return float4(p,0,1);}";
    const char* ps_source =
        "float4 main():SV_Target {return float4(0.25,0.5,0.75,1);}";
    ComPtr<ID3DBlob> vs_code, ps_code;
    if (FAILED(D3DCompile(vs_source, std::strlen(vs_source), nullptr, nullptr,
            nullptr, "main", "vs_5_0", 0, 0, &vs_code, nullptr)) ||
        FAILED(D3DCompile(ps_source, std::strlen(ps_source), nullptr, nullptr,
            nullptr, "main", "ps_5_0", 0, 0, &ps_code, nullptr))) return 1;
    std::vector<BYTE> patched;
    std::string reason;
    if (edpe::colourFanout(nullptr, 0, patched, reason) ||
        !edpe::colourFanout(ps_code->GetBufferPointer(), ps_code->GetBufferSize(),
            patched, reason)) return 2;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context))) return 3;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> original, fanout;
    if (FAILED(device->CreateVertexShader(vs_code->GetBufferPointer(),
            vs_code->GetBufferSize(), nullptr, &vertex)) ||
        FAILED(device->CreatePixelShader(ps_code->GetBufferPointer(),
            ps_code->GetBufferSize(), nullptr, &original)) ||
        FAILED(device->CreatePixelShader(patched.data(), patched.size(),
            nullptr, &fanout))) return 4;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 4;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> images[3];
    ComPtr<ID3D11RenderTargetView> targets[3];
    for (unsigned i = 0; i < 3; ++i)
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &images[i])) ||
            FAILED(device->CreateRenderTargetView(images[i].Get(), nullptr,
                &targets[i]))) return 5;
    const D3D11_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex.Get(), nullptr, 0);
    context->PSSetShader(original.Get(), nullptr, 0);
    ID3D11RenderTargetView* baseline = targets[0].Get();
    context->OMSetRenderTargets(1, &baseline, nullptr);
    context->Draw(3, 0);
    ID3D11RenderTargetView* paired[] = {targets[1].Get(), targets[2].Get()};
    context->OMSetRenderTargets(2, paired, nullptr);
    context->PSSetShader(fanout.Get(), nullptr, 0);
    context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    float observed[3][4]{};
    for (unsigned i = 0; i < 3; ++i) {
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return 6;
        context->CopyResource(staging.Get(), images[i].Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0,
                &mapped))) return 7;
        std::memcpy(observed[i], mapped.pData, sizeof(observed[i]));
        context->Unmap(staging.Get(), 0);
    }
    const float expected[] = {0.25f, 0.5f, 0.75f, 1.0f};
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned channel = 0; channel < 4; ++channel)
            if (std::fabs(observed[i][channel] - expected[channel]) > 0.001f) {
                std::fprintf(stderr, "fanout target %u channel %u: %f\n",
                    i, channel, observed[i][channel]);
                return 8;
            }
    return 0;
}
