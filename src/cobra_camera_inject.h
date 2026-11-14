#pragma once

#include "code_hook.h"
#include "elite_camera.h"
#include "log.h"
#include "temporal_math.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <d3d11.h>
#include <shlobj.h>
#include <windows.h>

namespace edpe::cobra_camera {

// RVA constants in EliteDangerous64.exe (Odyssey 4.3.1.1):
//   0x05921F0 : view-constant refresh entry (FUN_1405921f0)
//   0x0592300 : mid-function tangent / matrix finalizer site
//   0x1096640 : ICameraTargetShip::SetTarget (cockpit vehicle rig attach)
constexpr uintptr_t kViewConstRefreshEntryRva = 0x05921F0;
constexpr uintptr_t kViewConstRefreshSiteRva  = 0x0592300;
constexpr uintptr_t kShipSetTargetRva         = 0x1096640;

// Deferred lighting shader hashes (Pass 35 / Deferred lighting CB2 slot 2):
constexpr uint64_t kDeferredLightingVsHash = 0x7E38A6AA1269C901ull;
constexpr uint64_t kDeferredLightingPsHash = 0x7CECABDE34FFBE9Eull;

// Full-screen map / holo-canvas shader hashes (Galaxy Map, System Map, Orrery, FSS):
constexpr uint64_t kMapCanvasVsHash = 0x12382D2EA45E9632ull;
constexpr uint64_t kMapCanvasPsHash = 0x855C469156AB997Full;
constexpr uint64_t kMapSpriteVsHash = 0xC31238331D8AC3D4ull;

// Thread-local state for ABI-safe stubA / stubB return-address interception.
// `realRet` MUST remain at offset 0 for stubB's `mov rcx, [rax+0]` instruction.
struct RefreshTls {
    void* realRet = nullptr;       // offset 0
    uint8_t* camera = nullptr;     // offset 8
    float savedBoundX = 0.0f;      // offset 16
    float savedBoundY = 0.0f;      // offset 20
    uint32_t savedFlags = 0;       // offset 24
    uint32_t layoutVariant = 0;    // offset 28 (1 = +0x28C/+0x290, 2 = +0x228/+0x22C)
};

inline __declspec(thread) RefreshTls t_refreshTls{};

struct FrameJitterWindow {
    std::atomic<DWORD> renderThreadId{0};
    std::atomic<bool> windowOpen{false};
    std::atomic<bool> mapModeActive{false};
    std::atomic<uint32_t> renderWidth{0};
    std::atomic<uint32_t> renderHeight{0};
    std::atomic<float> jitterPxX{0.0f};
    std::atomic<float> jitterPxY{0.0f};
    std::atomic<uint64_t> injectedFrameCount{0};
    std::atomic<uint32_t> injectedDrawsThisFrame{0};
};

inline FrameJitterWindow& frameWindow() {
    static FrameJitterWindow w;
    return w;
}

inline RefreshTls* __fastcall getRefreshTls() {
    return &t_refreshTls;
}

// Modifies the camera frustum tangent bounds (boundX, boundY) and raises the dirty flags
// (4 | 8) right before the engine's own matrix finalizers (FUN_1404f2ff0, FUN_1404f49f0,
// FUN_140596830) compute all forward/inverse matrices and inverse frustum rays.
inline bool applyTransientCameraBoundJitter(uint8_t* camera, RefreshTls& tls) {
    if (!camera) return false;
    auto& win = frameWindow();
    if (!win.windowOpen.load(std::memory_order_acquire)) return false;
    if (win.mapModeActive.load(std::memory_order_relaxed)) return false;

    const DWORD tid = win.renderThreadId.load(std::memory_order_relaxed);
    if (tid != 0 && GetCurrentThreadId() != tid) return false;

    const uint32_t rw = win.renderWidth.load(std::memory_order_relaxed);
    const uint32_t rh = win.renderHeight.load(std::memory_order_relaxed);
    if (rw == 0 || rh == 0) return false;

    const float jx = win.jitterPxX.load(std::memory_order_relaxed);
    const float jy = win.jitterPxY.load(std::memory_order_relaxed);
    if (std::fabs(jx) < 1e-6f && std::fabs(jy) < 1e-6f) return false;

    // Check layout variant 1 (+0x264 kind, +0x28C/+0x290 bounds, +0x250 flags)
    // and layout variant 2 (+0x240/+0x244 kind, +0x228/+0x22C bounds, +0x250 flags).
    uint32_t kind1 = 0;
    std::memcpy(&kind1, camera + 0x264, sizeof(kind1));
    float bx1 = 0.0f, by1 = 0.0f;
    std::memcpy(&bx1, camera + 0x28C, sizeof(bx1));
    std::memcpy(&by1, camera + 0x290, sizeof(by1));

    uint32_t kind2a = 0, kind2b = 0;
    std::memcpy(&kind2a, camera + 0x240, sizeof(kind2a));
    std::memcpy(&kind2b, camera + 0x244, sizeof(kind2b));
    float bx2 = 0.0f, by2 = 0.0f;
    std::memcpy(&bx2, camera + 0x228, sizeof(bx2));
    std::memcpy(&by2, camera + 0x22C, sizeof(by2));

    auto validBounds = [&](float bx, float by) -> bool {
        if (!std::isfinite(bx) || !std::isfinite(by)) return false;
        if (std::fabs(bx) < 0.05f || std::fabs(bx) > 5.0f) return false;
        if (std::fabs(by) < 0.05f || std::fabs(by) > 5.0f) return false;
        const float camAspect = std::fabs(bx / by);
        const float targetAspect = static_cast<float>(rw) / static_cast<float>(rh);
        return std::fabs(camAspect - targetAspect) <= 0.12f * targetAspect;
    };

    size_t offBx = 0, offBy = 0;
    uint32_t variant = 0;
    if (kind1 == 3 && validBounds(bx1, by1)) {
        offBx = 0x28C;
        offBy = 0x290;
        variant = 1;
        tls.savedBoundX = bx1;
        tls.savedBoundY = by1;
    } else if ((kind2a == 3 || kind2b == 3) && validBounds(bx2, by2)) {
        offBx = 0x228;
        offBy = 0x22C;
        variant = 2;
        tls.savedBoundX = bx2;
        tls.savedBoundY = by2;
    } else {
        return false;
    }

    uint32_t flags = 0;
    std::memcpy(&flags, camera + 0x250, sizeof(flags));
    tls.camera = camera;
    tls.savedFlags = flags;
    tls.layoutVariant = variant;

    const float newBx = tls.savedBoundX + jx / static_cast<float>(rw);
    const float newBy = tls.savedBoundY - jy / static_cast<float>(rh);
    const uint32_t newFlags = flags | 0x4u | 0x8u;

    std::memcpy(camera + offBx, &newBx, sizeof(newBx));
    std::memcpy(camera + offBy, &newBy, sizeof(newBy));
    std::memcpy(camera + 0x250, &newFlags, sizeof(newFlags));

    win.injectedDrawsThisFrame.fetch_add(1, std::memory_order_relaxed);
    win.injectedFrameCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

inline void __fastcall refreshPost() {
    RefreshTls& tls = t_refreshTls;
    if (tls.camera && tls.layoutVariant != 0) {
        const size_t offBx = (tls.layoutVariant == 1) ? 0x28Cu : 0x228u;
        const size_t offBy = (tls.layoutVariant == 1) ? 0x290u : 0x22Cu;
        std::memcpy(tls.camera + offBx, &tls.savedBoundX, sizeof(tls.savedBoundX));
        std::memcpy(tls.camera + offBy, &tls.savedBoundY, sizeof(tls.savedBoundY));
        std::memcpy(tls.camera + 0x250, &tls.savedFlags, sizeof(tls.savedFlags));
    }
    tls.camera = nullptr;
    tls.layoutVariant = 0;
}

inline void* g_stubBAddr = nullptr;

inline void* __fastcall refreshPre(uint8_t* camera, void* realRet) {
    RefreshTls& tls = t_refreshTls;
    tls.realRet = realRet;
    tls.camera = nullptr;
    tls.layoutVariant = 0;
    if (g_stubBAddr && applyTransientCameraBoundJitter(camera, tls)) {
        return g_stubBAddr;
    }
    return realRet;
}

// Emits ABI-safe stubA (entry trampoline with 32-byte Win64 shadow space) and
// stubB (return epilogue trampoline that preserves RAX and XMM0 and restores camera bounds).
inline bool buildRefreshStubs(uint8_t* page, void* resumeTrampoline, uint8_t** outStubA, uint8_t** outStubB) {
    if (!page || !resumeTrampoline) return false;
    uint8_t* stubA = page;
    uint8_t* stubB = page + 128;

    // stubA:
    //   Save volatile argument registers RCX, RDX, R8, R9 into caller's home space:
    //   [rsp+8]  = rcx (camera)
    //   [rsp+16] = rdx
    //   [rsp+24] = r8
    //   [rsp+32] = r9
    //   mov rdx, [rsp]      (realRet)
    //   sub rsp, 0x20       (32-byte shadow space for refreshPre, 16B-aligned)
    //   mov rax, &refreshPre
    //   call rax            (returns either realRet or stubB in RAX)
    //   add rsp, 0x20
    //   mov [rsp], rax      (replace return address on stack)
    //   restore rcx, rdx, r8, r9
    //   jmp resumeTrampoline
    size_t a = 0;
    auto emitA = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) stubA[a++] = b;
    };
    emitA({0x48, 0x89, 0x4C, 0x24, 0x08}); // mov [rsp+0x08], rcx
    emitA({0x48, 0x89, 0x54, 0x24, 0x10}); // mov [rsp+0x10], rdx
    emitA({0x4C, 0x89, 0x44, 0x24, 0x18}); // mov [rsp+0x18], r8
    emitA({0x4C, 0x89, 0x4C, 0x24, 0x20}); // mov [rsp+0x20], r9
    emitA({0x48, 0x8B, 0x14, 0x24});       // mov rdx, [rsp]
    emitA({0x48, 0x83, 0xEC, 0x20});       // sub rsp, 0x20
    emitA({0x48, 0xB8});                   // mov rax, imm64 (&refreshPre)
    const uintptr_t preAddr = reinterpret_cast<uintptr_t>(&refreshPre);
    std::memcpy(stubA + a, &preAddr, sizeof(preAddr));
    a += 8;
    emitA({0xFF, 0xD0});                   // call rax
    emitA({0x48, 0x83, 0xC4, 0x20});       // add rsp, 0x20
    emitA({0x48, 0x89, 0x04, 0x24});       // mov [rsp], rax
    emitA({0x48, 0x8B, 0x4C, 0x24, 0x08}); // mov rcx, [rsp+0x08]
    emitA({0x48, 0x8B, 0x54, 0x24, 0x10}); // mov rdx, [rsp+0x10]
    emitA({0x4C, 0x8B, 0x44, 0x24, 0x18}); // mov r8,  [rsp+0x18]
    emitA({0x4C, 0x8B, 0x4C, 0x24, 0x20}); // mov r9,  [rsp+0x20]
    emitA({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp qword ptr [rip+0]
    const uintptr_t resumeVal = reinterpret_cast<uintptr_t>(resumeTrampoline);
    std::memcpy(stubA + a, &resumeVal, sizeof(resumeVal));
    a += 8;

    // stubB (entered via `ret` from FUN_1405921f0, so RSP % 16 == 8):
    //   sub rsp, 0x38                    (aligns RSP to 16B, reserves 0x20 shadow + 0x18 save area)
    //   mov [rsp+0x30], rax              (preserve integer return value)
    //   movaps [rsp+0x20], xmm0          (preserve float return value)
    //   mov rax, &refreshPost
    //   call rax
    //   mov rax, &getRefreshTls
    //   call rax
    //   mov rcx, [rax]                   (realRet at offset 0 of RefreshTls)
    //   movaps xmm0, [rsp+0x20]
    //   mov rax, [rsp+0x30]
    //   add rsp, 0x38
    //   jmp rcx
    size_t b = 0;
    auto emitB = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t by : bytes) stubB[b++] = by;
    };
    emitB({0x48, 0x83, 0xEC, 0x38});             // sub rsp, 0x38
    emitB({0x48, 0x89, 0x44, 0x24, 0x30});       // mov [rsp+0x30], rax
    emitB({0x0F, 0x29, 0x44, 0x24, 0x20});       // movaps [rsp+0x20], xmm0
    emitB({0x48, 0xB8});                         // mov rax, imm64 (&refreshPost)
    const uintptr_t postAddr = reinterpret_cast<uintptr_t>(&refreshPost);
    std::memcpy(stubB + b, &postAddr, sizeof(postAddr));
    b += 8;
    emitB({0xFF, 0xD0});                         // call rax
    emitB({0x48, 0xB8});                         // mov rax, imm64 (&getRefreshTls)
    const uintptr_t tlsAddr = reinterpret_cast<uintptr_t>(&getRefreshTls);
    std::memcpy(stubB + b, &tlsAddr, sizeof(tlsAddr));
    b += 8;
    emitB({0xFF, 0xD0});                         // call rax
    emitB({0x48, 0x8B, 0x08});                   // mov rcx, [rax]
    emitB({0x0F, 0x28, 0x44, 0x24, 0x20});       // movaps xmm0, [rsp+0x20]
    emitB({0x48, 0x8B, 0x44, 0x24, 0x30});       // mov rax, [rsp+0x30]
    emitB({0x48, 0x83, 0xC4, 0x38});             // add rsp, 0x38
    emitB({0xFF, 0xE1});                         // jmp rcx

