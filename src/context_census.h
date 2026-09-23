#pragma once

#include <dxgi.h>

void ContextCensusOnPresent(IDXGISwapChain* swap_chain, unsigned long long frame);
void ContextCensusOnSwapChainRelease(IUnknown* object);
