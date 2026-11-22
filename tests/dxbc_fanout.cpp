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

    // Verify on-foot helmet visor VS/PS patching fails open on non-visor shaders,
    // produces valid SM5 DXBC, shifts screen visor UV (t2/t3) by +jitter while keeping
    // already-jittered pre-visor HDR t1 unshifted.
    if (edpe::patchVisorVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), patched, reason) ||
        edpe::patchVisorPixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), patched, reason)) {
        std::fprintf(stderr, "non-visor shaders should not match visor patcher\n");
        return 9;
    }

    const char* visor_vs_src = R"hlsl(
cbuffer cb1 : register(b1) { float4 c1[91]; };
cbuffer cb2 : register(b2) { float4 c2[2]; };
struct Out {
    float4 tint : TINT0;
    float2 uv : TEX0;
    float3 col : COL0;
    float4 pos : SV_Position;
};
Out main(uint vid : SV_VertexID) {
    Out o;
    o.tint = float4(c2[0].xyz, 1.0f - c2[1].x);
    o.uv.x = (float)(vid >> 1) * 2.0f;
    o.uv.y = 1.0f - (float)(vid & 1u) * 2.0f;
    o.col = c1[90].xyz + c2[1].y;
    o.pos = float4((float)(vid >> 1) * 4.0f - 1.0f, (1.0f - c2[1].x) + (float)(vid & 1u) * 4.0f - 1.0f, 0.99f, 1.0f);
    return o;
}
)hlsl";

    const char* visor_ps_src = R"hlsl(