    g_stubBAddr = stubB;
    if (outStubA) *outStubA = stubA;
    if (outStubB) *outStubB = stubB;
    FlushInstructionCache(GetCurrentProcess(), page, 256);
    return true;
}

// ============================================================================
// Inverse Frustum Ray & Inverse Clip Jitter Math (CB0 rows 98..101 & CB2 rows 14..16)
// ============================================================================
// Shifts an inverse UV frustum ray block (4 float4 rows: dX, dY, originCorner, wRow)
// by subpixel offset (dx, dy) in normalized UV units (dx = jx / R_w, dy = jy / R_h):
//   originCorner' = originCorner - dx * dX - dy * dY
inline void jitterInverseUvRay(float* rows4x4, float dxUv, float dyUv) {
    if (!rows4x4) return;
    float* dX = rows4x4 + 0;
    float* dY = rows4x4 + 4;
    float* origin = rows4x4 + 8;
    for (int c = 0; c < 3; ++c) {
        origin[c] -= dxUv * dX[c] + dyUv * dY[c];
    }
}

// Shifts an inverse clip-space matrix (4 float4 rows: colX, colY, colZ, colW)
// by NDC jitter (ndcX, ndcY):
//   colZ' = colZ - ndcX * colX - ndcY * colY
inline void jitterInverseClip(float* rows4x4, float ndcX, float ndcY) {
    if (!rows4x4) return;
    float* colX = rows4x4 + 0;
    float* colY = rows4x4 + 4;
    float* colZ = rows4x4 + 8;
    for (int c = 0; c < 4; ++c) {
        colZ[c] -= ndcX * colX[c] + ndcY * colY[c];
    }
}

