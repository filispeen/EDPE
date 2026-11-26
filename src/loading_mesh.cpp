#include "loading_mesh.h"
#include "loading_mesh_res.h"
#include "log.h"
#include "shader_probe.h"

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace edpe {
namespace {

struct alignas(16) LoadingMeshCb2 {
    float params[4];                       // x = useGameSpin
    float bones[kMaxLoadingMeshBones * 12]; // 48 bones * 3 float4 rows
};
static_assert(sizeof(LoadingMeshCb2) == (1 + kMaxLoadingMeshBones * 3) * 16,
              "LoadingMeshCb2 size mismatch");

const char kLoadingMeshVsHlsl[] = R"(
cbuffer Cb0 : register(b0) {
    float4 gCb0[12];
};
cbuffer Cb1 : register(b1) {
    float4 gCb1[276];
};
cbuffer Cb2 : register(b2) {
    float4 gParams;
    float4 gBones[144];
};

struct InstanceData {
    uint4 header;
    float3 pos;
    uint pad[77];
};
StructuredBuffer<InstanceData> gInstances : register(t33);

struct VSInput {
    uint2  instIdx : INSTANCEANDMODELDATAINDEX;
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float4 tangent : TANGENT;
    float2 uv      : TEXCOORD0;
    uint4  bones   : BLENDINDICES;
    float4 weights : BLENDWEIGHT;
};

struct VSOutput {
    float3 bitangent : TEXCOORD0;
    float3 viewN     : TEXCOORD1;
    float3 viewT     : TEXCOORD2;
    float2 uv        : TEXCOORD4;
    float4 svPos     : SV_POSITION;
};

float4 unpackQuat(uint2 zw) {
    float4 q;
    q.x = float(zw.x & 0xFFFFu) * (2.0f / 65535.0f) - 1.0f;
    q.y = float(zw.x >> 16u)     * (2.0f / 65535.0f) - 1.0f;
    q.z = float(zw.y & 0xFFFFu) * (2.0f / 65535.0f) - 1.0f;
    q.w = float(zw.y >> 16u)     * (2.0f / 65535.0f) - 1.0f;
    float lenSq = dot(q, q);
    return (lenSq > 1e-8f) ? (q * rsqrt(lenSq)) : float4(0.0f, 0.0f, 0.0f, 1.0f);
}

float3 quatRotate(float4 q, float3 v) {
    float3 t = 2.0f * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

VSOutput main(VSInput input) {
    float4 r0 = 0.0f;
    float4 r1 = 0.0f;
    float4 r2 = 0.0f;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        uint b = min(input.bones[i], 47u) * 3u;
        float w = input.weights[i];
        r0 += gBones[b + 0u] * w;
        r1 += gBones[b + 1u] * w;
        r2 += gBones[b + 2u] * w;
    }

    float4 p4 = float4(input.pos, 1.0f);
    float3 localPos = float3(dot(r0, p4), dot(r1, p4), dot(r2, p4));
    float3 localN   = float3(dot(r0.xyz, input.normal), dot(r1.xyz, input.normal), dot(r2.xyz, input.normal));
    float3 localT   = float3(dot(r0.xyz, input.tangent.xyz), dot(r1.xyz, input.tangent.xyz), dot(r2.xyz, input.tangent.xyz));

    float nLenSq = dot(localN, localN);
    localN = (nLenSq > 1e-12f) ? (localN * rsqrt(nLenSq)) : float3(0.0f, 1.0f, 0.0f);
    float tLenSq = dot(localT, localT);
    localT = (tLenSq > 1e-12f) ? (localT * rsqrt(tLenSq)) : float3(1.0f, 0.0f, 0.0f);

    InstanceData inst = gInstances[input.instIdx.x];
    // Facing-viewer fallback: Ry(180 deg) followed by Rx(-23 deg) -> q = (0, cos(11.5 deg), sin(11.5 deg), 0)
    float4 q = (gParams.x > 0.5f) ? unpackQuat(inst.header.zw) : float4(0.0f, 0.9799247f, 0.1993679f, 0.0f);
    float scale = asfloat(inst.header.y);
    float3 instPos = inst.pos - gCb1[275].xyz;

    float3 worldPos = quatRotate(q, localPos) * scale + instPos;
    float3 worldN   = normalize(quatRotate(q, localN));
    float3 worldT   = normalize(quatRotate(q, localT));

    VSOutput o;
    float4 wp4 = float4(worldPos, 1.0f);
    o.svPos = float4(
        dot(gCb0[4], wp4),
        dot(gCb0[5], wp4),
        dot(gCb0[6], wp4),
        dot(gCb0[7], wp4)
    );
    o.viewN = float3(
        dot(worldN, gCb0[9].xyz),
        dot(worldN, gCb0[10].xyz),
        dot(worldN, gCb0[11].xyz)
    );
    o.viewT = float3(
        dot(worldT, gCb0[9].xyz),
        dot(worldT, gCb0[10].xyz),
        dot(worldT, gCb0[11].xyz)
    );
    o.bitangent = cross(worldN, worldT) * input.tangent.w;
    o.uv = input.uv;
    return o;
}
)";

const D3D11_INPUT_ELEMENT_DESC kLoadingMeshInputElements[] = {
    {"INSTANCEANDMODELDATAINDEX", 0, DXGI_FORMAT_R32G32_UINT,      0,  0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    {"POSITION",                  0, DXGI_FORMAT_R32G32B32_FLOAT,    1,  0, D3D11_INPUT_PER_VERTEX_DATA,   0},
    {"NORMAL",                    0, DXGI_FORMAT_R32G32B32_FLOAT,    1, 12, D3D11_INPUT_PER_VERTEX_DATA,   0},
    {"TANGENT",                   0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 24, D3D11_INPUT_PER_VERTEX_DATA,   0},
    {"TEXCOORD",                  0, DXGI_FORMAT_R32G32_FLOAT,       1, 40, D3D11_INPUT_PER_VERTEX_DATA,   0},
    {"BLENDINDICES",              0, DXGI_FORMAT_R8G8B8A8_UINT,      1, 48, D3D11_INPUT_PER_VERTEX_DATA,   0},
    {"BLENDWEIGHT",               0, DXGI_FORMAT_R8G8B8A8_UNORM,     1, 52, D3D11_INPUT_PER_VERTEX_DATA,   0},
};

bool hasPrivateTag(ID3D11DeviceChild* child, const GUID& guid) {
    if (!child) return false;
    uint8_t tag = 0;
    UINT sz = sizeof(tag);
    return SUCCEEDED(child->GetPrivateData(guid, &sz, &tag)) && sz == sizeof(tag) && tag == 1u;
}

bool querySrv2dDims(ID3D11ShaderResourceView* srv, UINT& outW, UINT& outH) {
    if (!srv) return false;
    Microsoft::WRL::ComPtr<ID3D11Resource> res;
    srv->GetResource(res.GetAddressOf());
    if (!res) return false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res.As(&tex)) || !tex) return false;
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    outW = desc.Width;
    outH = desc.Height;
    return true;
}

