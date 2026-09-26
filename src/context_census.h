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
int ContextCensusSceneDepthCandidate();
ID3D11DepthStencilView* ContextCensusTakeDepthSnapshot(unsigned* index,
    ID3D11RenderTargetView** color_view = nullptr);