// ============================================================================
// Deferred Lighting CB2 Jitter Phase Verification & Repair
// (VS 0x7E38A6AA1269C901 / PS 0x7CECABDE34FFBE9E, slot 2, b2[10..16])
// ============================================================================
// Allowances for float32 rounding at 2560x1440 (28-row cascade):
inline float flatDeferredFloatBound(const float* row4) {
    const float ax = std::fabs(row4[0]);
    const float ay = std::fabs(row4[1]);
    const float az = std::fabs(row4[2]);
    const float aw = std::fabs(row4[3]);
    const float m = std::max(std::max(ax, ay), std::max(az, aw));
    return std::max(m * 1.5e-5f, 2.0e-6f);
}

inline float flatDeferredFrustumScale(const float* b2Rows10to16) {
    const float* dX = b2Rows10to16 + 4 * 4; // row 14
    const float* dY = b2Rows10to16 + 5 * 4; // row 15
    const float lenX = std::sqrt(dX[0] * dX[0] + dX[1] * dX[1] + dX[2] * dX[2]);
    const float lenY = std::sqrt(dY[0] * dY[0] + dY[1] * dY[1] + dY[2] * dY[2]);
    return std::max({lenX, lenY, 1.0f});
}

inline float flatDeferredWorldAllowance(const float* b2Rows10to16) {
    const float* orig = b2Rows10to16 + 6 * 4; // row 16
    const float mag = std::sqrt(orig[0] * orig[0] + orig[1] * orig[1] + orig[2] * orig[2]);
    return std::max(flatDeferredFrustumScale(b2Rows10to16) * 2.5e-4f, mag * 5.0e-5f);
}

