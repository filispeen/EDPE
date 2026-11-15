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

namespace edpe {

static bool isValidRva(DWORD rva, DWORD sizeNeeded, DWORD imageSize) {
    if (rva == 0 || imageSize == 0 || sizeNeeded > imageSize) return false;
    return rva <= imageSize - sizeNeeded;
}

bool patchIatEntry(HMODULE module, const char* targetDll, const char* targetFunction, void* hook, void** orig) {
    if (!module || !targetDll || !targetFunction || !hook) return false;

    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dosHeader->e_lfanew <= 0 || dosHeader->e_lfanew >= 0x100000) return false;

    auto* ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return false;
    if (ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;

    const DWORD imageSize = ntHeaders->OptionalHeader.SizeOfImage;
    if (imageSize < sizeof(IMAGE_NT_HEADERS)) return false;

    const auto& importDataDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!isValidRva(importDataDir.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR), imageSize)) return false;

    auto* importDesc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(base + importDataDir.VirtualAddress);
    const auto* importEnd = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        base + importDataDir.VirtualAddress +
        (importDataDir.Size <= imageSize - importDataDir.VirtualAddress
             ? importDataDir.Size
             : imageSize - importDataDir.VirtualAddress));

    for (; importDesc + 1 <= importEnd && importDesc->Name != 0; ++importDesc) {
        if (!isValidRva(importDesc->Name, 1, imageSize)) continue;
        const char* currentDll = reinterpret_cast<const char*>(base + importDesc->Name);
        if (_stricmp(currentDll, targetDll) != 0) continue;

        // Require OriginalFirstThunk: a zero OriginalFirstThunk forces walking FirstThunk,
        // which already holds resolved 64-bit VAs at runtime rather than RVAs.
        if (!isValidRva(importDesc->OriginalFirstThunk, sizeof(IMAGE_THUNK_DATA), imageSize)) continue;
        if (!isValidRva(importDesc->FirstThunk, sizeof(IMAGE_THUNK_DATA), imageSize)) continue;

        auto* thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + importDesc->OriginalFirstThunk);
        auto* iat = reinterpret_cast<PIMAGE_THUNK_DATA>(base + importDesc->FirstThunk);

        for (; thunk->u1.AddressOfData != 0; ++thunk, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(thunk->u1.Ordinal)) continue;
            if (thunk->u1.AddressOfData > imageSize - sizeof(WORD) - 1) continue;

            const DWORD nameRva = static_cast<DWORD>(thunk->u1.AddressOfData);
            auto* importByName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(base + nameRva);
            if (strcmp(reinterpret_cast<const char*>(importByName->Name), targetFunction) == 0) {
                DWORD oldProtect = 0;
                if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                    return false;
                }
                if (orig && *orig == nullptr) {
                    *orig = reinterpret_cast<void*>(iat->u1.Function);
                }
                iat->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
                DWORD ignored = 0;
                VirtualProtect(&iat->u1.Function, sizeof(void*), oldProtect, &ignored);
                return true;
            }
        }
    }
    return false;
}

} // namespace edpe

namespace {

void hookVtableSlot(void** vtbl, int index, void* newFunc, void** origFunc) {
    if (!vtbl || !newFunc || vtbl[index] == newFunc) return;
    DWORD oldProtect = 0;
    if (VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        if (origFunc && *origFunc == nullptr) {
            *origFunc = vtbl[index];
        }
        InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&vtbl[index]), newFunc);
        DWORD ignored = 0;
        VirtualProtect(&vtbl[index], sizeof(void*), oldProtect, &ignored);
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

thread_local int t_createDeviceDepth = 0;

struct CreateDeviceDepthGuard {
    int& depth;
    explicit CreateDeviceDepthGuard(int& d) : depth(d) { ++depth; }
    ~CreateDeviceDepthGuard() { --depth; }
};

HRESULT STDMETHODCALLTYPE Hooked_CreateDeviceW(IDirectInput8W* self, REFGUID rguid, LPDIRECTINPUTDEVICE8W* lplpDevice, LPUNKNOWN pUnkOuter) {
    CreateDeviceDepthGuard guard(t_createDeviceDepth);
    HRESULT hr = orig_CreateDeviceW ? orig_CreateDeviceW(self, rguid, lplpDevice, pUnkOuter) : E_FAIL;
    if (guard.depth == 1 && SUCCEEDED(hr) && lplpDevice && *lplpDevice) {
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
    CreateDeviceDepthGuard guard(t_createDeviceDepth);
    HRESULT hr = orig_CreateDeviceA ? orig_CreateDeviceA(self, rguid, lplpDevice, pUnkOuter) : E_FAIL;
    if (guard.depth == 1 && SUCCEEDED(hr) && lplpDevice && *lplpDevice) {
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
        if (diVtbl && diVtbl[3] != reinterpret_cast<void*>(&Hooked_CreateDeviceW) &&
            diVtbl[3] != reinterpret_cast<void*>(&Hooked_CreateDeviceA)) {
            hookVtableSlot(diVtbl, 3, reinterpret_cast<void*>(&Hooked_CreateDeviceW), reinterpret_cast<void**>(&orig_CreateDeviceW));
        }
    } else if (IsEqualIID(riidltf, IID_IDirectInput8A)) {
        auto* di = reinterpret_cast<IDirectInput8A*>(diInstance);
        void** diVtbl = *reinterpret_cast<void***>(di);
        if (diVtbl && diVtbl[3] != reinterpret_cast<void*>(&Hooked_CreateDeviceA) &&
            diVtbl[3] != reinterpret_cast<void*>(&Hooked_CreateDeviceW)) {
            hookVtableSlot(diVtbl, 3, reinterpret_cast<void*>(&Hooked_CreateDeviceA), reinterpret_cast<void**>(&orig_CreateDeviceA));
        }
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
