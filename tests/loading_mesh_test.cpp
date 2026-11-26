#include "loading_mesh.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct alignas(16) TestCb0 {
    float v[12][4];
};

struct alignas(16) TestCb1 {
    float v[276][4];
};

struct alignas(16) TestCb2 {
    float params[4];
    float bones[edpe::kMaxLoadingMeshBones * 12];
};

struct TestInstanceData {
    uint32_t header[4];
    float pos[3];
    uint32_t pad[77];
};
static_assert(sizeof(TestInstanceData) == 336, "InstanceData stride must be 336");

uint32_t floatAsUint(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

bool createDeviceAndContext(ComPtr<ID3D11Device>& dev, ComPtr<ID3D11DeviceContext>& ctx) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 1, D3D11_SDK_VERSION, dev.GetAddressOf(), nullptr, ctx.GetAddressOf());
    if (SUCCEEDED(hr)) return true;
    hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        levels, 1, D3D11_SDK_VERSION, dev.GetAddressOf(), nullptr, ctx.GetAddressOf());
    return SUCCEEDED(hr);
}

bool renderFrameAtTime(ID3D11Device* dev,
                       ID3D11DeviceContext* ctx,
                       const edpe::ParsedLoadingMesh& mesh,
                       ID3D11InputLayout* il,
                       ID3D11VertexShader* vs,
                       ID3D11PixelShader* ps,
                       ID3D11Buffer* vb0,
                       ID3D11Buffer* vb1,
                       ID3D11Buffer* ib,
                       ID3D11Buffer* cb0,
                       ID3D11Buffer* cb1,
                       ID3D11Buffer* cb2,
                       ID3D11ShaderResourceView* t33Srv,
                       ID3D11RenderTargetView* rtv,
                       ID3D11DepthStencilView* dsv,
                       ID3D11RasterizerState* rs,
                       ID3D11DepthStencilState* dss,
                       ID3D11Texture2D* colorTex,
                       ID3D11Texture2D* stagingTex,
                       UINT width,
                       UINT height,
                       float timeSec,
                       bool useGameSpin,
                       std::vector<uint8_t>& outRgba) {
    TestCb2 cb2Data{};
    cb2Data.params[0] = useGameSpin ? 1.0f : 0.0f;
    edpe::sampleLoadingMeshBones(mesh, timeSec, cb2Data.bones);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(cb2, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    std::memcpy(mapped.pData, &cb2Data, sizeof(cb2Data));
    ctx->Unmap(cb2, 0);

    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(rtv, clearColor);
    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.0f, 0); // Reversed-Z far=0

    D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rs);
    ctx->OMSetRenderTargets(1, &rtv, dsv);
    ctx->OMSetDepthStencilState(dss, 0);

    ID3D11Buffer* vbs[2] = {vb0, vb1};
    UINT strides[2] = {8u, static_cast<UINT>(sizeof(edpe::LoadingMeshVertex))};
    UINT offsets[2] = {0u, 0u};
    ctx->IASetInputLayout(il);
    ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
    ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D11Buffer* cbs[3] = {cb0, cb1, cb2};
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 3, cbs);
    ctx->VSSetShaderResources(33, 1, &t33Srv);
    ctx->PSSetShader(ps, nullptr, 0);

    ctx->DrawIndexedInstanced(mesh.indexCount, 1u, 0u, 0, 5u);

    ctx->CopyResource(stagingTex, colorTex);
    D3D11_MAPPED_SUBRESOURCE readMap{};
    if (FAILED(ctx->Map(stagingTex, 0, D3D11_MAP_READ, 0, &readMap))) return false;
    outRgba.resize(static_cast<size_t>(width) * height * 4u);
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(outRgba.data() + static_cast<size_t>(y) * width * 4u,
                    static_cast<const uint8_t*>(readMap.pData) + y * readMap.RowPitch,
                    static_cast<size_t>(width) * 4u);
    }
    ctx->Unmap(stagingTex, 0);
    return true;
}

void writePpm(const std::filesystem::path& path, UINT width, UINT height, const std::vector<uint8_t>& rgba) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) return;
    char hdr[64]{};
    const int n = std::snprintf(hdr, sizeof(hdr), "P6\n%u %u\n255\n", width, height);
    out.write(hdr, n);
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3u);
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
}

} // namespace