// Case A: Forward rows b2[10..13] already carry the jittered view-projection matrix,
//         while inverse frustum rays b2[14..16] were populated from an unjittered copy.
inline bool repairDeferredMissingPhase(
    float* b2Floats,
    size_t floatCount,
    const float* jitteredForward16,
    const float* unjitteredInverseRays12,
    float dxUv,
    float dyUv) {
    if (!b2Floats || floatCount < 17 * 4 || !jitteredForward16 || !unjitteredInverseRays12) {
        return false;
    }
    if (std::fabs(dxUv) <= 1e-7f && std::fabs(dyUv) <= 1e-7f) {
        return false;
    }
    float* rows10to16 = b2Floats + 10 * 4;
    // Verify rows 10..13 match jitteredForward16 within float32 bound
    for (int r = 0; r < 4; ++r) {
        const float* actual = rows10to16 + r * 4;
        const float* expected = jitteredForward16 + r * 4;
        const float tol = flatDeferredFloatBound(expected);
        for (int c = 0; c < 4; ++c) {
            if (std::fabs(actual[c] - expected[c]) > tol * 8.0f) {
                return false;
            }
        }
    }
    // Check if rows 14..16 match unjitteredInverseRays12 (and are closer to unjittered than already-jittered)
    float expectedJittered12[12]{};
    std::memcpy(expectedJittered12, unjitteredInverseRays12, sizeof(expectedJittered12));
    jitterInverseUvRay(expectedJittered12, dxUv, dyUv);

    const float allow = flatDeferredWorldAllowance(rows10to16);
    float diffUnjittered = 0.0f;
    float diffJittered = 0.0f;
    for (int i = 0; i < 12; ++i) {
        diffUnjittered = std::max(diffUnjittered,
            std::fabs(rows10to16[16 + i] - unjitteredInverseRays12[i]));
        diffJittered = std::max(diffJittered,
            std::fabs(rows10to16[16 + i] - expectedJittered12[i]));
    }
    if (diffUnjittered > allow || !(diffUnjittered < diffJittered * 0.5f)) {
        return false;
    }
    jitterInverseUvRay(rows10to16 + 16, dxUv, dyUv);
    return true;
}

// Case B: Supercruise / planetary flight where the entire b2[10..16] camera block was
//         built without the current frame's subpixel jitter phase.
inline bool repairDeferredWholeOmission(
    float* b2Floats,
    size_t floatCount,
    const float* unjitteredForward16,
    const ProjectionJitter& jitter,
    float dxUv,
    float dyUv) {
    if (!b2Floats || floatCount < 17 * 4 || !unjitteredForward16) {
        return false;
    }
    float* rows10to16 = b2Floats + 10 * 4;
    for (int r = 0; r < 4; ++r) {
        const float* actual = rows10to16 + r * 4;
        const float* expected = unjitteredForward16 + r * 4;
        const float tol = flatDeferredFloatBound(expected);
        for (int c = 0; c < 4; ++c) {
            if (std::fabs(actual[c] - expected[c]) > tol * 8.0f) {
                return false;
            }
        }
    }
    float jitteredForward[16]{};
    if (!jitterEliteProjectionBlock(rows10to16, jitteredForward, jitter)) {
        return false;
    }
    std::memcpy(rows10to16, jitteredForward, sizeof(jitteredForward));
    jitterInverseUvRay(rows10to16 + 16, dxUv, dyUv);
    return true;
}

