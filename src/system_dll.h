#pragma once

#include <cwchar>
#include <windows.h>

inline HMODULE LoadSystemDll(const wchar_t* name) {
    wchar_t path[MAX_PATH];
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (!length || length >= MAX_PATH || length + 1 + wcslen(name) >= MAX_PATH) return nullptr;
    path[length] = L'\\';
    wcscpy_s(path + length + 1, MAX_PATH - length - 1, name);
    return LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}