cbuffer cb1 : register(b1) { float4 c1[333]; };
cbuffer cb2 : register(b2) { float4 c2[9]; };
Texture2D t0 : register(t0);
Texture2D t1 : register(t1);
Texture2D t2 : register(t2);
Texture2D t3 : register(t3);
Texture2D t4 : register(t4);
SamplerState s0 : register(s0);
SamplerState s1 : register(s1);
struct In {
    float4 tint : TINT0;
    float2 uv : TEX0;
    float3 col : COL0;
    float4 pos : SV_Position;
};
struct Out {
    float4 c0 : SV_Target0;
    float4 c1 : SV_Target1;
};
Out main(In i) {
    float2 suv = i.pos.xy * c1[332].zw;
    float vy = i.pos.y * c1[332].w + i.tint.w;
    float streak = t3.Sample(s1, float2(suv.x, vy)).r;
    float drop = t2.Sample(s1, i.uv).r;
    float t4v = t4.Sample(s1, i.uv).r;
    float t0v = t0.Load(int3(0, 0, 0)).r;
    float worldSample = t1.Sample(s0, suv + float2(c2[4].x, c2[8].x)).r;
    float worldEdge = t1.Sample(s0, i.uv).r;
    Out o;
    o.c0 = float4(streak + t0v * c2[0].x, drop + t4v * c2[0].y, worldSample + worldEdge * c2[0].z, 1.0f);
    o.c1 = float4(i.tint.xyz, 1.0f);
    return o;
}
)hlsl";

    ComPtr<ID3DBlob> v_vs_blob, v_ps_blob, v_err;
    if (FAILED(D3DCompile(visor_vs_src, std::strlen(visor_vs_src), nullptr, nullptr,
            nullptr, "main", "vs_5_0", 0, 0, &v_vs_blob, &v_err)) ||
        FAILED(D3DCompile(visor_ps_src, std::strlen(visor_ps_src), nullptr, nullptr,
            nullptr, "main", "ps_5_0", 0, 0, &v_ps_blob, &v_err))) {
        std::fprintf(stderr, "visor test compile failed: %s\n",
            v_err ? static_cast<const char*>(v_err->GetBufferPointer()) : "");
        return 10;
    }

    if (edpe::isVisorVertexShaderBytecode(vs_code->GetBufferPointer(), vs_code->GetBufferSize()) ||
        !edpe::isVisorVertexShaderBytecode(v_vs_blob->GetBufferPointer(), v_vs_blob->GetBufferSize())) {
        std::fprintf(stderr, "isVisorVertexShaderBytecode classification failed\n");
        return 12;
    }

    std::vector<BYTE> v_vs_patched, v_ps_patched;
    if (!edpe::patchVisorVertexShader(v_vs_blob->GetBufferPointer(), v_vs_blob->GetBufferSize(),
            v_vs_patched, reason)) {
        std::fprintf(stderr, "patchVisorVertexShader failed: %s\n", reason.c_str());
        return 11;
    }
    if (!edpe::patchVisorPixelShader(v_ps_blob->GetBufferPointer(), v_ps_blob->GetBufferSize(),
            v_ps_patched, reason)) {
        std::fprintf(stderr, "patchVisorPixelShader failed: %s\n", reason.c_str());
        return 12;
    }

    ComPtr<ID3DBlob> disVs, disPs;
    if (FAILED(D3DDisassemble(v_vs_patched.data(), v_vs_patched.size(), 0, nullptr, &disVs)) ||
        FAILED(D3DDisassemble(v_ps_patched.data(), v_ps_patched.size(), 0, nullptr, &disPs))) {
        std::fprintf(stderr, "D3DDisassemble failed on patched visor shaders\n");
        return 13;
    }

    ComPtr<ID3D11VertexShader> v_vs;
    ComPtr<ID3D11PixelShader> v_ps;
    if (FAILED(device->CreateVertexShader(v_vs_patched.data(), v_vs_patched.size(), nullptr, &v_vs)) ||
        FAILED(device->CreatePixelShader(v_ps_patched.data(), v_ps_patched.size(), nullptr, &v_ps))) {
        std::fprintf(stderr, "CreateShader failed on patched visor shaders\n");
        return 14;
    }

    // Create 4x1 R32G32B32A32_FLOAT textures where texel x encodes x * 0.25f.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 4;
    td.Height = 4;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    float ramp[16 * 4]{};
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            ramp[(y * 4 + x) * 4 + 0] = static_cast<float>(x) * 0.25f;
        }
    }
    D3D11_SUBRESOURCE_DATA initData{ramp, 4 * 16, 0};
    ComPtr<ID3D11Texture2D> rampTex;
    ComPtr<ID3D11ShaderResourceView> rampSrv;
    if (FAILED(device->CreateTexture2D(&td, &initData, &rampTex)) ||
        FAILED(device->CreateShaderResourceView(rampTex.Get(), nullptr, &rampSrv))) {
        return 15;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> pointSamp;
    if (FAILED(device->CreateSamplerState(&sd, &pointSamp))) return 16;

    auto makeCb = [&](UINT bytes, const void* data) {
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = bytes;
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA cd{data, 0, 0};
        ComPtr<ID3D11Buffer> buf;
        device->CreateBuffer(&cbd, &cd, &buf);
        return buf;
    };
    float cb1Data[333 * 4]{};
    cb1Data[332 * 4 + 0] = 4.0f;
    cb1Data[332 * 4 + 1] = 4.0f;
    cb1Data[332 * 4 + 2] = 0.25f;
    cb1Data[332 * 4 + 3] = 0.25f;
    float cb2Data[9 * 4]{};
    // Shift visor right by +1.0 px (cb13[0].xy = (-0.25, 0), cb13[0].zw = (-1, 0), cb13[1].xy = (+0.25, 0)).
    const float cb13Data[8] = {-0.25f, 0.0f, -1.0f, 0.0f, 0.25f, 0.0f, 0.0f, 0.0f};
    ComPtr<ID3D11Buffer> cb1 = makeCb(sizeof(cb1Data), cb1Data);
    ComPtr<ID3D11Buffer> cb2 = makeCb(sizeof(cb2Data), cb2Data);
    ComPtr<ID3D11Buffer> cb13 = makeCb(sizeof(cb13Data), cb13Data);

    D3D11_TEXTURE2D_DESC outDesc{};
    outDesc.Width = 4;
    outDesc.Height = 4;
    outDesc.MipLevels = outDesc.ArraySize = outDesc.SampleDesc.Count = 1;
    outDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    outDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> outTex0, outTex1, outStaging;
    ComPtr<ID3D11RenderTargetView> outRtv0, outRtv1;
    outDesc.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(device->CreateTexture2D(&outDesc, nullptr, &outTex0)) ||
        FAILED(device->CreateTexture2D(&outDesc, nullptr, &outTex1)) ||
        FAILED(device->CreateRenderTargetView(outTex0.Get(), nullptr, &outRtv0)) ||
        FAILED(device->CreateRenderTargetView(outTex1.Get(), nullptr, &outRtv1))) {
        return 17;
    }
    outDesc.Usage = D3D11_USAGE_STAGING;
    outDesc.BindFlags = 0;
    outDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&outDesc, nullptr, &outStaging))) return 17;

    ID3D11RenderTargetView* outRtvs[] = {outRtv0.Get(), outRtv1.Get()};
    context->OMSetRenderTargets(2, outRtvs, nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->RSSetViewports(1, &viewport);
    context->VSSetShader(v_vs.Get(), nullptr, 0);
    context->PSSetShader(v_ps.Get(), nullptr, 0);
    ID3D11Buffer* cbsVs[] = {cb1.Get(), cb2.Get()};
    ID3D11Buffer* cb13Ptr = cb13.Get();
    context->VSSetConstantBuffers(1, 2, cbsVs);
    context->VSSetConstantBuffers(13, 1, &cb13Ptr);
    context->PSSetConstantBuffers(1, 2, cbsVs);
    context->PSSetConstantBuffers(13, 1, &cb13Ptr);
    ID3D11ShaderResourceView* srvs[] = {rampSrv.Get(), rampSrv.Get(), rampSrv.Get(), rampSrv.Get(), rampSrv.Get()};
    context->PSSetShaderResources(0, 5, srvs);
    ID3D11SamplerState* samps[] = {pointSamp.Get(), pointSamp.Get()};
    context->PSSetSamplers(0, 2, samps);
    context->DrawInstanced(3, 1, 0, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);

    context->CopyResource(outStaging.Get(), outTex0.Get());
    D3D11_MAPPED_SUBRESOURCE mappedOut{};
    if (FAILED(context->Map(outStaging.Get(), 0, D3D11_MAP_READ, 0, &mappedOut))) return 18;
    const auto* row0 = static_cast<const float*>(mappedOut.pData);
    // At pixel x = 2:
    // - streak (t3, uses shifted v3.x = 2.5 - 1.0 = 1.5 -> texel x=1 -> 0.25)
    // - drop (t2, uses shifted v1.x = 2.5/4 - 0.25 = 0.375 -> texel x=1 -> 0.25)
    // - world (t1, shifted +0.25 back -> texel x=2 -> 0.50)
    const float streakAt2 = row0[2 * 4 + 0];
    const float dropAt2 = row0[2 * 4 + 1];
    const float worldAt2 = row0[2 * 4 + 2];
    context->Unmap(outStaging.Get(), 0);
    if (std::fabs(streakAt2 - 0.25f) > 0.001f ||
        std::fabs(dropAt2 - 0.25f) > 0.001f ||
        std::fabs(worldAt2 - 0.50f) > 0.001f) {
        std::fprintf(stderr, "visor jitter mismatch at x=2: streak=%.3f drop=%.3f world=%.3f\n",
            streakAt2, dropAt2, worldAt2);
        return 19;
    }

    // Verify DXBC vertex shader projection CB classification (Issue #24).
    const char* vs_cb0_proj_src = R"hlsl(
cbuffer cb0 : register(b0) { float4 c0[8]; };
cbuffer cb1 : register(b1) { float4 c1[276]; };
float4 main(float3 pos : POSITION) : SV_Position {
    float4 p = float4(pos + c1[275].xyz, 1.0f);
    return float4(dot(c0[4], p), dot(c0[5], p), dot(c0[6], p), dot(c0[7], p));
}
)hlsl";
    const char* vs_cb1_proj_src = R"hlsl(