// ============================================================================
// EDO Row-Major & Inverse Projection Jitter Repair Helpers
// Verified against Elite Dangerous Odyssey D3D11 constant buffers:
//   - Deferred lighting & shadowed local lights CB2 (752..784B):
//       b2[10..13] = row-major Proj*View (depthB at float index 51)
//       b2[14..16] = row-major world-space inverse UV rays (origin in .w)
//   - Cascade shadow mask CB2 (592B):
//       b2[0]      = (W, H, 1/W, 1/H)
//       b2[1..4]   = column-major InvProj (ndcX * cb2[1] + ndcY * cb2[2] + cb2[4])
//   - Clustered lighting CS cb0 (480B) & Main CB1[237..240]:
//       row-major UvToView (origin in .w)
// ============================================================================
inline void jitterRowMajorInverseUvRay(float* rows3x4, float dxUv, float dyUv) {
    if (!rows3x4) return;
    for (int r = 0; r < 3; ++r) {
        float* row = rows3x4 + r * 4;
        row[3] -= dxUv * row[0] + dyUv * row[1];
    }
}

inline void jitterColumnMajorInverseClip(float* cols4x4, float ndcX, float ndcY) {
    if (!cols4x4) return;
    const float* colX = cols4x4 + 0;
    const float* colY = cols4x4 + 4;
    float* colW = cols4x4 + 12;
    for (int c = 0; c < 3; ++c) {
        colW[c] -= ndcX * colX[c] + ndcY * colY[c];
    }
}

inline void jitterRowMajorInverseClip(float* rows4x4, float ndcX, float ndcY) {
    if (!rows4x4) return;
    for (int r = 0; r < 4; ++r) {
        float* row = rows4x4 + r * 4;
        row[3] -= ndcX * row[0] + ndcY * row[1];
    }
}

inline void transposeBlock4x4(const float* in16, float* out16) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out16[r * 4 + c] = in16[c * 4 + r];
        }
    }
}

inline bool matchMatrix4x4(const float* actual16, const float* expected16) {
    for (int r = 0; r < 4; ++r) {
        const float* a = actual16 + r * 4;
        const float* e = expected16 + r * 4;
        const float tol = flatDeferredFloatBound(e) * 8.0f;
        for (int c = 0; c < 4; ++c) {
            if (!std::isfinite(a[c]) || std::fabs(a[c] - e[c]) > tol) {
                return false;
            }
        }
    }
    return true;
}

// Verifies row-major cb2[14..16] against forward vector cb2[13].xyz using
// the exact Cobra identity: 0.5*dX + 0.5*dY + origin == cb2[13].xyz.
inline bool rowMajorInverseRaysNeedJitter(const float* rows10to16, float dxUv, float dyUv) {
    const float* forwardVec = rows10to16 + 3 * 4; // row 13 (.xyz)
    const float* r14 = rows10to16 + 4 * 4;
    const float* r15 = rows10to16 + 5 * 4;
    const float* r16 = rows10to16 + 6 * 4;
    for (int i = 0; i < 12; ++i) {
        if (!std::isfinite(r14[i])) return false;
    }
    const float rows[3][4] = {
        {r14[0], r14[1], r14[2], r14[3]},
        {r15[0], r15[1], r15[2], r15[3]},
        {r16[0], r16[1], r16[2], r16[3]},
    };
    float lenX2 = 0.0f, lenY2 = 0.0f;
    for (int c = 0; c < 3; ++c) {
        lenX2 += rows[c][0] * rows[c][0];
        lenY2 += rows[c][1] * rows[c][1];
    }
    if (lenX2 < 1e-4f || lenY2 < 1e-4f) return false;
    const float scale = std::max({std::sqrt(lenX2), std::sqrt(lenY2), 1.0f});
    const float allow = std::max(scale * 2.5e-4f, 1e-4f);

    float diffUnjittered = 0.0f;
    float diffJittered = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const float unjitOrig = forwardVec[c] - 0.5f * rows[c][0] - 0.5f * rows[c][1];
        const float jitOrig = unjitOrig - dxUv * rows[c][0] - dyUv * rows[c][1];
        diffUnjittered = std::max(diffUnjittered, std::fabs(rows[c][3] - unjitOrig));
        diffJittered = std::max(diffJittered, std::fabs(rows[c][3] - jitOrig));
    }
    return diffUnjittered <= allow && diffUnjittered < diffJittered * 0.5f;
}