HMODULE getOwnModule() {
    HMODULE mod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&getOwnModule),
        &mod);
    return mod;
}

} // namespace

bool parseLoadingMeshBlob(const uint8_t* data, size_t size, ParsedLoadingMesh& out, std::string* err) {
    auto fail = [&](const char* msg) {
        if (err) *err = msg;
        return false;
    };
    if (!data || size < sizeof(LoadingMeshHeader)) {
        return fail("Blob smaller than LoadingMeshHeader");
    }
    LoadingMeshHeader hdr{};
    std::memcpy(&hdr, data, sizeof(hdr));
    if (hdr.magic != kLoadingMeshMagic) return fail("Invalid EDPM magic");
    if (hdr.version != kLoadingMeshVersion) return fail("Unsupported EDPM version");
    if (hdr.vertexCount == 0 || hdr.vertexCount > 65535u) return fail("Invalid vertexCount");
    if (hdr.indexCount == 0 || (hdr.indexCount % 3u) != 0u) return fail("Invalid indexCount");
    if (hdr.boneCount == 0 || hdr.boneCount > kMaxLoadingMeshBones) return fail("Invalid boneCount");
    if (hdr.frameCount == 0) return fail("Invalid frameCount");
    if (!std::isfinite(hdr.durationSec) || hdr.durationSec <= 0.0f) return fail("Invalid durationSec");

    const size_t vertsBytes = static_cast<size_t>(hdr.vertexCount) * sizeof(LoadingMeshVertex);
    const size_t rawIdxBytes = static_cast<size_t>(hdr.indexCount) * sizeof(uint16_t);
    const size_t idxBytesPadded = (rawIdxBytes + 3u) & ~size_t{3u};
    const size_t boneFloatCount = static_cast<size_t>(hdr.frameCount) * static_cast<size_t>(hdr.boneCount) * 12u;
    const size_t boneBytes = boneFloatCount * sizeof(float);
    const size_t expectedSize = sizeof(LoadingMeshHeader) + vertsBytes + idxBytesPadded + boneBytes;
    if (size < expectedSize) return fail("Truncated EDPM blob");

    out.vertexCount = hdr.vertexCount;
    out.indexCount = hdr.indexCount;
    out.boneCount = hdr.boneCount;
    out.frameCount = hdr.frameCount;
    out.durationSec = hdr.durationSec;

    size_t offset = sizeof(LoadingMeshHeader);
    out.vertices.resize(hdr.vertexCount);
    std::memcpy(out.vertices.data(), data + offset, vertsBytes);
    offset += vertsBytes;

    out.indices.resize(hdr.indexCount);
    std::memcpy(out.indices.data(), data + offset, rawIdxBytes);
    offset += idxBytesPadded;

    out.boneFrames.resize(boneFloatCount);
    std::memcpy(out.boneFrames.data(), data + offset, boneBytes);

    for (uint16_t idx : out.indices) {
        if (idx >= hdr.vertexCount) return fail("Index out of vertex bounds");
    }
    for (const auto& v : out.vertices) {
        for (int k = 0; k < 4; ++k) {
            if (v.weights[k] > 0 && v.bones[k] >= hdr.boneCount) {
                return fail("Bone index out of bounds");
            }
        }
    }
    return true;
}

