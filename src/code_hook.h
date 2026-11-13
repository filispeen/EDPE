#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <windows.h>

namespace edpe {

// x86-64 inline function prologue hook with RIP-relative displacement relocation
// and atomic 8-byte CompareExchange installation.
class CodeHook {
public:
    CodeHook() = default;
    ~CodeHook() { remove(); }

    CodeHook(const CodeHook&) = delete;
    CodeHook& operator=(const CodeHook&) = delete;

    static void* allocateNear(void* target, size_t size = 64) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(target);
        constexpr uintptr_t kMaxRange = 0x70000000ull; // 1.75 GB (< 2 GB rel32 limit)
        const uintptr_t minAddr = (base > kMaxRange) ? (base - kMaxRange) : 0x10000ull;
        const uintptr_t maxAddr = base + kMaxRange;

        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000ull;

        for (uintptr_t addr = (base & ~(gran - 1)); addr >= minAddr; addr -= gran) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) break;
            if (mbi.State == MEM_FREE && mbi.RegionSize >= size) {
                const uintptr_t candidate =
                    (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + gran - 1) & ~(gran - 1);
                if (candidate >= minAddr && candidate <= maxAddr) {
                    if (void* p = VirtualAlloc(reinterpret_cast<LPVOID>(candidate), size,
                            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) {
                        return p;
                    }
                }
            }
            if (addr < gran) break;
        }