inline bool repairRowMajorDeferredMissingPhase(
    float* b2Floats,
    size_t floatCount,
    const float* colMajorJitteredForward16,
    float dxUv,
    float dyUv) {
    if (!b2Floats || floatCount < 17 * 4 || !colMajorJitteredForward16) return false;
    if (std::fabs(dxUv) <= 1e-7f && std::fabs(dyUv) <= 1e-7f) return false;

    float expectedRowMajor[16]{};
    transposeBlock4x4(colMajorJitteredForward16, expectedRowMajor);
    float* rows10to16 = b2Floats + 10 * 4;
    if (!matchMatrix4x4(rows10to16, expectedRowMajor)) return false;
    if (!rowMajorInverseRaysNeedJitter(rows10to16, dxUv, dyUv)) return false;

    jitterRowMajorInverseUvRay(rows10to16 + 16, dxUv, dyUv);
    return true;
}

inline bool repairRowMajorDeferredWholeOmission(
    float* b2Floats,
    size_t floatCount,
    const float* colMajorUnjitteredForward16,
    const ProjectionJitter& jitter,
    float dxUv,
    float dyUv) {
    if (!b2Floats || floatCount < 17 * 4 || !colMajorUnjitteredForward16) return false;
    if (std::fabs(dxUv) <= 1e-7f && std::fabs(dyUv) <= 1e-7f &&
        std::fabs(jitter.ndcX) <= 1e-7f && std::fabs(jitter.ndcY) <= 1e-7f) return false;

    float expectedRowMajor[16]{};
    transposeBlock4x4(colMajorUnjitteredForward16, expectedRowMajor);
    float* rows10to16 = b2Floats + 10 * 4;
    if (!matchMatrix4x4(rows10to16, expectedRowMajor)) return false;

    float jitteredRowMajor[16]{};
    if (!jitterEliteDotProjectionBlock(rows10to16, jitteredRowMajor, jitter)) return false;

    const bool patchRays = rowMajorInverseRaysNeedJitter(rows10to16, dxUv, dyUv);
    std::memcpy(rows10to16, jitteredRowMajor, sizeof(jitteredRowMajor));
    if (patchRays) {
        jitterRowMajorInverseUvRay(rows10to16 + 16, dxUv, dyUv);
    }
    return true;
}

inline bool repairCascadeShadowMaskInvProj(
    float* cbFloats,
    size_t floatCount,
    const CameraProjection& refCam,
    const ProjectionJitter& jitter) {
    if (!cbFloats || floatCount < 20) return false;
    if (refCam.scaleX <= 0.1f || refCam.scaleY <= 0.1f || refCam.depthB < 0.01f) return false;
    if (std::fabs(jitter.ndcX) <= 1e-7f && std::fabs(jitter.ndcY) <= 1e-7f) return false;
    for (int i = 0; i < 20; ++i) {
        if (!std::isfinite(cbFloats[i])) return false;
    }
    const float w = cbFloats[0];
    const float h = cbFloats[1];
    if (w < 320.0f || h < 240.0f ||
        std::fabs(cbFloats[2] - 1.0f / w) > 1e-4f ||
        std::fabs(cbFloats[3] - 1.0f / h) > 1e-4f) {
        return false;
    }
    const float invSx = 1.0f / refCam.scaleX;
    const float invSy = 1.0f / refCam.scaleY;
    const float invDb = 1.0f / refCam.depthB;
    const float* m = cbFloats + 4;
    if (std::fabs(m[0] - invSx) > 0.02f * invSx ||
        std::fabs(m[1]) > 1e-3f || std::fabs(m[2]) > 1e-3f || std::fabs(m[3]) > 1e-3f ||
        std::fabs(m[4]) > 1e-3f || std::fabs(m[5] - invSy) > 0.02f * invSy ||
        std::fabs(m[6]) > 1e-3f || std::fabs(m[7]) > 1e-3f ||
        std::fabs(m[8]) > 1e-3f || std::fabs(m[9]) > 1e-3f || std::fabs(m[10]) > 1e-3f ||
        std::fabs(m[11] - invDb) > 0.05f * invDb ||
        std::fabs(m[14] - 1.0f) > 1e-3f || std::fabs(m[15]) > 1e-3f) {
        return false;
    }
    if (std::fabs(m[12]) > 1e-5f || std::fabs(m[13]) > 1e-5f) {
        return false;
    }
    jitterColumnMajorInverseClip(cbFloats + 4, jitter.ndcX, jitter.ndcY);
    return true;
}

