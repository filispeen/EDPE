#pragma once

#include <cstring>
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

inline void* ResolvePeExportRaw(HMODULE module, const char* procName) {
    if (!module || !procName) return nullptr;
    const auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const auto& expDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (expDir.VirtualAddress == 0 || expDir.Size == 0) return nullptr;
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + expDir.VirtualAddress);
    const auto* funcs = reinterpret_cast<const DWORD*>(base + exp->AddressOfFunctions);
    const auto* names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exp->AddressOfNameOrdinals);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* name = reinterpret_cast<const char*>(base + names[i]);
        if (std::strcmp(name, procName) == 0) {
            const DWORD rva = funcs[ordinals[i]];
            if (rva >= expDir.VirtualAddress && rva < expDir.VirtualAddress + expDir.Size) {
                return nullptr; // Forwarded export: let GetProcAddress handle it.
            }
            return const_cast<unsigned char*>(base + rva);
        }
    }
    return nullptr;
}

inline bool IsAddressInRenderDocModule(const void* addr) {
    if (!addr) return false;
    const HMODULE rdoc = GetModuleHandleW(L"renderdoc.dll");
    if (!rdoc) return false;
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(addr), &owner)) {
        return false;
    }
    return owner == rdoc;
}
