#pragma once

#include <dxgi.h>

void UiOnPresent(IDXGISwapChain* swap_chain, UINT flags);
void UiOnResize(IDXGISwapChain* swap_chain);
void UiOnRelease(IUnknown* object);
bool UiMenuVisible();
bool UiProcessInput(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct _SYSTEMTIME;
typedef struct _SYSTEMTIME SYSTEMTIME;
void UiQueueComparisonItem(ID3D11DeviceContext* context, const wchar_t* name, ID3D11Texture2D* texture, const SYSTEMTIME* customTime = nullptr);
void UiQueueComparisonCapture(ID3D11DeviceContext* context, ID3D11Texture2D* beforeTex, ID3D11Texture2D* afterTex);