cbuffer cb0 : register(b0) { float4 c0[12]; };
cbuffer cb1 : register(b1) { float4 c1[279]; };
float4 main(float4 pos : POSITION) : SV_Position {
    float3 w = float3(dot(pos, c0[9]), dot(pos, c0[10]), dot(pos, c0[11]));
    return w.x * c1[270] + w.y * c1[271] + w.z * c1[272] + c1[273];
}
)hlsl";
    const char* vs_none_proj_src = R"hlsl(
cbuffer cb0 : register(b0) { float4 c0[12]; };
float4 main(float4 pos : POSITION) : SV_Position {
    return float4(dot(pos, c0[0]), dot(pos, c0[1]), dot(pos, c0[2]), dot(pos, c0[3]));
}
)hlsl";
    const char* vs_both_proj_src = R"hlsl(
cbuffer cb0 : register(b0) { float4 c0[8]; };
cbuffer cb1 : register(b1) { float4 c1[274]; };
float4 main(float4 pos : POSITION) : SV_Position {
    float4 a = float4(dot(c0[4], pos), dot(c0[5], pos), dot(c0[6], pos), dot(c0[7], pos));
    float4 b = pos.x * c1[270] + pos.y * c1[271] + pos.z * c1[272] + c1[273];
    return a + b;
}
)hlsl";
    ComPtr<ID3DBlob> bCb0Proj, bCb1Proj, bNoneProj, bBothProj, bErr;
    if (FAILED(D3DCompile(vs_cb0_proj_src, std::strlen(vs_cb0_proj_src), nullptr, nullptr,
            nullptr, "main", "vs_5_0", 0, 0, &bCb0Proj, &bErr)) ||
        FAILED(D3DCompile(vs_cb1_proj_src, std::strlen(vs_cb1_proj_src), nullptr, nullptr,
            nullptr, "main", "vs_5_0", 0, 0, &bCb1Proj, &bErr)) ||
        FAILED(D3DCompile(vs_none_proj_src, std::strlen(vs_none_proj_src), nullptr, nullptr,
            nullptr, "main", "vs_5_0", 0, 0, &bNoneProj, &bErr)) ||
        FAILED(D3DCompile(vs_both_proj_src, std::strlen(vs_both_proj_src), nullptr, nullptr,
            nullptr, "main", "vs_5_0", 0, 0, &bBothProj, &bErr))) {
        std::fprintf(stderr, "projection classifier test compile failed: %s\n",
            bErr ? static_cast<const char*>(bErr->GetBufferPointer()) : "");
        return 20;
    }
    if (edpe::classifyVsProjectionCb(bCb0Proj->GetBufferPointer(), bCb0Proj->GetBufferSize()) !=
        edpe::VsProjectionCb::Cb0Dot4To7) return 21;
    if (edpe::classifyVsProjectionCb(bCb1Proj->GetBufferPointer(), bCb1Proj->GetBufferSize()) !=
        edpe::VsProjectionCb::Cb1Proj270To273) return 22;
    if (edpe::classifyVsProjectionCb(vs_code->GetBufferPointer(), vs_code->GetBufferSize()) !=
            edpe::VsProjectionCb::None ||
        edpe::classifyVsProjectionCb(bNoneProj->GetBufferPointer(), bNoneProj->GetBufferSize()) !=
            edpe::VsProjectionCb::None) return 23;
    if (edpe::classifyVsProjectionCb(bBothProj->GetBufferPointer(), bBothProj->GetBufferSize()) !=
        edpe::VsProjectionCb::Both) return 24;
    if (edpe::classifyVsProjectionCb(nullptr, 0) != edpe::VsProjectionCb::Unknown ||
        edpe::classifyVsProjectionCb(bCb0Proj->GetBufferPointer(), 16) != edpe::VsProjectionCb::Unknown ||
        edpe::classifyVsProjectionCb(ps_code->GetBufferPointer(), ps_code->GetBufferSize()) !=
            edpe::VsProjectionCb::Unknown) return 25;

    return 0;
}
