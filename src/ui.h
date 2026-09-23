#pragma once

#include <dxgi.h>

void UiOnPresent(IDXGISwapChain* swap_chain, UINT flags);
void UiOnResize(IDXGISwapChain* swap_chain);
void UiOnRelease(IUnknown* object);
bool UiMenuVisible();
