#pragma once

#include "system_dll.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <d3d11.h>
#include <mutex>
#include <windows.h>
#include <tlhelp32.h>

inline bool BuildSystemDllPath(const wchar_t* name, wchar_t (&outPath)[MAX_PATH]) {
    const UINT length = GetSystemDirectoryW(outPath, MAX_PATH);
    if (!length || length >= MAX_PATH || length + 1 + wcslen(name) >= MAX_PATH) return false;
    outPath[length] = L'\\';
    wcscpy_s(outPath + length + 1, MAX_PATH - length - 1, name);
    return true;
}

inline HMODULE walkMappedD3D11AtPath(const wchar_t* wantedPath) {
    if (!wantedPath || !wantedPath[0]) return nullptr;
    HANDLE snap = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return nullptr;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    HMODULE found = nullptr;
    if (Module32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExePath, wantedPath) == 0) {
                found = entry.hModule;
                break;
            }
        } while (Module32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

// Verify that `held` is the genuine %SystemRoot%\System32\d3d11.dll module and
// not a third-party wrapper redirected by a process-wide LoadLibraryExW hook.
inline HMODULE chooseSystemD3D11(HMODULE held) {
    wchar_t wanted[MAX_PATH]{};
    if (!BuildSystemDllPath(L"d3d11.dll", wanted)) return held;

    if (held) {
        wchar_t actual[MAX_PATH]{};
        if (GetModuleFileNameW(held, actual, MAX_PATH) > 0 &&
            _wcsicmp(actual, wanted) == 0) {
            return held;
        }
    }

    HMODULE existing = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, wanted, &existing) &&
        existing && existing != held) {
        wchar_t actual[MAX_PATH]{};
        if (GetModuleFileNameW(existing, actual, MAX_PATH) > 0 &&
            _wcsicmp(actual, wanted) == 0) {
            return existing;
        }
    }

    if (HMODULE mapped = walkMappedD3D11AtPath(wanted); mapped && mapped != held) {
        return mapped;
    }
    return held;
}

inline HMODULE probeSystemD3D11Module() {
    static HMODULE s_mod = nullptr;
    if (s_mod) return s_mod;
    wchar_t wanted[MAX_PATH]{};
    if (BuildSystemDllPath(L"d3d11.dll", wanted)) {
        HMODULE existing = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, wanted, &existing) &&
            existing) {
            wchar_t actual[MAX_PATH]{};
            if (GetModuleFileNameW(existing, actual, MAX_PATH) > 0 &&
                _wcsicmp(actual, wanted) == 0) {
                s_mod = existing;
                return s_mod;
            }
        }
        if (HMODULE mapped = walkMappedD3D11AtPath(wanted)) {
            s_mod = mapped;
            return s_mod;
        }
    }
    s_mod = chooseSystemD3D11(LoadSystemDll(L"d3d11.dll"));
    return s_mod;
}

namespace edpe {

enum class HookMode {
    InPlace,  // Patch entries directly inside the object's existing vtable
    LiveCopy, // Redirect object's vptr to a 512-slot private table backed by 16-byte live trampolines
};

inline bool safeReadPtr(const void* addr, void** outVal) {
    if (!addr || !outVal) return false;
    __try {
        *outVal = *reinterpret_cast<void* const*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outVal = nullptr;
        return false;
    }
}

inline bool safeWritePtr(void* addr, void* val) {
    if (!addr) return false;
    __try {
        *reinterpret_cast<void**>(addr) = val;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool isExecutableCodePtr(const void* p) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    constexpr DWORD kExecMask =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & kExecMask) == 0) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    return true;
}

inline size_t vtableEntriesInModule(void** vt, size_t count, HMODULE mod) {
    if (!vt || !mod) return 0;
    size_t hits = 0;
    for (size_t i = 0; i < count; ++i) {
        void* entry = nullptr;
        if (!safeReadPtr(&vt[i], &entry) || !entry) break;
        HMODULE owner = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(entry), &owner) &&
            owner == mod) {
            ++hits;
        }
    }
    return hits;
}

// Select LiveCopy when at least 75% of the first 96 context vtable entries reside inside
// %SystemRoot%\System32\d3d11.dll. When a third-party wrapper owns the vtable, fall back to InPlace.
inline HookMode contextHookModeFor(ID3D11DeviceContext* ctx) {
    if (!ctx) return HookMode::InPlace;
    void** vt = nullptr;
    if (!safeReadPtr(ctx, reinterpret_cast<void**>(&vt)) || !vt) return HookMode::InPlace;
    HMODULE sys = probeSystemD3D11Module();
    if (!sys) return HookMode::InPlace;
    constexpr size_t kSample = 96;
    const size_t inSystem = vtableEntriesInModule(vt, kSample, sys);
    return (inSystem * 4 >= kSample * 3) ? HookMode::LiveCopy : HookMode::InPlace;
}

