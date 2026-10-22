#include "input_hook.h"
#include "config.h"
#include "log.h"
#include "ui.h"

#include <windows.h>
#include <initguid.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include "dinput_proxy.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace {

bool patchIatEntry(HMODULE module, const char* targetDll, const char* targetFunction, void* hook, void** orig) {
    if (!module || !targetDll || !targetFunction || !hook) return false;

    auto* dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(
        reinterpret_cast<uint8_t*>(module) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto& importDataDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDataDir.VirtualAddress == 0 || importDataDir.Size == 0) return false;

    auto* importDesc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(
        reinterpret_cast<uint8_t*>(module) + importDataDir.VirtualAddress);

    for (; importDesc->Name != 0; ++importDesc) {
        const char* currentDll = reinterpret_cast<const char*>(
            reinterpret_cast<uint8_t*>(module) + importDesc->Name);
        if (_stricmp(currentDll, targetDll) != 0) continue;

        auto* thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
            reinterpret_cast<uint8_t*>(module) +
            (importDesc->OriginalFirstThunk ? importDesc->OriginalFirstThunk : importDesc->FirstThunk));
        auto* iat = reinterpret_cast<PIMAGE_THUNK_DATA>(
            reinterpret_cast<uint8_t*>(module) + importDesc->FirstThunk);

        for (; thunk->u1.AddressOfData != 0; ++thunk, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(thunk->u1.Ordinal)) continue;

            auto* importByName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
                reinterpret_cast<uint8_t*>(module) + thunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(importByName->Name), targetFunction) == 0) {
                DWORD oldProtect = 0;
                if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                    return false;
                }
                if (orig && *orig == nullptr) {
                    *orig = reinterpret_cast<void*>(iat->u1.Function);
                }
                iat->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
                VirtualProtect(&iat->u1.Function, sizeof(void*), oldProtect, &oldProtect);
                return true;
            }
        }
    }
    return false;
}

void hookVtableSlot(void** vtbl, int index, void* newFunc, void** origFunc) {
    if (!vtbl || vtbl[index] == newFunc) return;
    DWORD oldProtect = 0;
    if (VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        if (origFunc && *origFunc == nullptr) {
            *origFunc = vtbl[index];
        }
        vtbl[index] = newFunc;
        VirtualProtect(&vtbl[index], sizeof(void*), oldProtect, &oldProtect);
    }
}

using PFN_ClipCursor = BOOL(WINAPI*)(const RECT*);
using PFN_SetCursorPos = BOOL(WINAPI*)(int, int);
using PFN_GetCursorPos = BOOL(WINAPI*)(LPPOINT);

PFN_ClipCursor orig_ClipCursor = nullptr;
PFN_SetCursorPos orig_SetCursorPos = nullptr;
PFN_GetCursorPos orig_GetCursorPos = nullptr;

static POINT s_savedCursorPos{0, 0};
static std::atomic<bool> s_hasSavedPos{false};

BOOL WINAPI Hooked_ClipCursor(const RECT* lpRect) {
    if (UiMenuVisible()) {
        return orig_ClipCursor ? orig_ClipCursor(nullptr) : ClipCursor(nullptr);
    }
    return orig_ClipCursor ? orig_ClipCursor(lpRect) : ClipCursor(lpRect);
}

BOOL WINAPI Hooked_SetCursorPos(int X, int Y) {
    if (UiMenuVisible()) {
        return TRUE;
    }
    return orig_SetCursorPos ? orig_SetCursorPos(X, Y) : SetCursorPos(X, Y);
}

BOOL WINAPI Hooked_GetCursorPos(LPPOINT lpPoint) {
    if (!lpPoint) return FALSE;
    if (UiMenuVisible()) {
        if (!s_hasSavedPos.load(std::memory_order_relaxed)) {
            GetCursorPos(&s_savedCursorPos);
            s_hasSavedPos.store(true, std::memory_order_relaxed);
        }
        *lpPoint = s_savedCursorPos;
        return TRUE;
    }
    s_hasSavedPos.store(false, std::memory_order_relaxed);
    return orig_GetCursorPos ? orig_GetCursorPos(lpPoint) : GetCursorPos(lpPoint);
}

using PFN_DirectInput8Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using PFN_CreateDeviceW = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8W*, REFGUID, LPDIRECTINPUTDEVICE8W*, LPUNKNOWN);
using PFN_CreateDeviceA = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);

PFN_DirectInput8Create orig_DirectInput8Create = nullptr;
PFN_DirectInput8Create g_pfnDirectInput8Create = nullptr;
PFN_CreateDeviceW orig_CreateDeviceW = nullptr;
PFN_CreateDeviceA orig_CreateDeviceA = nullptr;

