#include "input_hook.h"
#include "config.h"
#include "log.h"
#include "ui.h"

#include <windows.h>
#include <initguid.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace edpe {

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

} // namespace edpe

namespace {

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

using PFN_GetAsyncKeyState = SHORT(WINAPI*)(int);
using PFN_GetKeyState = SHORT(WINAPI*)(int);
using PFN_PeekMessageA = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
using PFN_PeekMessageW = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
using PFN_GetMessageA = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT);
using PFN_GetMessageW = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT);
using PFN_ClipCursor = BOOL(WINAPI*)(const RECT*);
using PFN_SetCursorPos = BOOL(WINAPI*)(int, int);

PFN_GetAsyncKeyState orig_GetAsyncKeyState = nullptr;
PFN_GetKeyState orig_GetKeyState = nullptr;
PFN_PeekMessageA orig_PeekMessageA = nullptr;
PFN_PeekMessageW orig_PeekMessageW = nullptr;
PFN_GetMessageA orig_GetMessageA = nullptr;
PFN_GetMessageW orig_GetMessageW = nullptr;
PFN_ClipCursor orig_ClipCursor = nullptr;
PFN_SetCursorPos orig_SetCursorPos = nullptr;

SHORT WINAPI Hooked_GetAsyncKeyState(int vKey) {
    if (vKey == VK_F5 || vKey == VK_F7) {
        return orig_GetAsyncKeyState ? orig_GetAsyncKeyState(vKey) : GetAsyncKeyState(vKey);
    }
    if (UiMenuVisible()) {
        return 0;
    }
    return orig_GetAsyncKeyState ? orig_GetAsyncKeyState(vKey) : GetAsyncKeyState(vKey);
}

SHORT WINAPI Hooked_GetKeyState(int vKey) {
    if (vKey == VK_F5 || vKey == VK_F7) {
        return orig_GetKeyState ? orig_GetKeyState(vKey) : GetKeyState(vKey);
    }
    if (UiMenuVisible()) {
        return 0;
    }
    return orig_GetKeyState ? orig_GetKeyState(vKey) : GetKeyState(vKey);
}

void filterInputMessage(LPMSG lpMsg, UINT wRemoveMsg) {
    if (!lpMsg || !UiMenuVisible()) return;
    const UINT msg = lpMsg->message;
    const bool isInput = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
                         (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
                         (msg == WM_INPUT);
    if (!isInput) return;

    const bool isHotkey = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN ||
                           msg == WM_KEYUP || msg == WM_SYSKEYUP) &&
                          (lpMsg->wParam == VK_F5 || lpMsg->wParam == VK_F7);
    if (isHotkey) return;

    if (wRemoveMsg & PM_REMOVE) {
        UiProcessInput(lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam);
    }
    lpMsg->message = WM_NULL;
    lpMsg->wParam = 0;
    lpMsg->lParam = 0;
}

BOOL WINAPI Hooked_PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg) {
    BOOL ret = orig_PeekMessageA ? orig_PeekMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg)
                                 : PeekMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
    if (ret) {
        filterInputMessage(lpMsg, wRemoveMsg);
    }
    return ret;
}

BOOL WINAPI Hooked_PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg) {
    BOOL ret = orig_PeekMessageW ? orig_PeekMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg)
                                 : PeekMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
    if (ret) {
        filterInputMessage(lpMsg, wRemoveMsg);
    }
    return ret;
}

BOOL WINAPI Hooked_GetMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax) {
    BOOL ret = orig_GetMessageA ? orig_GetMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax)
                                : GetMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax);
    if (ret > 0) {
        filterInputMessage(lpMsg, PM_REMOVE);
    }
    return ret;
}

BOOL WINAPI Hooked_GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax) {
    BOOL ret = orig_GetMessageW ? orig_GetMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax)
                                : GetMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax);
    if (ret > 0) {
        filterInputMessage(lpMsg, PM_REMOVE);
    }
    return ret;
}

BOOL WINAPI Hooked_ClipCursor(const RECT* lpRect) {
    if (UiMenuVisible()) {
        return TRUE;
    }
    return orig_ClipCursor ? orig_ClipCursor(lpRect) : ClipCursor(lpRect);
}

BOOL WINAPI Hooked_SetCursorPos(int X, int Y) {
    if (UiMenuVisible()) {
        return TRUE;
    }
    return orig_SetCursorPos ? orig_SetCursorPos(X, Y) : SetCursorPos(X, Y);
}

using PFN_DirectInput8Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using PFN_CreateDevice = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8W*, REFGUID, LPDIRECTINPUTDEVICE8W*, LPUNKNOWN);
using PFN_GetDeviceState = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using PFN_GetDeviceData = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

PFN_DirectInput8Create orig_DirectInput8Create = nullptr;
PFN_DirectInput8Create g_pfnDirectInput8Create = nullptr;
PFN_CreateDevice orig_CreateDevice = nullptr;
PFN_GetDeviceState orig_GetDeviceState = nullptr;
PFN_GetDeviceData orig_GetDeviceData = nullptr;

