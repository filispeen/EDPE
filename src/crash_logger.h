#pragma once

#include <windows.h>
#include <cstdint>

struct IDXGISwapChain;
struct ID3D11Device;

namespace edpe {

void installCrashLogger();
void uninstallCrashLogger();

void setLastD3D11HResult(HRESULT hr);
void updateCrashUpscalerState(int backend, int mode, bool isFallback, uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH, uint64_t frameNumber);
void updateCrashGpuInfo(IDXGISwapChain* swapChain);
void updateCrashGpuInfoFromDevice(ID3D11Device* device);
void setCrashFrameNumber(uint64_t frameNumber);

void pruneCrashLogs();
void writeCrashDumpForTesting(EXCEPTION_POINTERS* ep);

}
