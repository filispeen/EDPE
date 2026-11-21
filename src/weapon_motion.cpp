#include "weapon_motion.h"
#include "weapon_motion_shader.h"
#include "renderdoc_support.h"
#include "shader_probe.h"

#include <d3dcompiler.h>
#include <d3d11shader.h>

namespace edpe {

bool WeaponMotionTracker::initialize(ID3D11Device* device) {
    if (!device || device_) return false;

    D3D11_BUFFER_DESC soDesc{};
    soDesc.ByteWidth = kWeaponSoPoolBytes;
    soDesc.Usage = D3D11_USAGE_DEFAULT;
    soDesc.BindFlags = D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_VERTEX_BUFFER;
    for (int i = 0; i < 2; ++i) {
        if (FAILED(device->CreateBuffer(&soDesc, nullptr, &soBuffers_[i]))) {
            shutdown();
            return false;
        }
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob;
    if (FAILED(D3DCompile(kWeaponMotionRasterShader, sizeof(kWeaponMotionRasterShader) - 1,
            nullptr, nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vsBlob, nullptr)) ||
        FAILED(D3DCompile(kWeaponMotionRasterShader, sizeof(kWeaponMotionRasterShader) - 1,
            nullptr, nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &psBlob, nullptr)) ||
        FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
            nullptr, &rasterVs_)) ||
        FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
            nullptr, &rasterPs_))) {
        shutdown();
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"POSITION", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(), &inputLayout_))) {
        shutdown();
        return false;
    }

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = 8 * sizeof(float);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &constants_))) {
        shutdown();
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dsDesc{};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL; // Reversed-Z
    if (FAILED(device->CreateDepthStencilState(&dsDesc, &depthStencilState_))) {
        shutdown();
        return false;
    }

    D3D11_RASTERIZER_DESC rsDesc{};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rsDesc, &rasterizerState_))) {
        shutdown();
        return false;
    }

    D3D11_BLEND_DESC bsDesc{};
    bsDesc.RenderTarget[0].BlendEnable = FALSE;
    bsDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bsDesc, &blendState_))) {
        shutdown();
        return false;
    }

    device_ = device;
    return true;
}

void WeaponMotionTracker::shutdown() {
    soBuffers_[0].Reset();
    soBuffers_[1].Reset();
    rasterVs_.Reset();
    rasterPs_.Reset();
    inputLayout_.Reset();
    constants_.Reset();
    depthStencilState_.Reset();
    rasterizerState_.Reset();
    blendState_.Reset();
    motionTex_.Reset();
    motionRtv_.Reset();
    motionSrv_.Reset();
    depthTex_.Reset();
    depthDsv_.Reset();
    soGsCache_.clear();
    device_.Reset();
    draws_[0].clear();
    draws_[1].clear();
    width_ = height_ = 0;
    writeCursorBytes_ = 0;
    hasResolvedMotion_ = false;
    matchedDraws_ = 0;
}

bool WeaponMotionTracker::isFirstPersonWeaponState(ID3D11DeviceContext* context) {
    if (!context) return false;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> ds;
    UINT stencilRef = 0;
    context->OMGetDepthStencilState(&ds, &stencilRef);
    if (!ds || (stencilRef & kWeaponStencilBit) == 0u) return false;
    D3D11_DEPTH_STENCIL_DESC desc{};
    ds->GetDesc(&desc);
    return desc.StencilEnable && desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
}

bool WeaponMotionTracker::ensureTargets(UINT width, UINT height) {
    if (motionTex_ && width == width_ && height == height_) return true;
    motionTex_.Reset();
    motionRtv_.Reset();
    motionSrv_.Reset();
    depthTex_.Reset();
    depthDsv_.Reset();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &motionTex_)) ||
        FAILED(device_->CreateRenderTargetView(motionTex_.Get(), nullptr, &motionRtv_)) ||
        FAILED(device_->CreateShaderResourceView(motionTex_.Get(), nullptr, &motionSrv_))) {
        return false;
    }
    SetD3D11DebugName(motionTex_.Get(), "EDPE_WeaponMotion");

    D3D11_TEXTURE2D_DESC dd = td;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device_->CreateTexture2D(&dd, nullptr, &depthTex_)) ||
        FAILED(device_->CreateDepthStencilView(depthTex_.Get(), nullptr, &depthDsv_))) {
        return false;
    }
    width_ = width;
    height_ = height;
    return true;
}

void WeaponMotionTracker::beginFrame(ID3D11DeviceContext* context, UINT width, UINT height,
                                     float worldNear, float weaponNear) {
    (void)context;
    curSlot_ ^= 1u;
    draws_[curSlot_].clear();
    writeCursorBytes_ = 0;
    worldNear_ = worldNear;
    weaponNear_ = weaponNear;
    hasResolvedMotion_ = false;
    matchedDraws_ = 0;
    if (device_ && width > 0 && height > 0) {
        ensureTargets(width, height);
    }
}

