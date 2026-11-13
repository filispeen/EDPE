#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

namespace edpe {

class UiLayer {
public:
    static UiLayer& instance();

    bool initialize(ID3D11Device* device);
    void shutdown();

    // Викликається на межі кадру при зміні розміру вікна:
    void resize(ID3D11Device* device, UINT width, UINT height);

    // Очищення шарів на початку кадру:
    void beginFrame(ID3D11DeviceContext* context);

    // Перевірка та перехоплення виклику Draw:
    bool shouldInterceptDraw(uint64_t vsHash, uint64_t psHash, bool isHdrTarget);
    void beginIntercept(ID3D11DeviceContext* context, float jitterX, float jitterY,
                        UINT renderW, UINT renderH, bool isHdr,
                        ID3D11DepthStencilView* overrideDsv = nullptr,
                        ID3D11DepthStencilState* overrideDsState = nullptr,
                        UINT overrideStencilRef = 0,
                        ID3D11BlendState* overrideBlend = nullptr,
                        ID3D11RenderTargetView* knownOrigRtv = nullptr,
                        ID3D11DepthStencilView* knownOrigDsv = nullptr,
                        ID3D11BlendState* knownOrigBlend = nullptr,
                        const FLOAT* knownOrigBlendFactor = nullptr,
                        UINT knownOrigSampleMask = 0xFFFFFFFF,
                        ID3D11DepthStencilState* knownOrigDsState = nullptr,
                        UINT knownOrigStencilRef = 0);
    void endIntercept(ID3D11DeviceContext* context);

    // Ремапінг SV_Position та Sobel-радіуса для 3D-голограм корабля/цілі (cb13):
    bool beginHoloRemap(ID3D11DeviceContext* context, uint64_t psHash,
                        UINT renderW, UINT renderH, float jitterX, float jitterY,
                        float sceneCb1Row120Z = 1.0f);
    void endHoloRemap(ID3D11DeviceContext* context);

    // Розгортання 1-піксельних ліній суперкруїзу (LINELIST) у згладжені смуги через Geometry Shader:
    bool beginSupercruiseBars(ID3D11DeviceContext* context, UINT renderW, UINT renderH);
    void endSupercruiseBars(ID3D11DeviceContext* context);

    // Повторний запуск тонемапу (Tonemap Re-issue):
    bool beginTonemapReissue(ID3D11DeviceContext* context,
                             ID3D11ShaderResourceView* gameLut = nullptr,
                             ID3D11Buffer* gameExposureCb = nullptr,
                             int hdrSlot = 1,
                             uint64_t psHash = 0);
    void endTonemapReissue(ID3D11DeviceContext* context);

    // Фінальний композитинг перед Pass 54 або Present (CS UAV або прямий PS RTV без проміжного копіювання):
    bool composite(ID3D11DeviceContext* context,
                   ID3D11ShaderResourceView* worldReconstruction,
                   ID3D11UnorderedAccessView* backbufferUav,
                   UINT outputWidth, UINT outputHeight,
                   bool debugView = false);
    bool compositeToRtv(ID3D11DeviceContext* context,
                        ID3D11ShaderResourceView* worldReconstruction,
                        ID3D11RenderTargetView* targetRtv,
                        UINT outputWidth, UINT outputHeight,
                        bool debugView = false,
                        bool callerRestoresState = false);

    ID3D11Texture2D* hdrLayerTexture() const { return hdrLayerTex_.Get(); }
    ID3D11RenderTargetView* hdrLayerRtv() const { return hdrLayerRtv_.Get(); }
    ID3D11ShaderResourceView* hdrLayerSrv() const { return hdrLayerSrv_.Get(); }

    ID3D11Texture2D* ldrLayerTexture() const { return ldrLayerTex_.Get(); }
    ID3D11RenderTargetView* ldrLayerRtv() const { return ldrLayerRtv_.Get(); }
    ID3D11ShaderResourceView* ldrLayerSrv() const { return ldrLayerSrv_.Get(); }

    ID3D11Texture2D* multLayerTexture() const { return multLayerTex_.Get(); }
    ID3D11RenderTargetView* multLayerRtv() const { return multLayerRtv_.Get(); }
    ID3D11ShaderResourceView* multLayerSrv() const { return multLayerSrv_.Get(); }

    ID3D11ShaderResourceView* zeroBloomSrv() const { return zeroBloomSrv_.Get(); }

    UINT width() const { return width_; }
    UINT height() const { return height_; }
    ID3D11Device* device() const { return device_.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> compositeCs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> compositePs_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> coverageVs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> coveragePs_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> toneRgbOnlyBlend_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> coverageAlphaOnlyBlend_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> alphaBlend_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> premulBlend_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> addBlend_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> scaledAddBlend_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> opaqueBlend_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> darkenBlend_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> multBlend_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> noDepthState_;

    // Чорна 1x1 текстура для обнулення слотів Bloom при повторному виклику тонемапу:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> zeroBloomTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> zeroBloomSrv_;

    // Ресурси для бінарного патчу 3D-голограм (PS b13):
    Microsoft::WRL::ComPtr<ID3D11PixelShader> holoPatchedPs_[3];
    bool holoPatchAttempted_[3]{};
    Microsoft::WRL::ComPtr<ID3D11Buffer> holoRemapCb_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> savedHoloPs_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> savedHoloCb13_;
    bool isHoloRemapped_ = false;

    // Ресурси Geometry Shader для смуг суперкруїзу:
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> supercruiseBarsGs_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> supercruiseBarsCb_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> barsRasterNoScissor_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> barsRasterScissor_;
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> savedBarsGs_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> savedBarsGsCb0_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> savedBarsRs_;
    bool isBarsActive_ = false;

    // Позаекранні текстури інтерфейсу на нативній роздільності екрана D:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> hdrLayerTex_;       // R16G16B16A16_FLOAT
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> hdrLayerRtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> hdrLayerSrv_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> ldrLayerTex_;       // R8G8B8A8_UNORM
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> ldrLayerRtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ldrLayerSrv_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> multLayerTex_;      // R8G8B8A8_UNORM
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> multLayerRtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> multLayerSrv_;

    // Збережений контекст для відновлення:
    D3D11_VIEWPORT savedViewports_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT savedViewportCount_ = 0;
    D3D11_RECT savedScissors_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT savedScissorCount_ = 0;
    bool viewportModified_ = false;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> savedRtv_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> savedDsv_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> savedBlend_;
    FLOAT savedBlendFactor_[4]{};
    UINT savedSampleMask_ = 0xFFFFFFFF;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> savedDsState_;
    UINT savedStencilRef_ = 0;

    // Збережений контекст для Tonemap Re-issue:
    D3D11_VIEWPORT savedToneViewport_{};
    UINT savedToneViewportCount_ = 0;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> savedToneRtv_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> savedToneDsv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> savedToneSrvs_[5];
    Microsoft::WRL::ComPtr<ID3D11Buffer> savedToneExposureCb_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> savedToneBlend_;
    FLOAT savedToneBlendFactor_[4]{};
    UINT savedToneSampleMask_ = 0xFFFFFFFF;

    bool isIntercepting_ = false;
    bool isTonemapReissuing_ = false;
    bool hasMultiplyDraws_ = false;
    bool multLayerCleared_ = false;
    UINT width_ = 0;
    UINT height_ = 0;
};

} // namespace edpe
