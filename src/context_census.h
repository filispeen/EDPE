#pragma once

#include <dxgi.h>

void ContextCensusOnPresent(IDXGISwapChain* swap_chain, unsigned long long frame, UINT flags);
void ContextCensusAfterOverlay(IDXGISwapChain* swap_chain, UINT flags);
void ContextCensusOnSwapChainRelease(IUnknown* object);
bool ContextCensusRequestBindSequence();
bool ContextCensusBindSequenceAvailable();