inline bool matchesRowMajorUvToViewBlock(
    const float* rows4x4,
    const CameraProjection& refCam) {
    if (!rows4x4 || refCam.scaleX <= 0.1f || refCam.scaleY <= 0.1f || refCam.depthB < 0.01f) {
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(rows4x4[i])) return false;
    }
    const float invSx = 1.0f / refCam.scaleX;
    const float invSy = 1.0f / refCam.scaleY;
    const float invDb = 1.0f / refCam.depthB;
    return std::fabs(rows4x4[0] - 2.0f * invSx) <= 0.02f * (2.0f * invSx) &&
           std::fabs(rows4x4[1]) <= 1e-3f && std::fabs(rows4x4[2]) <= 1e-3f &&
           std::fabs(rows4x4[4]) <= 1e-3f &&
           std::fabs(rows4x4[5] + 2.0f * invSy) <= 0.02f * (2.0f * invSy) &&
           std::fabs(rows4x4[6]) <= 1e-3f &&
           std::fabs(rows4x4[8]) <= 1e-3f && std::fabs(rows4x4[9]) <= 1e-3f &&
           std::fabs(rows4x4[10]) <= 1e-3f && std::fabs(rows4x4[11] - 1.0f) <= 1e-3f &&
           std::fabs(rows4x4[12]) <= 1e-3f && std::fabs(rows4x4[13]) <= 1e-3f &&
           std::fabs(rows4x4[14] - invDb) <= 0.05f * invDb && std::fabs(rows4x4[15]) <= 1e-3f;
}

inline bool repairRowMajorUvToViewBlock(
    float* rows4x4,
    const CameraProjection& refCam,
    float dxUv,
    float dyUv) {
    if (!matchesRowMajorUvToViewBlock(rows4x4, refCam)) return false;
    if (std::fabs(dxUv) <= 1e-7f && std::fabs(dyUv) <= 1e-7f) return false;
    // Only jitter if unjittered center (u=0.5, v=0.5) maps to view ray ~(0, 0).
    if (std::fabs(0.5f * rows4x4[0] + rows4x4[3]) > 1e-4f ||
        std::fabs(0.5f * rows4x4[5] + rows4x4[7]) > 1e-4f) {
        return false;
    }
    jitterRowMajorInverseUvRay(rows4x4, dxUv, dyUv);
    return true;
}

inline bool unjitterRowMajorUvToViewBlock(
    float* rows4x4,
    const CameraProjection& refCam) {
    if (!matchesRowMajorUvToViewBlock(rows4x4, refCam)) return false;
    const float centerX = 0.5f * rows4x4[0] + rows4x4[3];
    const float centerY = 0.5f * rows4x4[5] + rows4x4[7];
    if (std::fabs(centerX) > 0.05f * rows4x4[0] || std::fabs(centerY) > 0.05f * std::fabs(rows4x4[5])) {
        return false;
    }
    if (std::fabs(centerX) <= 1e-6f && std::fabs(centerY) <= 1e-6f) {
        return false;
    }
    rows4x4[3] = -0.5f * rows4x4[0];
    rows4x4[7] = -0.5f * rows4x4[5];
    return true;
}

// ============================================================================
// Cockpit Vehicle Rig Tracker (ICameraTargetShip::SetTarget RVA 0x1096640)
// ============================================================================
struct VehicleRigState {
    std::atomic<const void*> activeShipTarget{nullptr};
    float prevShipWorld[12]{};
    float currShipWorld[12]{};
    bool hasValidRig = false;
};

inline VehicleRigState& vehicleRig() {
    static VehicleRigState s;
    return s;
}

using ShipSetTargetFn = void*(__fastcall*)(void* thisPtr, const void* targetEntity);
inline ShipSetTargetFn g_origShipSetTarget = nullptr;

inline void* __fastcall detourShipSetTarget(void* thisPtr, const void* targetEntity) {
    if (targetEntity) {
        vehicleRig().activeShipTarget.store(targetEntity, std::memory_order_relaxed);
    }
    return g_origShipSetTarget ? g_origShipSetTarget(thisPtr, targetEntity) : nullptr;
}

