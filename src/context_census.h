#pragma once

#include <dxgi.h>
#include <d3d11.h>

void ContextCensusOnPresent(IDXGISwapChain* swap_chain, unsigned long long frame, UINT flags);
void ContextCensusAfterPresent(IDXGISwapChain* swap_chain, UINT flags);
void ContextCensusAfterOverlay(IDXGISwapChain* swap_chain, UINT flags);
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