HRESULT STDMETHODCALLTYPE Hooked_CreateDeviceW(IDirectInput8W* self, REFGUID rguid, LPDIRECTINPUTDEVICE8W* lplpDevice, LPUNKNOWN pUnkOuter) {
    HRESULT hr = orig_CreateDeviceW ? orig_CreateDeviceW(self, rguid, lplpDevice, pUnkOuter) : E_FAIL;
    if (SUCCEEDED(hr) && lplpDevice && *lplpDevice) {
        if (IsEqualGUID(rguid, GUID_SysMouse) || IsEqualGUID(rguid, GUID_SysKeyboard)) {
            *lplpDevice = new SafeDirectInputDevice8WProxy(*lplpDevice, IID_IDirectInputDevice8W);
            EdpeLog(IsEqualGUID(rguid, GUID_SysMouse)
                ? L"EDPE: wrapped DirectInput SysMouse with SafeDirectInputDevice8WProxy"
                : L"EDPE: wrapped DirectInput SysKeyboard with SafeDirectInputDevice8WProxy");
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hooked_CreateDeviceA(IDirectInput8A* self, REFGUID rguid, LPDIRECTINPUTDEVICE8A* lplpDevice, LPUNKNOWN pUnkOuter) {
    HRESULT hr = orig_CreateDeviceA ? orig_CreateDeviceA(self, rguid, lplpDevice, pUnkOuter) : E_FAIL;
    if (SUCCEEDED(hr) && lplpDevice && *lplpDevice) {
        if (IsEqualGUID(rguid, GUID_SysMouse) || IsEqualGUID(rguid, GUID_SysKeyboard)) {
            *lplpDevice = new SafeDirectInputDevice8AProxy(*lplpDevice, IID_IDirectInputDevice8A);
            EdpeLog(IsEqualGUID(rguid, GUID_SysMouse)
                ? L"EDPE: wrapped DirectInput SysMouse with SafeDirectInputDevice8AProxy"
                : L"EDPE: wrapped DirectInput SysKeyboard with SafeDirectInputDevice8AProxy");
        }
    }
    return hr;
}

void hookDirectInput8Vtable(REFIID riidltf, void* diInstance) {
    if (!diInstance) return;
    if (IsEqualIID(riidltf, IID_IDirectInput8W)) {
        auto* di = reinterpret_cast<IDirectInput8W*>(diInstance);
        void** diVtbl = *reinterpret_cast<void***>(di);
        hookVtableSlot(diVtbl, 3, reinterpret_cast<void*>(&Hooked_CreateDeviceW), reinterpret_cast<void**>(&orig_CreateDeviceW));
    } else if (IsEqualIID(riidltf, IID_IDirectInput8A)) {
        auto* di = reinterpret_cast<IDirectInput8A*>(diInstance);
        void** diVtbl = *reinterpret_cast<void***>(di);
        hookVtableSlot(diVtbl, 3, reinterpret_cast<void*>(&Hooked_CreateDeviceA), reinterpret_cast<void**>(&orig_CreateDeviceA));
    }
}

HRESULT WINAPI Hooked_DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
    PFN_DirectInput8Create target = orig_DirectInput8Create ? orig_DirectInput8Create : g_pfnDirectInput8Create;
    HRESULT hr = target ? target(hinst, dwVersion, riidltf, ppvOut, punkOuter) : E_FAIL;
    if (SUCCEEDED(hr) && ppvOut && *ppvOut) {
        hookDirectInput8Vtable(riidltf, *ppvOut);
    }
    return hr;
}

} // namespace

namespace edpe {

void installInputHooks() {
    if (!isRunningInEliteProcess()) return;

    static std::atomic<bool> s_installed{false};
    if (s_installed.exchange(true)) return;

    HMODULE gameExe = GetModuleHandleW(nullptr);
    if (!gameExe) return;

    // Patch USER32 imports in the main executable for cursor confinement & centering during flight
    patchIatEntry(gameExe, "USER32.dll", "ClipCursor", reinterpret_cast<void*>(&Hooked_ClipCursor), reinterpret_cast<void**>(&orig_ClipCursor));
    patchIatEntry(gameExe, "USER32.dll", "SetCursorPos", reinterpret_cast<void*>(&Hooked_SetCursorPos), reinterpret_cast<void**>(&orig_SetCursorPos));
    patchIatEntry(gameExe, "USER32.dll", "GetCursorPos", reinterpret_cast<void*>(&Hooked_GetCursorPos), reinterpret_cast<void**>(&orig_GetCursorPos));

    // Patch DINPUT8 imports
    patchIatEntry(gameExe, "DINPUT8.dll", "DirectInput8Create", reinterpret_cast<void*>(&Hooked_DirectInput8Create), reinterpret_cast<void**>(&orig_DirectInput8Create));

    // Proactively hook DirectInput vtable via a temporary instance
    HMODULE hDInput = GetModuleHandleW(L"dinput8.dll");
    if (!hDInput) {
        hDInput = LoadLibraryW(L"dinput8.dll");
    }
    if (hDInput) {
        auto pDICreate = reinterpret_cast<PFN_DirectInput8Create>(GetProcAddress(hDInput, "DirectInput8Create"));
        if (pDICreate) {
            g_pfnDirectInput8Create = pDICreate;
            IDirectInput8W* dummyDI = nullptr;
            if (SUCCEEDED(pDICreate(gameExe, DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&dummyDI), nullptr)) && dummyDI) {
                hookDirectInput8Vtable(IID_IDirectInput8W, dummyDI);
                dummyDI->Release();
            }
        }
    }
    EdpeLog(L"EDPE: input isolation hooks installed (SafeDirectInputProxy, ClipCursor, SetCursorPos, GetCursorPos)");
}

} // namespace edpe