bool WeaponMotionTracker::recordCapturedDraw(const WeaponDrawIdentity& identity,
                                             uint32_t vertexCount,
                                             uint32_t* outByteOffset) {
    if (!vertexCount) return false;
    const uint32_t bytes = vertexCount * 16u; // float4 clipPos
    if (bytes > kWeaponSoPoolBytes || writeCursorBytes_ > kWeaponSoPoolBytes - bytes) {
        return false;
    }
    const uint32_t offset = writeCursorBytes_;
    writeCursorBytes_ += bytes;
    draws_[curSlot_].push_back({identity, offset, vertexCount});
    if (outByteOffset) *outByteOffset = offset;
    return true;
}

ID3D11GeometryShader* WeaponMotionTracker::getOrCreateStreamOutGs(
    ID3D11VertexShader* vs, uint64_t vsHash) {
    if (!device_ || !vs) return nullptr;
    auto it = soGsCache_.find(vsHash);
    if (it != soGsCache_.end()) {
        return it->second.Get();
    }

    Microsoft::WRL::ComPtr<ID3D11GeometryShader> soGs;
    UINT bcSize = 0;
    if (SUCCEEDED(vs->GetPrivateData(kEdpeVertexBytecodeGuid, &bcSize, nullptr)) &&
        bcSize > 0 && bcSize <= 65536u) {
        std::vector<uint8_t> bytecode(bcSize);
        if (SUCCEEDED(vs->GetPrivateData(kEdpeVertexBytecodeGuid, &bcSize, bytecode.data()))) {
            const char* semName = "SV_Position";
            UINT semIndex = 0;
            std::string reflectedName;
            Microsoft::WRL::ComPtr<ID3D11ShaderReflection> refl;
            if (SUCCEEDED(D3DReflect(bytecode.data(), bytecode.size(),
                    __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(refl.GetAddressOf()))) &&
                refl) {
                D3D11_SHADER_DESC sd{};
                if (SUCCEEDED(refl->GetDesc(&sd))) {
                    for (UINT i = 0; i < sd.OutputParameters; ++i) {
                        D3D11_SIGNATURE_PARAMETER_DESC pd{};
                        if (SUCCEEDED(refl->GetOutputParameterDesc(i, &pd)) &&
                            pd.SystemValueType == D3D_NAME_POSITION && pd.SemanticName) {
                            reflectedName = pd.SemanticName;
                            semName = reflectedName.c_str();
                            semIndex = pd.SemanticIndex;
                            break;
                        }
                    }
                }
            }

            const UINT stride = 16u;
            D3D11_SO_DECLARATION_ENTRY decl{};
            decl.Stream = 0;
            decl.SemanticName = semName;
            decl.SemanticIndex = semIndex;
            decl.StartComponent = 0;
            decl.ComponentCount = 4;
            decl.OutputSlot = 0;
            if (FAILED(device_->CreateGeometryShaderWithStreamOutput(
                    bytecode.data(), bytecode.size(), &decl, 1u, &stride, 1u,
                    D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &soGs))) {
                decl.SemanticName = "SV_POSITION";
                decl.SemanticIndex = 0;
                device_->CreateGeometryShaderWithStreamOutput(
                    bytecode.data(), bytecode.size(), &decl, 1u, &stride, 1u,
                    D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &soGs);
            }
        }
    }

    soGsCache_.emplace(vsHash, soGs);
    return soGs.Get();
}