class VTableHook {
public:
    // 512 slots covers the entire ID3D11DeviceContext4 + internal d3d11.dll vtable (~302 methods)
    // and places m_privateTable on a dedicated 4096-byte PAGE_READWRITE page followed by
    // 2 pages (8192 bytes) of PAGE_EXECUTE_READ 16-byte live trampolines.
    static constexpr size_t kTableSlots = 512;
    static constexpr size_t kStubSize = 16;
    static constexpr size_t kMaxSlotOwners = 256;

    VTableHook() = default;
    // Do NOT call unhookAll() in ~VTableHook(): static destruction runs during DLL_PROCESS_DETACH
    // after COM objects (ID3D11DeviceContext) have already been freed.
    ~VTableHook() = default;

    VTableHook(const VTableHook&) = delete;
    VTableHook& operator=(const VTableHook&) = delete;

    // Generates a 16-byte live trampoline:
    //   48 B8 <8-byte &m_vtable[slot]>   mov rax, imm64
    //   FF 20                            jmp qword ptr [rax]
    //   CC CC CC CC                      int3 padding
    static void writeLiveStub(uint8_t* stub, void** realVtableSlotAddr) {
        stub[0] = 0x48;
        stub[1] = 0xB8;
        std::memcpy(stub + 2, &realVtableSlotAddr, sizeof(void*));
        stub[10] = 0xFF;
        stub[11] = 0x20;
        stub[12] = 0xCC;
        stub[13] = 0xCC;
        stub[14] = 0xCC;
        stub[15] = 0xCC;
    }

    bool initialize(void* instance, HookMode mode) {
        if (!instance) return false;
        std::lock_guard<std::mutex> lock(s_mutex());
        if (m_initialized) return true;

        void** vt = nullptr;
        if (!safeReadPtr(instance, reinterpret_cast<void**>(&vt)) || !vt) return false;

        m_instance = instance;
        m_vtable = vt;
        m_mode = mode;

        if (m_mode == HookMode::LiveCopy) {
            constexpr size_t kTableBytes = kTableSlots * sizeof(void*); // 4096 bytes (1 page)
            constexpr size_t kStubBytes = kTableSlots * kStubSize;      // 8192 bytes (2 pages)
            constexpr size_t kAllocBytes = kTableBytes + kStubBytes;
            auto* mem = static_cast<uint8_t*>(
                VirtualAlloc(nullptr, kAllocBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!mem) {
                m_mode = HookMode::InPlace;
            } else {
                m_liveAlloc = mem;
                m_liveAllocBytes = kAllocBytes;
                m_privateTable = reinterpret_cast<void**>(mem);
                m_liveStubs = mem + kTableBytes;

                bool readingValid = true;
                for (size_t i = 0; i < kTableSlots; ++i) {
                    uint8_t* stub = m_liveStubs + i * kStubSize;
                    writeLiveStub(stub, &m_vtable[i]);
                    void* entry = nullptr;
                    if (readingValid && safeReadPtr(&m_vtable[i], &entry)) {
                        if (isExecutableCodePtr(entry)) {
                            m_privateTable[i] = stub;
                            m_validStubSlot[i] = true;
                        } else {
                            m_privateTable[i] = entry;
                            m_validStubSlot[i] = false;
                        }
                    } else {
                        readingValid = false;
                        m_privateTable[i] = stub;
                        m_validStubSlot[i] = false;
                    }
                }

                // Seal only the stub pages as PAGE_EXECUTE_READ; m_privateTable (first 4096B page)
                // remains PAGE_READWRITE data so atomic slot updates never fault on a read-only page.
                DWORD oldProt = 0;
                if (!VirtualProtect(m_liveStubs, kStubBytes, PAGE_EXECUTE_READ, &oldProt)) {
                    m_mode = HookMode::InPlace;
                } else {
                    FlushInstructionCache(GetCurrentProcess(), m_liveStubs, kStubBytes);
                    DWORD vptrProt = 0;
                    if (VirtualProtect(m_instance, sizeof(void*), PAGE_READWRITE, &vptrProt)) {
                        safeWritePtr(m_instance, m_privateTable);
                        DWORD ignored = 0;
                        VirtualProtect(m_instance, sizeof(void*), vptrProt, &ignored);
                    } else {
                        m_mode = HookMode::InPlace;
                    }
                }
            }
        }

        m_initialized = true;
        return true;
    }

    bool hook(void* instance, size_t slotIndex, void* hookFunc, void** originalOut,
              HookMode mode = HookMode::InPlace) {
        if (!instance || !hookFunc || slotIndex >= kTableSlots) return false;
        if (!m_initialized) {
            if (!initialize(instance, mode)) return false;
        }
        std::lock_guard<std::mutex> lock(s_mutex());
        if (m_mode == HookMode::LiveCopy && m_privateTable && m_liveStubs) {
            uint8_t* stubAddr = m_liveStubs + slotIndex * kStubSize;
            if (originalOut) {
                *originalOut = stubAddr;
            }
            m_hookedSlots[slotIndex] = hookFunc;
            m_forwardSlots[slotIndex] = originalOut;
            InterlockedExchangePointer(
                reinterpret_cast<PVOID volatile*>(&m_privateTable[slotIndex]), hookFunc);
            void* curVptr = nullptr;
            if (safeReadPtr(m_instance, &curVptr) && curVptr != m_privateTable) {
                DWORD vptrProt = 0;
                if (VirtualProtect(m_instance, sizeof(void*), PAGE_READWRITE, &vptrProt)) {
                    safeWritePtr(m_instance, m_privateTable);
                    DWORD ignored = 0;
                    VirtualProtect(m_instance, sizeof(void*), vptrProt, &ignored);
                }
            }
            return true;
        }

        // InPlace mode with 256-slot owner registry
        void** slotAddr = &m_vtable[slotIndex];
        for (size_t i = 0; i < kMaxSlotOwners; ++i) {
            if (s_owners()[i].slotAddr == slotAddr && s_owners()[i].owner != this) {
                if (originalOut) *originalOut = s_owners()[i].original;
                return true;
            }
        }

        void* current = nullptr;
        if (!safeReadPtr(slotAddr, &current)) return false;
        if (current != hookFunc) {
            m_originals[slotIndex] = current;
        }
        if (originalOut && m_originals[slotIndex]) {
            *originalOut = m_originals[slotIndex];
        }
        m_hookedSlots[slotIndex] = hookFunc;
        m_forwardSlots[slotIndex] = originalOut;

        DWORD oldProt = 0;
        if (!VirtualProtect(slotAddr, sizeof(void*), PAGE_READWRITE, &oldProt)) {
            return false;
        }
        InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slotAddr), hookFunc);
        DWORD ignored = 0;
        VirtualProtect(slotAddr, sizeof(void*), oldProt, &ignored);