int main() {
    const std::filesystem::path binPath =
        std::filesystem::path(EDPE_SOURCE_DIR) / "src" / "assets" / "loading_dancer.bin";
    std::ifstream in(binPath, std::ios::binary | std::ios::ate);
    if (!in) {
        std::fprintf(stderr, "FAIL: missing asset %s\n", binPath.string().c_str());
        return 1;
    }
    const auto sz = static_cast<size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> blob(sz);
    if (!in.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(sz))) {
        std::fprintf(stderr, "FAIL: could not read asset\n");
        return 1;
    }

    edpe::ParsedLoadingMesh mesh;
    std::string err;
    if (!edpe::parseLoadingMeshBlob(blob.data(), blob.size(), mesh, &err)) {
        std::fprintf(stderr, "FAIL: parseLoadingMeshBlob: %s\n", err.c_str());
        return 1;
    }
    if (mesh.vertexCount != 45806u || mesh.indexCount != 231834u || mesh.boneCount != 34u || mesh.frameCount != 88u) {
        std::fprintf(stderr, "FAIL: unexpected mesh dims v=%u i=%u b=%u f=%u\n",
            mesh.vertexCount, mesh.indexCount, mesh.boneCount, mesh.frameCount);
        return 1;
    }
    for (const auto& v : mesh.vertices) {
        const unsigned sum = v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3];
        if (sum != 255u) {
            std::fprintf(stderr, "FAIL: vertex weight sum=%u != 255\n", sum);
            return 1;
        }
    }

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (!createDeviceAndContext(dev, ctx)) {
        std::fprintf(stderr, "FAIL: D3D11CreateDevice failed\n");
        return 1;
    }

    ComPtr<ID3DBlob> vsBlob;
    if (!edpe::LoadingMeshReplacer::compileVertexShader(vsBlob.GetAddressOf(), &err)) {
        std::fprintf(stderr, "FAIL: compileVertexShader: %s\n", err.c_str());
        return 1;
    }
    ComPtr<ID3D11VertexShader> vs;
    if (FAILED(dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, vs.GetAddressOf()))) {
        std::fprintf(stderr, "FAIL: CreateVertexShader failed\n");
        return 1;
    }
    UINT ilCount = 0;
    const auto* ilDesc = edpe::LoadingMeshReplacer::getInputElements(&ilCount);
    ComPtr<ID3D11InputLayout> il;
    if (FAILED(dev->CreateInputLayout(ilDesc, ilCount, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), il.GetAddressOf()))) {
        std::fprintf(stderr, "FAIL: CreateInputLayout failed\n");
        return 1;
    }

    static const char kTestPsHlsl[] = R"(
        struct PSInput {
            float3 bitangent : TEXCOORD0;
            float3 viewN     : TEXCOORD1;
            float3 viewT     : TEXCOORD2;
            float2 uv        : TEXCOORD4;
            float4 svPos     : SV_POSITION;
        };
        float4 main(PSInput i) : SV_TARGET {
            float3 n = normalize(i.viewN);
            float rim = pow(saturate(1.0f - abs(n.z)), 1.6f);
            float core = saturate(-n.z) * 0.25f;
            float3 col = float3(1.0f, 0.35f, 0.05f) * (0.25f + 0.85f * rim + core);
            return float4(col, 1.0f);
        }
    )";
    ComPtr<ID3DBlob> psBlob, psErr;
    if (FAILED(D3DCompile(kTestPsHlsl, sizeof(kTestPsHlsl) - 1, nullptr, nullptr, nullptr,
                          "main", "ps_5_0", 0, 0, psBlob.GetAddressOf(), psErr.GetAddressOf()))) {
        std::fprintf(stderr, "FAIL: compile PS failed\n");
        return 1;
    }
    ComPtr<ID3D11PixelShader> ps;
    if (FAILED(dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, ps.GetAddressOf()))) {
        std::fprintf(stderr, "FAIL: CreatePixelShader failed\n");
        return 1;
    }

    // VB0: instance stream (startInstance=5 -> instIdx.x=1)
    uint32_t vb0Data[12]{};
    vb0Data[5 * 2 + 0] = 1u;
    vb0Data[5 * 2 + 1] = 0u;
    D3D11_BUFFER_DESC vb0Desc{sizeof(vb0Data), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA vb0Init{vb0Data, 0, 0};
    ComPtr<ID3D11Buffer> vb0;
    if (FAILED(dev->CreateBuffer(&vb0Desc, &vb0Init, vb0.GetAddressOf()))) return 1;

    D3D11_BUFFER_DESC vb1Desc{mesh.vertexCount * static_cast<UINT>(sizeof(edpe::LoadingMeshVertex)),
                              D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA vb1Init{mesh.vertices.data(), 0, 0};
    ComPtr<ID3D11Buffer> vb1;
    if (FAILED(dev->CreateBuffer(&vb1Desc, &vb1Init, vb1.GetAddressOf()))) return 1;

    D3D11_BUFFER_DESC ibDesc{mesh.indexCount * static_cast<UINT>(sizeof(uint16_t)),
                             D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA ibInit{mesh.indices.data(), 0, 0};
    ComPtr<ID3D11Buffer> ib;
    if (FAILED(dev->CreateBuffer(&ibDesc, &ibInit, ib.GetAddressOf()))) return 1;

    // Exact cb0 from loadingscren.rdc EID 120
    TestCb0 cb0Data{};
    cb0Data.v[4][0] = 1.052385f;
    cb0Data.v[5][1] = 1.870908f;
    cb0Data.v[6][3] = 0.025f;
    cb0Data.v[7][2] = 1.0f;
    cb0Data.v[9][0] = 1.0f;
    cb0Data.v[10][1] = 1.0f;
    cb0Data.v[11][2] = 1.0f;
    D3D11_BUFFER_DESC cb0Desc{sizeof(TestCb0), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA cb0Init{&cb0Data, 0, 0};
    ComPtr<ID3D11Buffer> cb0;
    if (FAILED(dev->CreateBuffer(&cb0Desc, &cb0Init, cb0.GetAddressOf()))) return 1;

    TestCb1 cb1Data{};
    D3D11_BUFFER_DESC cb1Desc{sizeof(TestCb1), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA cb1Init{&cb1Data, 0, 0};
    ComPtr<ID3D11Buffer> cb1;
    if (FAILED(dev->CreateBuffer(&cb1Desc, &cb1Init, cb1.GetAddressOf()))) return 1;

    D3D11_BUFFER_DESC cb2Desc{sizeof(TestCb2), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ComPtr<ID3D11Buffer> cb2;
    if (FAILED(dev->CreateBuffer(&cb2Desc, nullptr, cb2.GetAddressOf()))) return 1;

    // Exact t33[1] from loadingscren.rdc EID 120: scale=0.8, pos=(0, 0.2, 2.0), quat=0xda936e67d6c96da3
    TestInstanceData t33Data[2]{};
    t33Data[1].header[0] = 0u;
    t33Data[1].header[1] = floatAsUint(0.8f);
    t33Data[1].header[2] = 0xd6c96da3u;
    t33Data[1].header[3] = 0xda936e67u;
    t33Data[1].pos[0] = 0.0f;
    t33Data[1].pos[1] = 0.2f;
    t33Data[1].pos[2] = 2.0f;
    D3D11_BUFFER_DESC t33Desc{sizeof(t33Data), D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0,
                              D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(TestInstanceData)};
    D3D11_SUBRESOURCE_DATA t33Init{t33Data, 0, 0};
    ComPtr<ID3D11Buffer> t33Buf;
    if (FAILED(dev->CreateBuffer(&t33Desc, &t33Init, t33Buf.GetAddressOf()))) return 1;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = 2;
    ComPtr<ID3D11ShaderResourceView> t33Srv;
    if (FAILED(dev->CreateShaderResourceView(t33Buf.Get(), &srvDesc, t33Srv.GetAddressOf()))) return 1;

    constexpr UINT W = 640, H = 360;
    D3D11_TEXTURE2D_DESC rtDesc{W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                                D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    ComPtr<ID3D11Texture2D> colorTex;
    if (FAILED(dev->CreateTexture2D(&rtDesc, nullptr, colorTex.GetAddressOf()))) return 1;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(dev->CreateRenderTargetView(colorTex.Get(), nullptr, rtv.GetAddressOf()))) return 1;

    D3D11_TEXTURE2D_DESC stDesc{W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                                D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    ComPtr<ID3D11Texture2D> stagingTex;
    if (FAILED(dev->CreateTexture2D(&stDesc, nullptr, stagingTex.GetAddressOf()))) return 1;

    D3D11_TEXTURE2D_DESC dsDesc{W, H, 1, 1, DXGI_FORMAT_D32_FLOAT, {1, 0},
                                D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL, 0, 0};
    ComPtr<ID3D11Texture2D> depthTex;
    if (FAILED(dev->CreateTexture2D(&dsDesc, nullptr, depthTex.GetAddressOf()))) return 1;
    ComPtr<ID3D11DepthStencilView> dsv;
    if (FAILED(dev->CreateDepthStencilView(depthTex.Get(), nullptr, dsv.GetAddressOf()))) return 1;

    D3D11_RASTERIZER_DESC rsDesc{};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_BACK;
    rsDesc.FrontCounterClockwise = FALSE; // D3D11 default CW front-facing (matches EID 120)
    rsDesc.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rs;
    if (FAILED(dev->CreateRasterizerState(&rsDesc, rs.GetAddressOf()))) return 1;

    D3D11_DEPTH_STENCIL_DESC dssDesc{};
    dssDesc.DepthEnable = TRUE;
    dssDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dssDesc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL; // Reversed-Z
    ComPtr<ID3D11DepthStencilState> dss;
    if (FAILED(dev->CreateDepthStencilState(&dssDesc, dss.GetAddressOf()))) return 1;

    std::vector<uint8_t> frame0, frameMid, frameFront;
    if (!renderFrameAtTime(dev.Get(), ctx.Get(), mesh, il.Get(), vs.Get(), ps.Get(),
                           vb0.Get(), vb1.Get(), ib.Get(), cb0.Get(), cb1.Get(), cb2.Get(),
                           t33Srv.Get(), rtv.Get(), dsv.Get(), rs.Get(), dss.Get(),
                           colorTex.Get(), stagingTex.Get(), W, H, 0.0f, true, frame0)) {
        std::fprintf(stderr, "FAIL: renderFrameAtTime(0.0, spin) failed\n");
        return 1;
    }
    if (!renderFrameAtTime(dev.Get(), ctx.Get(), mesh, il.Get(), vs.Get(), ps.Get(),
                           vb0.Get(), vb1.Get(), ib.Get(), cb0.Get(), cb1.Get(), cb2.Get(),
                           t33Srv.Get(), rtv.Get(), dsv.Get(), rs.Get(), dss.Get(),
                           colorTex.Get(), stagingTex.Get(), W, H, 0.72f, true, frameMid)) {
        std::fprintf(stderr, "FAIL: renderFrameAtTime(0.72, spin) failed\n");
        return 1;
    }
    if (!renderFrameAtTime(dev.Get(), ctx.Get(), mesh, il.Get(), vs.Get(), ps.Get(),
                           vb0.Get(), vb1.Get(), ib.Get(), cb0.Get(), cb1.Get(), cb2.Get(),
                           t33Srv.Get(), rtv.Get(), dsv.Get(), rs.Get(), dss.Get(),
                           colorTex.Get(), stagingTex.Get(), W, H, 0.35f, false, frameFront)) {
        std::fprintf(stderr, "FAIL: renderFrameAtTime(0.35, front) failed\n");
        return 1;
    }

    auto countLit = [&](const std::vector<uint8_t>& buf) {
        size_t n = 0;
        for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) {
            if (buf[i * 4 + 0] > 10 || buf[i * 4 + 1] > 10 || buf[i * 4 + 2] > 10) ++n;
        }
        return n;
    };
    const size_t lit0 = countLit(frame0);
    const size_t litMid = countLit(frameMid);
    const size_t litFront = countLit(frameFront);

    size_t diffPixels = 0;
    for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) {
        const int dr = std::abs(int(frame0[i * 4 + 0]) - int(frameMid[i * 4 + 0]));
        const int dg = std::abs(int(frame0[i * 4 + 1]) - int(frameMid[i * 4 + 1]));
        const int db = std::abs(int(frame0[i * 4 + 2]) - int(frameMid[i * 4 + 2]));
        if (dr + dg + db > 15) ++diffPixels;
    }

    if (std::getenv("EDPE_DUMP_DANCER_PREVIEW")) {
        writePpm(std::filesystem::path(EDPE_SOURCE_DIR) / "build-ninja" / "dancer_preview_spin.ppm", W, H, frame0);
        writePpm(std::filesystem::path(EDPE_SOURCE_DIR) / "build-ninja" / "dancer_preview_front.ppm", W, H, frameFront);
    }

    std::printf("loading_mesh_test: lit0=%zu, litMid=%zu, litFront=%zu, animDiff=%zu\n",
                lit0, litMid, litFront, diffPixels);

    if (lit0 < 1500 || lit0 > 150000 || litMid < 1500 || litFront < 1500) {
        std::fprintf(stderr, "FAIL: unexpected lit pixel count\n");
        return 1;
    }
    if (diffPixels < 500) {
        std::fprintf(stderr, "FAIL: skeletal animation did not deform mesh across frames\n");
        return 1;
    }

    std::printf("PASS: loading_mesh_test\n");
    return 0;
}
