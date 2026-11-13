#pragma once

#include <cwchar>
#include <windows.h>

inline HMODULE LoadSystemDll(const wchar_t* name) {
    wchar_t path[MAX_PATH];
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (!length || length >= MAX_PATH || length + 1 + wcslen(name) >= MAX_PATH) return nullptr;
    path[length] = L'\\';
    wcscpy_s(path + length + 1, MAX_PATH - length - 1, name);
    HMODULE mod = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod) return nullptr;
    HMODULE selfMod = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&LoadSystemDll), &selfMod) &&
        selfMod && mod == selfMod) {
        FreeLibrary(mod);
        return nullptr;
    }
    return mod;
}