void sampleLoadingMeshBones(const ParsedLoadingMesh& mesh, float timeSec,
                            float outMatrices[kMaxLoadingMeshBones * 12]) {
    for (uint32_t b = 0; b < kMaxLoadingMeshBones; ++b) {
        float* dst = outMatrices + b * 12;
        std::memset(dst, 0, sizeof(float) * 12);
        dst[0] = 1.0f;
        dst[5] = 1.0f;
        dst[10] = 1.0f;
    }
    if (mesh.frameCount == 0 || mesh.boneCount == 0 || mesh.boneFrames.empty()) return;

    const uint32_t stride = mesh.boneCount * 12u;
    if (mesh.frameCount == 1u || mesh.durationSec <= 0.0f) {
        std::memcpy(outMatrices, mesh.boneFrames.data(), sizeof(float) * stride);
        return;
    }

    float wrapped = std::fmod(timeSec, mesh.durationSec);
    if (wrapped < 0.0f) wrapped += mesh.durationSec;
    const float f = (wrapped / mesh.durationSec) * static_cast<float>(mesh.frameCount - 1u);
    const uint32_t f0 = std::min(static_cast<uint32_t>(f), mesh.frameCount - 1u);
    const uint32_t f1 = std::min(f0 + 1u, mesh.frameCount - 1u);
    const float alpha = std::clamp(f - static_cast<float>(f0), 0.0f, 1.0f);
    const float invAlpha = 1.0f - alpha;

    const float* m0 = mesh.boneFrames.data() + static_cast<size_t>(f0) * stride;
    const float* m1 = mesh.boneFrames.data() + static_cast<size_t>(f1) * stride;
    for (uint32_t i = 0; i < stride; ++i) {
        outMatrices[i] = m0[i] * invAlpha + m1[i] * alpha;
    }
}

LoadingMeshReplacer& LoadingMeshReplacer::instance() {
    static LoadingMeshReplacer s_inst;
    return s_inst;
}

const char* LoadingMeshReplacer::getVertexShaderHlsl() {
    return kLoadingMeshVsHlsl;
}

bool LoadingMeshReplacer::compileVertexShader(ID3DBlob** outBytecode, std::string* err) {
    if (!outBytecode) return false;
    *outBytecode = nullptr;
    Microsoft::WRL::ComPtr<ID3DBlob> errBlob;
    const HRESULT hr = D3DCompile(
        kLoadingMeshVsHlsl,
        sizeof(kLoadingMeshVsHlsl) - 1,
        "loading_mesh_vs.hlsl",
        nullptr,
        nullptr,
        "main",
        "vs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        outBytecode,
        errBlob.GetAddressOf());
    if (FAILED(hr)) {
        if (err && errBlob) {
            err->assign(static_cast<const char*>(errBlob->GetBufferPointer()), errBlob->GetBufferSize());
        }
        return false;
    }
    return true;
}

