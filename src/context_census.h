#pragma once

#include <dxgi.h>
#include <d3d11.h>

void ContextCensusOnPresent(IDXGISwapChain* swap_chain, unsigned long long frame, UINT flags);
void ContextCensusAfterPresent(IDXGISwapChain* swap_chain, UINT flags);
void ContextCensusAfterOverlay(IDXGISwapChain* swap_chain, UINT flags);
void ContextCensusHookDeferredContext(ID3D11DeviceContext* context);
unsigned long long ContextCensusGetDsvBinds();
void ContextCensusOnSwapChainRelease(IUnknown* object);
bool ContextCensusRequestBindSequence();
bool ContextCensusBindSequenceAvailable();
bool ContextCensusRequestDepthSnapshot(unsigned index);
bool ContextCensusDepthSnapshotAvailable(unsigned index);
bool ContextCensusRequestCameraPair(unsigned index);
bool ContextCensusRequestMotionPair(unsigned index);
ID3D11ShaderResourceView* ContextCensusTakeMotionSnapshot(UINT* width, UINT* height);
ID3D11ShaderResourceView* ContextCensusTakeMotionColorSnapshot(UINT* width, UINT* height);
ID3D11ShaderResourceView* ContextCensusTakeMotionEarlyColorSnapshot(UINT* width, UINT* height);
ID3D11ShaderResourceView* ContextCensusTakeMotionDepthSnapshot(UINT* width, UINT* height, unsigned* index = nullptr);
int ContextCensusSceneDepthCandidate();
ID3D11DepthStencilView* ContextCensusTakeDepthSnapshot(unsigned* index,
    ID3D11RenderTargetView** color_view = nullptr);

// Phase 5: Projection Jitter Injection controls and diagnostics
void ContextCensusSetJitterEnabled(bool enabled);
bool ContextCensusGetJitterEnabled();
void ContextCensusSetJitterTestOverride(bool override);
bool ContextCensusGetJitterTestOverride();
bool ContextCensusIsJitterActive();
void ContextCensusGetActiveJitter(float* pixelX, float* pixelY, float* ndcX, float* ndcY,
    unsigned long long* frameIndex = nullptr);
unsigned ContextCensusGetJitteredWritesCount();
unsigned ContextCensusGetJitterRejectionsCount();

// Jitter and Motion Runtime Tuning (Phase 5 / DLAA stability)
void ContextCensusSetJitterInvertX(bool invert);
bool ContextCensusGetJitterInvertX();
void ContextCensusSetJitterInvertY(bool invert);
bool ContextCensusGetJitterInvertY();
void ContextCensusSetJitterSwapXY(bool swap);
bool ContextCensusGetJitterSwapXY();
void ContextCensusSetJitterScale(float scale);
float ContextCensusGetJitterScale();
void ContextCensusSetJitterPhaseCount(unsigned count);
unsigned ContextCensusGetJitterPhaseCount();

void ContextCensusSetZeroMotion(bool zero);
bool ContextCensusGetZeroMotion();
void ContextCensusSetMotionInvertX(bool invert);
bool ContextCensusGetMotionInvertX();
void ContextCensusSetMotionInvertY(bool invert);
bool ContextCensusGetMotionInvertY();
void ContextCensusSetMotionScale(float scale);
float ContextCensusGetMotionScale();
void ContextCensusSetPreserveNativeUi(bool preserve);
bool ContextCensusGetPreserveNativeUi();
void ContextCensusGetSceneRenderSize(UINT* outWidth, UINT* outHeight);
void ContextCensusRequestComparisonCapture();
bool ContextCensusIsComparisonCapturePending();
bool ContextCensusConsumeComparisonCaptureRequest();
ID3D11Texture2D* ContextCensusGetBackbufferCopyTexture();
ID3D11RenderTargetView* ContextCensusGetBackbufferCopyRtv();

// Subpixel Jitter Debug View (Full-screen grayscale game scene + green jitter pixels)
void ContextCensusSetJitterDebugView(bool enable);
bool ContextCensusGetJitterDebugView();
void ContextCensusToggleJitterDebugView();
void ContextCensusSetJitterDebugSensitivity(float sensitivity);
float ContextCensusGetJitterDebugSensitivity();
void ContextCensusSetJitterDebugThreshold(float threshold);
float ContextCensusGetJitterDebugThreshold();
void ContextCensusSetJitterDebugMode(int mode);
int ContextCensusGetJitterDebugMode();

