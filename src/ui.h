#pragma once

#include <dxgi.h>

void UiOnPresent(IDXGISwapChain* swap_chain, UINT flags);
void UiOnResize(IDXGISwapChain* swap_chain);
void UiOnRelease(IUnknown* object);
bool UiMenuVisible();
bool UiProcessInput(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
void UiQueueComparisonCapture(ID3D11DeviceContext* context, ID3D11Texture2D* beforeTex, ID3D11Texture2D* afterTex);