HRESULT STDMETHODCALLTYPE Hooked_GetDeviceState(IDirectInputDevice8W* self, DWORD cbData, LPVOID lpvData) {
    HRESULT hr = orig_GetDeviceState ? orig_GetDeviceState(self, cbData, lpvData) : DI_OK;
    if (SUCCEEDED(hr) && UiMenuVisible() && lpvData && cbData > 0) {
        std::memset(lpvData, 0, cbData);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hooked_GetDeviceData(IDirectInputDevice8W* self, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags) {
    HRESULT hr = orig_GetDeviceData ? orig_GetDeviceData(self, cbObjectData, rgdod, pdwInOut, dwFlags) : DI_OK;
    if (SUCCEEDED(hr) && UiMenuVisible()) {
        if (pdwInOut) {
            *pdwInOut = 0;
        }
    }
    return hr;
}

void hookDeviceVtable(IDirectInputDevice8W* device) {
    if (!device) return;
    void** devVtbl = *reinterpret_cast<void***>(device);
    hookVtableSlot(devVtbl, 9, reinterpret_cast<void*>(&Hooked_GetDeviceState), reinterpret_cast<void**>(&orig_GetDeviceState));
    hookVtableSlot(devVtbl, 10, reinterpret_cast<void*>(&Hooked_GetDeviceData), reinterpret_cast<void**>(&orig_GetDeviceData));
}

HRESULT STDMETHODCALLTYPE Hooked_CreateDevice(IDirectInput8W* self, REFGUID rguid, LPDIRECTINPUTDEVICE8W* lplpDevice, LPUNKNOWN pUnkOuter) {
    HRESULT hr = orig_CreateDevice ? orig_CreateDevice(self, rguid, lplpDevice, pUnkOuter) : E_FAIL;
    if (SUCCEEDED(hr) && lplpDevice && *lplpDevice) {
        hookDeviceVtable(*lplpDevice);
    }
    return hr;
}

HRESULT WINAPI Hooked_DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
    PFN_DirectInput8Create target = orig_DirectInput8Create ? orig_DirectInput8Create : g_pfnDirectInput8Create;
    HRESULT hr = target ? target(hinst, dwVersion, riidltf, ppvOut, punkOuter) : E_FAIL;
    if (SUCCEEDED(hr) && ppvOut && *ppvOut) {
        auto* di = reinterpret_cast<IDirectInput8W*>(*ppvOut);
        void** diVtbl = *reinterpret_cast<void***>(di);
        hookVtableSlot(diVtbl, 3, reinterpret_cast<void*>(&Hooked_CreateDevice), reinterpret_cast<void**>(&orig_CreateDevice));
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

    // Patch USER32 imports in the main executable
    patchIatEntry(gameExe, "USER32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(&Hooked_GetAsyncKeyState), reinterpret_cast<void**>(&orig_GetAsyncKeyState));
    patchIatEntry(gameExe, "USER32.dll", "GetKeyState", reinterpret_cast<void*>(&Hooked_GetKeyState), reinterpret_cast<void**>(&orig_GetKeyState));
    patchIatEntry(gameExe, "USER32.dll", "PeekMessageA", reinterpret_cast<void*>(&Hooked_PeekMessageA), reinterpret_cast<void**>(&orig_PeekMessageA));
    patchIatEntry(gameExe, "USER32.dll", "PeekMessageW", reinterpret_cast<void*>(&Hooked_PeekMessageW), reinterpret_cast<void**>(&orig_PeekMessageW));
    patchIatEntry(gameExe, "USER32.dll", "GetMessageA", reinterpret_cast<void*>(&Hooked_GetMessageA), reinterpret_cast<void**>(&orig_GetMessageA));
    patchIatEntry(gameExe, "USER32.dll", "GetMessageW", reinterpret_cast<void*>(&Hooked_GetMessageW), reinterpret_cast<void**>(&orig_GetMessageW));
    patchIatEntry(gameExe, "USER32.dll", "ClipCursor", reinterpret_cast<void*>(&Hooked_ClipCursor), reinterpret_cast<void**>(&orig_ClipCursor));
    patchIatEntry(gameExe, "USER32.dll", "SetCursorPos", reinterpret_cast<void*>(&Hooked_SetCursorPos), reinterpret_cast<void**>(&orig_SetCursorPos));

    // Patch DINPUT8 imports
    patchIatEntry(gameExe, "DINPUT8.dll", "DirectInput8Create", reinterpret_cast<void*>(&Hooked_DirectInput8Create), reinterpret_cast<void**>(&orig_DirectInput8Create));

    // Proactively hook DirectInput device vtables via a temporary instance
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
                void** diVtbl = *reinterpret_cast<void***>(dummyDI);
                hookVtableSlot(diVtbl, 3, reinterpret_cast<void*>(&Hooked_CreateDevice), reinterpret_cast<void**>(&orig_CreateDevice));

                IDirectInputDevice8W* dummyMouse = nullptr;
                if (SUCCEEDED(dummyDI->CreateDevice(GUID_SysMouse, &dummyMouse, nullptr)) && dummyMouse) {
                    hookDeviceVtable(dummyMouse);
                    dummyMouse->Release();
                }
                IDirectInputDevice8W* dummyKeyboard = nullptr;
                if (SUCCEEDED(dummyDI->CreateDevice(GUID_SysKeyboard, &dummyKeyboard, nullptr)) && dummyKeyboard) {
                    hookDeviceVtable(dummyKeyboard);
                    dummyKeyboard->Release();
                }
                dummyDI->Release();
            }
        }
    }
    EdpeLog(L"EDPE: input isolation hooks installed (DirectInput, GetAsyncKeyState, PeekMessage)");
}

} // namespace edpe