// ============================================================================
// Status.json GuiFocus 6..9 Map Mode Detector (Galaxy Map, System Map, Orrery, FSS)
// ============================================================================
class MapFocusPoller {
public:
    static bool isMapGuiFocusActive() {
        static MapFocusPoller s_poller;
        return s_poller.poll();
    }

private:
    MapFocusPoller() {
        PWSTR savedGames = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &savedGames)) && savedGames) {
            swprintf_s(m_statusPath, L"%ls\\Frontier Developments\\Elite Dangerous\\Status.json", savedGames);
            CoTaskMemFree(savedGames);
        }
    }

    bool poll() {
        const ULONGLONG now = GetTickCount64();
        if (now - m_lastPollTick < 250) {
            return m_cachedMapOpen;
        }
        m_lastPollTick = now;
        if (!m_statusPath[0]) return false;

        HANDLE h = CreateFileW(m_statusPath, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return m_cachedMapOpen;

        FILETIME ftWrite{};
        if (GetFileTime(h, nullptr, nullptr, &ftWrite)) {
            if (ftWrite.dwLowDateTime == m_lastWriteTime.dwLowDateTime &&
                ftWrite.dwHighDateTime == m_lastWriteTime.dwHighDateTime) {
                CloseHandle(h);
                return m_cachedMapOpen;
            }
            m_lastWriteTime = ftWrite;
        }

        char buf[1024]{};
        DWORD bytesRead = 0;
        if (ReadFile(h, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buf[bytesRead] = '\0';
            if (const char* p = std::strstr(buf, "\"GuiFocus\"")) {
                p += 10;
                while (*p == ' ' || *p == '\t' || *p == ':') ++p;
                const int guiFocus = std::atoi(p);
                // GuiFocus: 6 = GalaxyMap, 7 = SystemMap, 8 = Orrery, 9 = FSS
                m_cachedMapOpen = (guiFocus >= 6 && guiFocus <= 9);
            }
        }
        CloseHandle(h);
        return m_cachedMapOpen;
    }

    wchar_t m_statusPath[MAX_PATH]{};
    ULONGLONG m_lastPollTick = 0;
    FILETIME m_lastWriteTime{};
    bool m_cachedMapOpen = false;
};

// ============================================================================
// Engine Hook Manager (Installs FUN_1405921f0 + VehicleRig hooks when in Elite)
// ============================================================================
class EngineHookManager {
public:
    static EngineHookManager& instance() {
        static EngineHookManager inst;
        return inst;
    }

    bool ensureInstalled() {
        if (m_attempted.exchange(true, std::memory_order_acq_rel)) {
            return m_viewRefreshHooked.load(std::memory_order_acquire);
        }

        wchar_t exePath[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return false;
        const wchar_t* baseName = wcsrchr(exePath, L'\\');
        baseName = baseName ? (baseName + 1) : exePath;
        if (_wcsicmp(baseName, L"EliteDangerous64.exe") != 0) {
            return false;
        }

        HMODULE exeMod = GetModuleHandleW(nullptr);
        if (!exeMod) return false;
        const auto baseAddr = reinterpret_cast< uintptr_t >(exeMod);

        // Verify and hook view-constant refresh FUN_1405921f0
        auto* refreshTarget = reinterpret_cast<uint8_t*>(baseAddr + kViewConstRefreshEntryRva);
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(refreshTarget, &mbi, sizeof(mbi)) &&
            mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            // Check standard x64 function prologue bytes (48 89 / 48 83 / 40 53 / 48 8B / 4C 8B)
            const uint8_t b0 = refreshTarget[0];
            const uint8_t b1 = refreshTarget[1];
            const bool validPrologue =
                (b0 == 0x48 && (b1 == 0x89 || b1 == 0x83 || b1 == 0x8B || b1 == 0x81)) ||
                (b0 == 0x40 && (b1 >= 0x50 && b1 <= 0x57)) ||
                (b0 == 0x4C && (b1 == 0x8B || b1 == 0x89));
            if (validPrologue) {
                m_stubPage = static_cast<uint8_t*>(CodeHook::allocateNear(refreshTarget, 256));
                if (m_stubPage) {
                    void* resumeTramp = nullptr;
                    uint8_t* stubA = m_stubPage;
                    uint8_t* stubB = m_stubPage + 128;
                    if (m_refreshHook.install(refreshTarget, stubA, &resumeTramp)) {
                        buildRefreshStubs(m_stubPage, resumeTramp, &stubA, &stubB);
                        m_viewRefreshHooked.store(true, std::memory_order_release);
                        EdpeLog(L"EDPE: Cobra view-constant refresh hook installed (RVA 0x5921F0)");
                    } else {
                        VirtualFree(m_stubPage, 0, MEM_RELEASE);
                        m_stubPage = nullptr;
                    }
                }
            }
        }

        // Optional vehicle rig hook at RVA 0x1096640
        auto* rigTarget = reinterpret_cast<uint8_t*>(baseAddr + kShipSetTargetRva);
        if (VirtualQuery(rigTarget, &mbi, sizeof(mbi)) &&
            mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            const uint8_t b0 = rigTarget[0];
            const uint8_t b1 = rigTarget[1];
            if ((b0 == 0x48 && (b1 == 0x89 || b1 == 0x83 || b1 == 0x8B)) ||
                (b0 == 0x40 && (b1 >= 0x50 && b1 <= 0x57))) {
                void* origRig = nullptr;
                if (m_rigHook.install(rigTarget, reinterpret_cast<void*>(&detourShipSetTarget), &origRig)) {
                    g_origShipSetTarget = reinterpret_cast<ShipSetTargetFn>(origRig);
                    EdpeLog(L"EDPE: Cockpit vehicle rig target hook installed (RVA 0x1096640)");
                }
            }
        }

        return m_viewRefreshHooked.load(std::memory_order_acquire);
    }

    bool isViewRefreshHooked() const {
        return m_viewRefreshHooked.load(std::memory_order_acquire);
    }

    void shutdown() {
        m_rigHook.remove();
        m_refreshHook.remove();
        if (m_stubPage) {
            VirtualFree(m_stubPage, 0, MEM_RELEASE);
            m_stubPage = nullptr;
        }
        m_viewRefreshHooked.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> m_attempted{false};
    std::atomic<bool> m_viewRefreshHooked{false};
    CodeHook m_refreshHook;
    CodeHook m_rigHook;
    uint8_t* m_stubPage = nullptr;
};

} // namespace edpe::cobra_camera