        for (uintptr_t addr = ((base + gran - 1) & ~(gran - 1)); addr <= maxAddr; addr += gran) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) break;
            if (mbi.State == MEM_FREE && mbi.RegionSize >= size) {
                const uintptr_t candidate =
                    (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + gran - 1) & ~(gran - 1);
                if (candidate >= minAddr && candidate <= maxAddr) {
                    if (void* p = VirtualAlloc(reinterpret_cast<LPVOID>(candidate), size,
                            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) {
                        return p;
                    }
                }
            }
        }
        return nullptr;
    }

    // Decodes a single x86-64 prologue instruction length and outputs the offset of any
    // 32-bit RIP-relative displacement inside the instruction (or -1 if none).
    static size_t decodeInsnLen(const uint8_t* p, int* outRipDispOffset = nullptr) {
        if (outRipDispOffset) *outRipDispOffset = -1;
        size_t i = 0;

        while (p[i] == 0x66 || p[i] == 0xF2 || p[i] == 0xF3) ++i;
        if ((p[i] & 0xF0) == 0x40) ++i; // REX prefix

        const uint8_t op = p[i++];
        // Single-byte PUSH/POP r64 or NOP/RET
        if ((op >= 0x50 && op <= 0x5F) || op == 0x90 || op == 0xC3) {
            return i;
        }
        // MOV r64, imm64 (B8..BF with REX.W) or MOV r32, imm32
        if (op >= 0xB8 && op <= 0xBF) {
            const bool rexW = (i >= 2 && ((p[i - 2] & 0xF8) == 0x48));
            return i + (rexW ? 8 : 4);
        }
        // E8 call rel32 / E9 jmp rel32
        if (op == 0xE8 || op == 0xE9) {
            if (outRipDispOffset) *outRipDispOffset = static_cast<int>(i);
            return i + 4;
        }
        // EB jmp rel8 / 70..7F jcc rel8
        if (op == 0xEB || (op >= 0x70 && op <= 0x7F)) {
            return i + 1;
        }

        size_t immBytes = 0;
        if (op == 0x0F) {
            const uint8_t op2 = p[i++];
            if (op2 >= 0x80 && op2 <= 0x8F) {
                if (outRipDispOffset) *outRipDispOffset = static_cast<int>(i);
                return i + 4;
            }
            // Standard ModRM two-byte instructions (movups, movaps, xorps, nop, etc.)
        } else if (op == 0x80 || op == 0x82 || op == 0x83 || op == 0xC0 || op == 0xC1 || op == 0xC6) {
            immBytes = 1;
        } else if (op == 0x81 || op == 0xC7 || op == 0x69) {
            immBytes = 4;
        } else if (op == 0xF6) {
            immBytes = ((p[i] & 0x38) == 0) ? 1 : 0;
        } else if (op == 0xF7) {
            immBytes = ((p[i] & 0x38) == 0) ? 4 : 0;
        }

        // Parse ModRM + optional SIB + displacement
        const uint8_t modrm = p[i++];
        const uint8_t mod = (modrm >> 6) & 3;
        const uint8_t rm = modrm & 7;

        if (mod != 3 && rm == 4) {
            const uint8_t sib = p[i++];
            const uint8_t base = sib & 7;
            if (mod == 0 && base == 5) {
                return i + 4 + immBytes;
            }
        }
        if (mod == 0 && rm == 5) {
            if (outRipDispOffset) *outRipDispOffset = static_cast<int>(i);
            return i + 4 + immBytes;
        }
        if (mod == 1) return i + 1 + immBytes;
        if (mod == 2) return i + 4 + immBytes;
        return i + immBytes;
    }

    bool install(void* target, void* detour, void** originalOut) {
        if (!target || !detour || m_installed) return false;

        auto* src = static_cast<uint8_t*>(target);
        size_t stolen = 0;
        while (stolen < 5) {
            const size_t len = decodeInsnLen(src + stolen);
            if (len == 0 || stolen + len > 32) return false;
            stolen += len;
        }

        auto* tramp = static_cast<uint8_t*>(allocateNear(target, 64));
        if (!tramp) return false;

        // Copy stolen instructions and relocate any RIP-relative displacements
        size_t pos = 0;
        while (pos < stolen) {
            int ripDispOff = -1;
            const size_t len = decodeInsnLen(src + pos, &ripDispOff);
            std::memcpy(tramp + pos, src + pos, len);
            if (ripDispOff >= 0) {
                int32_t oldDisp = 0;
                std::memcpy(&oldDisp, src + pos + ripDispOff, sizeof(oldDisp));
                const uintptr_t targetAddr =
                    reinterpret_cast<uintptr_t>(src + pos + len) + static_cast<intptr_t>(oldDisp);
                const intptr_t newDisp =
                    static_cast<intptr_t>(targetAddr) - reinterpret_cast<intptr_t>(tramp + pos + len);
                if (newDisp < INT32_MIN || newDisp > INT32_MAX) {
                    VirtualFree(tramp, 0, MEM_RELEASE);
                    return false;
                }
                const int32_t rel32 = static_cast<int32_t>(newDisp);
                std::memcpy(tramp + pos + ripDispOff, &rel32, sizeof(rel32));
            }
            pos += len;
        }

        // Append absolute 14-byte jump back to (src + stolen): FF 25 00 00 00 00 <8-byte addr>
        const uintptr_t resumeAddr = reinterpret_cast<uintptr_t>(src + stolen);
        tramp[stolen + 0] = 0xFF;
        tramp[stolen + 1] = 0x25;
        tramp[stolen + 2] = 0x00;
        tramp[stolen + 3] = 0x00;
        tramp[stolen + 4] = 0x00;
        tramp[stolen + 5] = 0x00;
        std::memcpy(tramp + stolen + 6, &resumeAddr, sizeof(resumeAddr));
        FlushInstructionCache(GetCurrentProcess(), tramp, 64);

        const intptr_t jmpDelta =
            reinterpret_cast<intptr_t>(detour) - reinterpret_cast<intptr_t>(src + 5);
        if (jmpDelta < INT32_MIN || jmpDelta > INT32_MAX) {
            VirtualFree(tramp, 0, MEM_RELEASE);
            return false;
        }

        DWORD oldProt = 0;
        if (!VirtualProtect(src, 8, PAGE_EXECUTE_READWRITE, &oldProt)) {
            VirtualFree(tramp, 0, MEM_RELEASE);
            return false;
        }

        std::memcpy(&m_savedBytes8, src, sizeof(m_savedBytes8));
        uint8_t patched8[8];
        std::memcpy(patched8, &m_savedBytes8, sizeof(patched8));
        patched8[0] = 0xE9;
        const int32_t rel32 = static_cast<int32_t>(jmpDelta);
        std::memcpy(patched8 + 1, &rel32, sizeof(rel32));
        for (size_t k = 5; k < stolen && k < 8; ++k) {
            patched8[k] = 0x90; // NOP padding
        }

        int64_t expected64 = 0, desired64 = 0;
        std::memcpy(&expected64, &m_savedBytes8, sizeof(expected64));
        std::memcpy(&desired64, patched8, sizeof(desired64));
        const int64_t prev = InterlockedCompareExchange64(
            reinterpret_cast<volatile LONG64*>(src), desired64, expected64);

        DWORD ignored = 0;
        VirtualProtect(src, 8, oldProt, &ignored);
        if (prev != expected64) {
            VirtualFree(tramp, 0, MEM_RELEASE);
            return false;
        }

        FlushInstructionCache(GetCurrentProcess(), src, 8);
        m_target = src;
        m_trampoline = tramp;
        m_stolenLen = stolen;
        m_installed = true;
        if (originalOut) *originalOut = tramp;
        return true;
    }

    void remove() {
        if (!m_installed || !m_target) return;
        DWORD oldProt = 0;
        if (VirtualProtect(m_target, 8, PAGE_EXECUTE_READWRITE, &oldProt)) {
            int64_t cur64 = *reinterpret_cast<volatile LONG64*>(m_target);
            int64_t orig64 = 0;
            std::memcpy(&orig64, &m_savedBytes8, sizeof(orig64));
            InterlockedCompareExchange64(
                reinterpret_cast<volatile LONG64*>(m_target), orig64, cur64);
            DWORD ignored = 0;
            VirtualProtect(m_target, 8, oldProt, &ignored);
            FlushInstructionCache(GetCurrentProcess(), m_target, 8);
        }
        if (m_trampoline) {
            VirtualFree(m_trampoline, 0, MEM_RELEASE);
            m_trampoline = nullptr;
        }
        m_target = nullptr;
        m_stolenLen = 0;
        m_installed = false;
    }

    bool isInstalled() const { return m_installed; }
    void* trampoline() const { return m_trampoline; }

private:
    uint8_t* m_target = nullptr;
    uint8_t* m_trampoline = nullptr;
    uint64_t m_savedBytes8 = 0;
    size_t m_stolenLen = 0;
    bool m_installed = false;
};

} // namespace edpe