        for (size_t i = 0; i < kMaxSlotOwners; ++i) {
            if (s_owners()[i].slotAddr == nullptr || s_owners()[i].slotAddr == slotAddr) {
                s_owners()[i].slotAddr = slotAddr;
                s_owners()[i].owner = this;
                s_owners()[i].original = m_originals[slotIndex];
                break;
            }
        }
        return true;
    }

    void notifySlotPatched(size_t slotIndex, void* replacement) {
        if (!m_initialized || slotIndex >= kTableSlots) return;
        std::lock_guard<std::mutex> lock(s_mutex());
        if (m_mode == HookMode::LiveCopy && m_liveStubs) {
            void* stubAddr = m_liveStubs + slotIndex * kStubSize;
            m_hookedSlots[slotIndex] = (replacement == stubAddr) ? nullptr : replacement;
        } else {
            m_hookedSlots[slotIndex] = (replacement == m_originals[slotIndex]) ? nullptr : replacement;
        }
    }

    bool reclaim() {
        if (!m_initialized || !m_instance) return false;
        std::lock_guard<std::mutex> lock(s_mutex());
        bool reclaimedAny = false;
        if (m_mode == HookMode::LiveCopy && m_privateTable) {
            void* curVptr = nullptr;
            if (safeReadPtr(m_instance, &curVptr) && curVptr != m_privateTable) {
                DWORD vptrProt = 0;
                if (VirtualProtect(m_instance, sizeof(void*), PAGE_READWRITE, &vptrProt)) {
                    if (safeWritePtr(m_instance, m_privateTable)) {
                        reclaimedAny = true;
                    }
                    DWORD ignored = 0;
                    VirtualProtect(m_instance, sizeof(void*), vptrProt, &ignored);
                }
            }
            return reclaimedAny;
        }

        if (!m_vtable) return false;
        for (size_t i = 0; i < kTableSlots; ++i) {
            void* hookFn = m_hookedSlots[i];
            if (!hookFn) continue;
            void* current = nullptr;
            if (!safeReadPtr(&m_vtable[i], &current)) continue;
            if (current && current != hookFn) {
                m_originals[i] = current;
                if (m_forwardSlots[i]) {
                    *m_forwardSlots[i] = current;
                }
                DWORD oldProt = 0;
                if (VirtualProtect(&m_vtable[i], sizeof(void*), PAGE_READWRITE, &oldProt)) {
                    InterlockedExchangePointer(
                        reinterpret_cast<PVOID volatile*>(&m_vtable[i]), hookFn);
                    DWORD ignored = 0;
                    VirtualProtect(&m_vtable[i], sizeof(void*), oldProt, &ignored);
                    reclaimedAny = true;
                }
            }
        }
        return reclaimedAny;
    }

    void unhookAll() {
        std::lock_guard<std::mutex> lock(s_mutex());
        if (!m_initialized) return;

        if (m_mode == HookMode::LiveCopy) {
            if (m_privateTable && m_liveStubs) {
                for (size_t i = 0; i < kTableSlots; ++i) {
                    if (m_hookedSlots[i]) {
                        void* stubAddr = m_liveStubs + i * kStubSize;
                        InterlockedExchangePointer(
                            reinterpret_cast<PVOID volatile*>(&m_privateTable[i]), stubAddr);
                    }
                }
            }
            void* curVptr = nullptr;
            if (m_instance && m_vtable && safeReadPtr(m_instance, &curVptr) && curVptr == m_privateTable) {
                DWORD vptrProt = 0;
                if (VirtualProtect(m_instance, sizeof(void*), PAGE_READWRITE, &vptrProt)) {
                    safeWritePtr(m_instance, m_vtable);
                    DWORD ignored = 0;
                    VirtualProtect(m_instance, sizeof(void*), vptrProt, &ignored);
                }
            }
            // Intentionally never VirtualFree m_liveAlloc: see reference vtable_hook design.
            // Any in-flight call or late table reference must never hit an unmapped page.
        } else if (m_vtable) {
            for (size_t i = 0; i < kTableSlots; ++i) {
                if (m_hookedSlots[i] && m_originals[i]) {
                    void** slotAddr = &m_vtable[i];
                    DWORD oldProt = 0;
                    if (VirtualProtect(slotAddr, sizeof(void*), PAGE_READWRITE, &oldProt)) {
                        InterlockedCompareExchangePointer(
                            reinterpret_cast<PVOID volatile*>(slotAddr),
                            m_originals[i],
                            m_hookedSlots[i]);
                        DWORD ignored = 0;
                        VirtualProtect(slotAddr, sizeof(void*), oldProt, &ignored);
                    }
                    for (size_t k = 0; k < kMaxSlotOwners; ++k) {
                        if (s_owners()[k].slotAddr == slotAddr && s_owners()[k].owner == this) {
                            s_owners()[k] = {};
                        }
                    }
                }
            }
        }

        std::memset(m_hookedSlots, 0, sizeof(m_hookedSlots));
        std::memset(m_originals, 0, sizeof(m_originals));
        std::memset(m_forwardSlots, 0, sizeof(m_forwardSlots));
        m_instance = nullptr;
        m_vtable = nullptr;
        m_initialized = false;
    }

    bool isInitialized() const { return m_initialized; }
    HookMode mode() const { return m_mode; }
    void* instance() const { return m_instance; }
    void** originalTable() const { return m_vtable; }
    void** activeTable() const { return (m_mode == HookMode::LiveCopy && m_privateTable) ? m_privateTable : m_vtable; }
    void* liveStubForSlot(size_t slotIndex) const {
        if (m_mode == HookMode::LiveCopy && m_liveStubs && slotIndex < kTableSlots) {
            return m_liveStubs + slotIndex * kStubSize;
        }
        return (m_vtable && slotIndex < kTableSlots) ? m_vtable[slotIndex] : nullptr;
    }

private:
    struct SlotOwner {
        void** slotAddr = nullptr;
        VTableHook* owner = nullptr;
        void* original = nullptr;
    };

    static std::mutex& s_mutex() {
        static std::mutex m;
        return m;
    }

    static SlotOwner (&s_owners())[kMaxSlotOwners] {
        static SlotOwner owners[kMaxSlotOwners]{};
        return owners;
    }

    void* m_instance = nullptr;
    void** m_vtable = nullptr;
    void** m_privateTable = nullptr;
    uint8_t* m_liveStubs = nullptr;
    uint8_t* m_liveAlloc = nullptr;
    size_t m_liveAllocBytes = 0;
    HookMode m_mode = HookMode::InPlace;
    bool m_initialized = false;
    bool m_validStubSlot[kTableSlots]{};
    void* m_hookedSlots[kTableSlots]{};
    void* m_originals[kTableSlots]{};
    void** m_forwardSlots[kTableSlots]{};
};

} // namespace edpe