const D3D11_INPUT_ELEMENT_DESC* LoadingMeshReplacer::getInputElements(UINT* outCount) {
    if (outCount) *outCount = static_cast<UINT>(sizeof(kLoadingMeshInputElements) / sizeof(kLoadingMeshInputElements[0]));
    return kLoadingMeshInputElements;
}

bool LoadingMeshReplacer::loadMeshAsset() {
    if (meshParsed_) return true;

    std::string parseErr;
    HMODULE selfMod = getOwnModule();
    if (selfMod) {
        wchar_t modPath[MAX_PATH]{};
        const DWORD len = GetModuleFileNameW(selfMod, modPath, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            const std::filesystem::path overridePath =
                std::filesystem::path(modPath).parent_path() / L"edpe_loading_mesh.bin";
            std::ifstream in(overridePath, std::ios::binary | std::ios::ate);
            if (in) {
                const auto fileSize = static_cast<size_t>(in.tellg());
                in.seekg(0, std::ios::beg);
                std::vector<uint8_t> buf(fileSize);
                if (in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(fileSize))) {
                    if (parseLoadingMeshBlob(buf.data(), buf.size(), mesh_, &parseErr)) {
                        meshParsed_ = true;
                        assetLoaded_.store(true, std::memory_order_relaxed);
                        EdpeLog(L"EDPE: LoadingMeshReplacer loaded external override edpe_loading_mesh.bin");
                        return true;
                    }
                    EdpeLog(L"EDPE: LoadingMeshReplacer external override invalid; falling back to embedded asset");
                }
            }
        }
    }

#if defined(EDPE_HAVE_EMBEDDED_LOADING_MESH) && EDPE_HAVE_EMBEDDED_LOADING_MESH
    if (selfMod) {
        HRSRC resInfo = FindResourceW(selfMod, MAKEINTRESOURCEW(IDR_EDPE_LOADING_DANCER), MAKEINTRESOURCEW(10));
        if (resInfo) {
            HGLOBAL resData = LoadResource(selfMod, resInfo);
            const DWORD resSize = SizeofResource(selfMod, resInfo);
            const void* ptr = resData ? LockResource(resData) : nullptr;
            if (ptr && resSize > 0) {
                if (parseLoadingMeshBlob(static_cast<const uint8_t*>(ptr), resSize, mesh_, &parseErr)) {
                    meshParsed_ = true;
                    assetLoaded_.store(true, std::memory_order_relaxed);
                    wchar_t msg[192];
                    swprintf_s(msg, L"EDPE: LoadingMeshReplacer loaded embedded RCDATA mesh (verts=%u tris=%u bones=%u frames=%u)",
                        mesh_.vertexCount, mesh_.indexCount / 3u, mesh_.boneCount, mesh_.frameCount);
                    EdpeLog(msg);
                    return true;
                }
            }
        }
    }
#endif

    EdpeLog(L"EDPE: LoadingMeshReplacer unavailable (no valid EDPM asset found)");
    return false;
}

