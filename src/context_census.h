#pragma once

#include <dxgi.h>

void ContextCensusOnPresent(IDXGISwapChain* swap_chain, unsigned long long frame, UINT flags);
void ContextCensusAfterPresent(IDXGISwapChain* swap_chain, UINT flags, HRESULT result);
void ContextCensusOnSwapChainRelease(IUnknown* object);
bool ContextCensusDepthClearProbeAvailable();
bool ContextCensusRequestDepthClearProbe();
const char* ContextCensusDepthClearProbeStatus();