bool WeaponMotionTracker::captureDrawIndexedInstanced(
    ID3D11DeviceContext* context,
    UINT indexCount,
    UINT instanceCount,
    UINT startIndex,
    INT baseVertex,
    UINT startInstance,
    DrawIndexedInstancedFn origDraw) {
    if (!device_ || !context || !origDraw || !soBuffers_[curSlot_]) return false;
    if (instanceCount != 1u || indexCount < 3u || indexCount > 250000u) return false;

    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> ds;
    UINT stencilRef = 0;
    context->OMGetDepthStencilState(&ds, &stencilRef);
    if (!ds || (stencilRef & kWeaponStencilBit) == 0u) return false;
    D3D11_DEPTH_STENCIL_DESC dsDesc{};
    ds->GetDesc(&dsDesc);
    if (!dsDesc.StencilEnable || (dsDesc.StencilWriteMask & kWeaponStencilBit) == 0u) return false;

    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    context->IAGetPrimitiveTopology(&topo);
    if (topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) return false;

    Microsoft::WRL::ComPtr<ID3D11GeometryShader> origGs;
    context->GSGetShader(&origGs, nullptr, nullptr);
    if (origGs) return false;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    context->VSGetShader(&vs, nullptr, nullptr);
    if (!vs) return false;

    uint64_t vsHash = 0;
    UINT hashSize = sizeof(vsHash);
    if (FAILED(vs->GetPrivateData(kEdpeShaderHashGuid, &hashSize, &vsHash)) ||
        hashSize != sizeof(vsHash)) {
        return false;
    }

    ID3D11GeometryShader* soGs = getOrCreateStreamOutGs(vs.Get(), vsHash);
    if (!soGs) return false;

    WeaponDrawIdentity id{};
    id.indexCount = indexCount;
    id.instanceCount = instanceCount;
    id.boneBase = (startIndex & 0xFFFFu) | ((static_cast<uint32_t>(baseVertex) & 0xFFFFu) << 16);
    id.signature = static_cast<uint32_t>(vsHash ^ (vsHash >> 32)) & kWeaponSignatureMask;

    uint32_t byteOffset = 0;
    if (!recordCapturedDraw(id, indexCount, &byteOffset)) return false;

    Microsoft::WRL::ComPtr<ID3D11PixelShader> origPs;
    context->PSGetShader(&origPs, nullptr, nullptr);

    ID3D11Buffer* soBuf = soBuffers_[curSlot_].Get();
    UINT soOff = byteOffset;
    context->PSSetShader(nullptr, nullptr, 0);
    context->GSSetShader(soGs, nullptr, 0);
    context->SOSetTargets(1u, &soBuf, &soOff);

    origDraw(context, indexCount, 1u, startIndex, baseVertex, startInstance);

    ID3D11Buffer* nullSo = nullptr;
    UINT zeroOff = 0;
    context->SOSetTargets(1u, &nullSo, &zeroOff);
    context->GSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(origPs.Get(), nullptr, 0);
    return true;
}

bool WeaponMotionTracker::resolve(ID3D11DeviceContext* context,
                                  float jitterNowX, float jitterNowY,
                                  float jitterPrevX, float jitterPrevY,
                                  bool viewmodelJittered) {
    hasResolvedMotion_ = false;
    matchedDraws_ = 0;
    if (!device_ || !context || !motionRtv_ || !depthDsv_) return false;

    const auto& curDraws = draws_[curSlot_];
    const auto& prevDraws = draws_[curSlot_ ^ 1u];
    if (curDraws.empty() || prevDraws.empty()) return false;

    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context->ClearRenderTargetView(motionRtv_.Get(), clearColor);
    context->ClearDepthStencilView(depthDsv_.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);

    const float cb[8] = {
        static_cast<float>(width_),
        static_cast<float>(height_),
        worldNear_,
        weaponNear_,
        jitterNowX - jitterPrevX,
        jitterNowY - jitterPrevY,
        viewmodelJittered ? 1.0f : 0.0f,
        0.0f
    };
    context->UpdateSubresource(constants_.Get(), 0, nullptr, cb, 0, 0);

    ID3D11RenderTargetView* rtv = motionRtv_.Get();
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ID3D11Buffer* cbPtr = constants_.Get();

    context->OMSetRenderTargets(1, &rtv, depthDsv_.Get());
    context->OMSetDepthStencilState(depthStencilState_.Get(), 0);
    context->OMSetBlendState(blendState_.Get(), blendFactor, 0xFFFFFFFFu);
    context->RSSetState(rasterizerState_.Get());
    context->RSSetViewports(1, &vp);
    context->IASetInputLayout(inputLayout_.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(rasterVs_.Get(), nullptr, 0);
    context->PSSetShader(rasterPs_.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &cbPtr);

    ID3D11Buffer* vbs[2] = {soBuffers_[curSlot_].Get(), soBuffers_[curSlot_ ^ 1u].Get()};
    const UINT strides[2] = {16u, 16u};

    for (const auto& cur : curDraws) {
        for (const auto& prev : prevDraws) {
            if (sameWeaponIdentity(cur.identity, prev.identity) &&
                cur.vertexCount == prev.vertexCount) {
                const UINT offsets[2] = {cur.vertexOffsetBytes, prev.vertexOffsetBytes};
                context->IASetVertexBuffers(0, 2, vbs, strides, offsets);
                context->Draw(cur.vertexCount, 0);
                ++matchedDraws_;
                break;
            }
        }
    }

    ID3D11Buffer* nullVbs[2] = {nullptr, nullptr};
    const UINT zeroArr[2] = {0u, 0u};
    context->IASetVertexBuffers(0, 2, nullVbs, zeroArr, zeroArr);
    context->VSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(nullptr, nullptr, 0);
    context->OMSetDepthStencilState(nullptr, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    hasResolvedMotion_ = (matchedDraws_ > 0);
    return hasResolvedMotion_;
}

} // namespace edpe