bool LoadingMeshReplacer::ensureInitialized(ID3D11Device* device) {
    if (!device) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_ == device) {
        if (ready_) return true;
        if (failed_) return false;
    } else {
        vb_.Reset();
        ib_.Reset();
        cb2_.Reset();
        il_.Reset();
        vs_.Reset();
        device_ = device;
        ready_ = false;
        failed_ = false;
    }

    if (!loadMeshAsset()) {
        failed_ = true;
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    std::string compileErr;
    if (!compileVertexShader(vsBlob.GetAddressOf(), &compileErr)) {
        EdpeLog(L"EDPE: LoadingMeshReplacer VS compile failed");
        failed_ = true;
        return false;
    }

    if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                          nullptr, vs_.GetAddressOf()))) {
        EdpeLog(L"EDPE: LoadingMeshReplacer CreateVertexShader failed");
        failed_ = true;
        return false;
    }

    UINT ilCount = 0;
    const D3D11_INPUT_ELEMENT_DESC* ilDesc = getInputElements(&ilCount);
    if (FAILED(device->CreateInputLayout(ilDesc, ilCount,
                                         vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                         il_.GetAddressOf()))) {
        EdpeLog(L"EDPE: LoadingMeshReplacer CreateInputLayout failed");
        failed_ = true;
        return false;
    }

    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = mesh_.vertexCount * static_cast<UINT>(sizeof(LoadingMeshVertex));
    vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbInit{mesh_.vertices.data(), 0, 0};
    if (FAILED(device->CreateBuffer(&vbDesc, &vbInit, vb_.GetAddressOf()))) {
        EdpeLog(L"EDPE: LoadingMeshReplacer CreateBuffer(VB) failed");
        failed_ = true;
        return false;
    }

    D3D11_BUFFER_DESC ibDesc{};
    ibDesc.ByteWidth = mesh_.indexCount * static_cast<UINT>(sizeof(uint16_t));
    ibDesc.Usage = D3D11_USAGE_IMMUTABLE;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibInit{mesh_.indices.data(), 0, 0};
    if (FAILED(device->CreateBuffer(&ibDesc, &ibInit, ib_.GetAddressOf()))) {
        EdpeLog(L"EDPE: LoadingMeshReplacer CreateBuffer(IB) failed");
        failed_ = true;
        return false;
    }

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(LoadingMeshCb2);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cbDesc, nullptr, cb2_.GetAddressOf()))) {
        EdpeLog(L"EDPE: LoadingMeshReplacer CreateBuffer(CB2) failed");
        failed_ = true;
        return false;
    }

    ready_ = true;
    return true;
}

float LoadingMeshReplacer::currentElapsedSeconds() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (qpcFreq_ == 0) {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        qpcFreq_ = (freq.QuadPart > 0) ? freq.QuadPart : 1;
        qpcStart_ = now.QuadPart;
    }
    return static_cast<float>(static_cast<double>(now.QuadPart - qpcStart_) /
                              static_cast<double>(qpcFreq_));
}

bool LoadingMeshReplacer::tryReplaceDrawIndexedInstanced(
    ID3D11DeviceContext* context,
    UINT indexCount,
    UINT instanceCount,
    UINT /*startIndex*/,
    INT /*baseVertex*/,
    UINT startInstance,
    DrawIndexedInstancedFn origDraw) {
    if (!context || !origDraw) return false;
    if (!enabled_.load(std::memory_order_relaxed)) return false;
    if (instanceCount != 1u || indexCount < 1000u) return false;
    const bool allHolo = replaceAllHolograms_.load(std::memory_order_relaxed);
    if (!allHolo && indexCount != kLoadingScreenHoloIndexCount) return false;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> boundVs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> boundPs;
    context->VSGetShader(boundVs.GetAddressOf(), nullptr, nullptr);
    context->PSGetShader(boundPs.GetAddressOf(), nullptr, nullptr);
    const bool vsOk = hasPrivateTag(boundVs.Get(), kEdpeLoadingHoloVsGuid);
    const bool psOk = hasPrivateTag(boundPs.Get(), kEdpeLoadingHoloPsGuid);
    if (!vsOk || !psOk) {
        if (indexCount == kLoadingScreenHoloIndexCount &&
            !loggedMismatch_.exchange(true, std::memory_order_relaxed)) {
            uint64_t vsHash = 0, psHash = 0;
            UINT sz = sizeof(uint64_t);
            if (boundVs) boundVs->GetPrivateData(kEdpeShaderHashGuid, &sz, &vsHash);
            sz = sizeof(uint64_t);
            if (boundPs) boundPs->GetPrivateData(kEdpeShaderHashGuid, &sz, &psHash);
            wchar_t msg[192];
            swprintf_s(msg, L"EDPE: LoadingMeshReplacer indexCount=231870 shader tag mismatch (vsOk=%u vsHash=0x%016llX psOk=%u psHash=0x%016llX)",
                static_cast<unsigned>(vsOk), static_cast<unsigned long long>(vsHash),
                static_cast<unsigned>(psOk), static_cast<unsigned long long>(psHash));
            EdpeLog(msg);
        }
        return false;
    }

    ID3D11ShaderResourceView* psSrvs[3]{};
    context->PSGetShaderResources(0, 3, psSrvs);
    UINT w0 = 0, h0 = 0, w1 = 0, h1 = 0, w2 = 0, h2 = 0;
    const bool srvOk =
        querySrv2dDims(psSrvs[0], w0, h0) && w0 <= 128u && h0 <= 128u &&
        querySrv2dDims(psSrvs[1], w1, h1) && w1 == 1u && h1 == 1u &&
        querySrv2dDims(psSrvs[2], w2, h2) && w2 == 1u && h2 == 1u;
    for (auto*& s : psSrvs) {
        if (s) { s->Release(); s = nullptr; }
    }
    if (!srvOk) return false;

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(device.GetAddressOf());
    if (!ensureInitialized(device.Get())) return false;

    LoadingMeshCb2 cbData{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_) return false;
        cbData.params[0] = useGameSpin_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        const float speed = std::clamp(animSpeed_.load(std::memory_order_relaxed), 0.1f, 5.0f);
        const float t = currentElapsedSeconds() * speed;
        sampleLoadingMeshBones(mesh_, t, cbData.bones);
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(cb2_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, &cbData, sizeof(cbData));
    context->Unmap(cb2_.Get(), 0);

    // Save only the IA/VS slots we override; VB slot 0, VS cb0/cb1, t33, PS, OM, RS stay intact.
    Microsoft::WRL::ComPtr<ID3D11InputLayout> prevIl;
    Microsoft::WRL::ComPtr<ID3D11Buffer> prevVb1;
    UINT prevVb1Stride = 0, prevVb1Offset = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> prevIb;
    DXGI_FORMAT prevIbFmt = DXGI_FORMAT_UNKNOWN;
    UINT prevIbOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY prevTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> prevVs;
    ID3D11ClassInstance* prevVsClass[256]{};
    UINT prevVsClassCount = 256;
    Microsoft::WRL::ComPtr<ID3D11Buffer> prevCb2;

    context->IAGetInputLayout(prevIl.GetAddressOf());
    context->IAGetVertexBuffers(1, 1, prevVb1.GetAddressOf(), &prevVb1Stride, &prevVb1Offset);
    context->IAGetIndexBuffer(prevIb.GetAddressOf(), &prevIbFmt, &prevIbOffset);
    context->IAGetPrimitiveTopology(&prevTopo);
    context->VSGetShader(prevVs.GetAddressOf(), prevVsClass, &prevVsClassCount);
    context->VSGetConstantBuffers(2, 1, prevCb2.GetAddressOf());

    const UINT vbStride = sizeof(LoadingMeshVertex);
    const UINT vbOffset = 0;
    ID3D11Buffer* vb1Ptr = vb_.Get();
    ID3D11Buffer* cb2Ptr = cb2_.Get();

    context->IASetInputLayout(il_.Get());
    context->IASetVertexBuffers(1, 1, &vb1Ptr, &vbStride, &vbOffset);
    context->IASetIndexBuffer(ib_.Get(), DXGI_FORMAT_R16_UINT, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs_.Get(), nullptr, 0);
    context->VSSetConstantBuffers(2, 1, &cb2Ptr);

    origDraw(context, mesh_.indexCount, instanceCount, 0u, 0, startInstance);

    ID3D11Buffer* prevVb1Ptr = prevVb1.Get();
    ID3D11Buffer* prevCb2Ptr = prevCb2.Get();
    context->IASetInputLayout(prevIl.Get());
    context->IASetVertexBuffers(1, 1, &prevVb1Ptr, &prevVb1Stride, &prevVb1Offset);
    context->IASetIndexBuffer(prevIb.Get(), prevIbFmt, prevIbOffset);
    context->IASetPrimitiveTopology(prevTopo);
    context->VSSetShader(prevVs.Get(), prevVsClassCount ? prevVsClass : nullptr, prevVsClassCount);
    context->VSSetConstantBuffers(2, 1, &prevCb2Ptr);
    for (UINT i = 0; i < prevVsClassCount; ++i) {
        if (prevVsClass[i]) prevVsClass[i]->Release();
    }

    lastMatchedIndexCount_.store(indexCount, std::memory_order_relaxed);
    const uint64_t count = replacedDrawCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1) {
        wchar_t msg[160];
        swprintf_s(msg, L"EDPE: LoadingMeshReplacer replaced first hologram draw (origIndices=%u -> dancerIndices=%u)",
            indexCount, mesh_.indexCount);
        EdpeLog(msg);
    }
    return true;
}

void LoadingMeshReplacer::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    vb_.Reset();
    ib_.Reset();
    cb2_.Reset();
    il_.Reset();
    vs_.Reset();
    device_ = nullptr;
    ready_ = false;
    failed_ = false;
}

} // namespace edpe
