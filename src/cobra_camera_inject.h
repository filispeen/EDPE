#pragma once

#include "elite_camera.h"
#include "temporal_math.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

#ifdef _WIN32
#include "code_hook.h"
#include "log.h"
#include <d3d11.h>
#include <shlobj.h>
#include <windows.h>
#endif

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

// ============================================================================
// Portable State Machines (Unit-Testable on both Windows and Linux)
// ============================================================================

// Tracks write faults within a sliding 5-second window; >= 8 faults triggers
// permanent stand-down (g_gate = 0) and automatic fallback to the D3D11 CB path.
class WriteFaultTracker {
public:
    static constexpr size_t kThreshold = 8;
    static constexpr uint64_t kWindowMs = 5000;

    bool recordFault(uint64_t nowMs) noexcept {
        m_timestamps[m_head] = nowMs;
        m_head = (m_head + 1) % kThreshold;
        if (m_count < kThreshold) ++m_count;

        size_t recent = 0;
        for (size_t i = 0; i < m_count; ++i) {
            if (nowMs >= m_timestamps[i] && (nowMs - m_timestamps[i]) <= kWindowMs) {
                ++recent;
            }
        }
        if (recent >= kThreshold) {
            m_stoodDown = true;
        }
        return m_stoodDown;
    }

    bool isStoodDown() const noexcept { return m_stoodDown; }

    void reset() noexcept {
        m_timestamps.fill(0);
        m_head = 0;
        m_count = 0;
        m_stoodDown = false;
    }

private:
    std::array<uint64_t, kThreshold> m_timestamps{};
    size_t m_head = 0;
    size_t m_count = 0;
    bool m_stoodDown = false;
};

// Compact zero-allocation set of camera pointers that received transient upstream
// jitter. On the first non-injected refresh call for a tracked camera (jitter off,
// stand-down, mapModeActive / GuiFocus 6..9), consumeForFlush() returns true once
// so dirty bits (0x4 | 0x8) at camera + 0x250 are raised and Cobra recomputes a
// clean unjittered projection matrix.
class InjectedCameraSet {
public:
    static constexpr size_t kCapacity = 16;

    void recordInjected(const void* camera) noexcept {
        if (!camera) return;
        const uintptr_t key = reinterpret_cast<uintptr_t>(camera);
        for (size_t i = 0; i < m_count; ++i) {
            if (m_slots[i] == key) return;
        }
        if (m_count < kCapacity) {
            m_slots[m_count++] = key;
        } else {
            // Bounded overwrite of oldest entry to avoid unbounded growth or heap allocation.
            for (size_t i = 1; i < kCapacity; ++i) {
                m_slots[i - 1] = m_slots[i];
            }
            m_slots[kCapacity - 1] = key;
        }
        m_pendingAtomic.store(static_cast<uint32_t>(m_count), std::memory_order_release);
    }

    bool consumeForFlush(const void* camera) noexcept {
        if (!camera || m_count == 0) return false;
        const uintptr_t key = reinterpret_cast<uintptr_t>(camera);
        for (size_t i = 0; i < m_count; ++i) {
            if (m_slots[i] == key) {
                m_slots[i] = m_slots[m_count - 1];
                m_slots[m_count - 1] = 0;
                --m_count;
                m_pendingAtomic.store(static_cast<uint32_t>(m_count), std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    bool contains(const void* camera) const noexcept {
        if (!camera) return false;
        const uintptr_t key = reinterpret_cast<uintptr_t>(camera);
        for (size_t i = 0; i < m_count; ++i) {
            if (m_slots[i] == key) return true;
        }
        return false;
    }

    bool empty() const noexcept {
        return m_pendingAtomic.load(std::memory_order_acquire) == 0;
    }

    size_t size() const noexcept {
        return static_cast<size_t>(m_pendingAtomic.load(std::memory_order_acquire));
    }

    void clear() noexcept {
        m_slots.fill(0);
        m_count = 0;
        m_pendingAtomic.store(0, std::memory_order_release);
    }

private:
    std::array<uintptr_t, kCapacity> m_slots{};
    size_t m_count = 0;
    std::atomic<uint32_t> m_pendingAtomic{0};
};

// Verifies x64 prologue bytes AND camera field offset witnesses (+0x250 dirty flags
// and at least one frustum/camera bound offset witness: +0x28C/+0x290/+0x228/+0x22C/+0x264)
// before patching FUN_1405921f0. Prevents hooking across incompatible game binary updates.
inline bool verifyRefreshFunctionBytes(const uint8_t* code, size_t maxBytes) noexcept {
    if (!code || maxBytes < 32) return false;
    const uint8_t b0 = code[0];
    const uint8_t b1 = code[1];
    const bool validPrologue =
        (b0 == 0x48 && (b1 == 0x89 || b1 == 0x83 || b1 == 0x8B || b1 == 0x81)) ||
        (b0 == 0x40 && (b1 >= 0x50 && b1 <= 0x57)) ||
        (b0 == 0x4C && (b1 == 0x8B || b1 == 0x89));
    if (!validPrologue) return false;

    const size_t scanLimit = (std::min)(maxBytes, static_cast<size_t>(1024));
    bool hasFlagsWitness = false;  // +0x250
    bool hasBoundsWitness = false; // +0x28C / +0x290 / +0x228 / +0x22C / +0x264
    for (size_t i = 0; i + 4 <= scanLimit; ++i) {
        uint32_t disp = 0;
        std::memcpy(&disp, code + i, sizeof(disp));
        if (disp == 0x00000250u) {
            hasFlagsWitness = true;
        } else if (disp == 0x0000028Cu || disp == 0x00000290u ||
                   disp == 0x00000228u || disp == 0x0000022Cu ||
                   disp == 0x00000264u) {
            hasBoundsWitness = true;
        }
        if (hasFlagsWitness && hasBoundsWitness) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Float32 Higham Error Bound & Deferred CB2 UV->Clip Non-Degeneracy Certification
// ============================================================================
// Higham rounding bound gamma(n) = (n * u) / (1 - n * u) with binary32 unit roundoff u = 2^-24.
inline float float32Gamma(unsigned n) noexcept {
    constexpr double u = 5.960464477539063e-8; // 2^-24
    const double nu = static_cast<double>(n) * u;
    if (nu >= 0.5) return 1.0f;
    return static_cast<float>(nu / (1.0 - nu));
}

// Strict float32 row bound for 4x4 projection/view cascade (~32 FP ops).
inline float flatDeferredFloatBound(const float* row4) noexcept {
    const float ax = std::fabs(row4[0]);
    const float ay = std::fabs(row4[1]);
    const float az = std::fabs(row4[2]);
    const float aw = std::fabs(row4[3]);
    const float m = (std::max)((std::max)(ax, ay), (std::max)(az, aw));
    return (std::max)(m * float32Gamma(32) * 8.0f, 2.0e-6f);
}

inline float flatDeferredFrustumScale(const float* b2Rows10to16) noexcept {
    const float* dX = b2Rows10to16 + 4 * 4; // row 14
    const float* dY = b2Rows10to16 + 5 * 4; // row 15
    const float lenX = std::sqrt(dX[0] * dX[0] + dX[1] * dX[1] + dX[2] * dX[2]);
    const float lenY = std::sqrt(dY[0] * dY[0] + dY[1] * dY[1] + dY[2] * dY[2]);
    return (std::max)({lenX, lenY, 1.0f});
}

inline float flatDeferredWorldAllowance(const float* b2Rows10to16) noexcept {
    const float* orig = b2Rows10to16 + 6 * 4; // row 16
    const float mag = std::sqrt(orig[0] * orig[0] + orig[1] * orig[1] + orig[2] * orig[2]);
    return (std::max)(flatDeferredFrustumScale(b2Rows10to16) * float32Gamma(64) * 64.0f,
                      mag * float32Gamma(32) * 16.0f);
}

// Certifies that the UV->Clip composite map A * B inside Deferred CB2 rows 10..16 is
// mathematically non-degenerate via the Banach/Neumann bound: ||(A*B - T) * T^-1||_inf < 1,
// where T maps UV (u, v, 1) -> NDC (2u - 1, 1 - 2v, 1).
// Prevents false-positive patching of unrelated constant buffers.
inline bool certifyDeferredUvToClipNonDegenerate(
    const float* rows10to16,
    bool rowMajor,
    float* outNormInf = nullptr) noexcept {
    if (!rows10to16) return false;
    for (int i = 0; i < 28; ++i) {
        if (!std::isfinite(rows10to16[i])) return false;
    }

    double A[3][3]{};
    double B[3][3]{};
    if (rowMajor) {
        // Row-major forward: b2[10] = clipX, b2[11] = clipY, b2[13] = clipW
        for (int j = 0; j < 3; ++j) {
            A[0][j] = static_cast<double>(rows10to16[0 * 4 + j]);
            A[1][j] = static_cast<double>(rows10to16[1 * 4 + j]);
            A[2][j] = static_cast<double>(rows10to16[3 * 4 + j]);
        }
        // Row-major inverse UV rays: rows 14..16 hold (dX_c, dY_c, 0, orig_c)
        for (int i = 0; i < 3; ++i) {
            const float* r = rows10to16 + (4 + i) * 4;
            B[i][0] = static_cast<double>(r[0]); // dX
            B[i][1] = static_cast<double>(r[1]); // dY
            B[i][2] = static_cast<double>(r[3]); // origin
        }
    } else {
        // Column-major forward: rows 0..2 are world basis, cols 0, 1, 3 are clip X, Y, W
        for (int j = 0; j < 3; ++j) {
            A[0][j] = static_cast<double>(rows10to16[j * 4 + 0]);
            A[1][j] = static_cast<double>(rows10to16[j * 4 + 1]);
            A[2][j] = static_cast<double>(rows10to16[j * 4 + 3]);
        }
        // Column-major inverse UV rays: row 14 = dX.xyz, row 15 = dY.xyz, row 16 = origin.xyz
        const float* dX = rows10to16 + 4 * 4;
        const float* dY = rows10to16 + 5 * 4;
        const float* orig = rows10to16 + 6 * 4;
        for (int i = 0; i < 3; ++i) {
            B[i][0] = static_cast<double>(dX[i]);
            B[i][1] = static_cast<double>(dY[i]);
            B[i][2] = static_cast<double>(orig[i]);
        }
    }

    // C = A * B
    double C[3][3]{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            C[i][j] = A[i][0] * B[0][j] + A[i][1] * B[1][j] + A[i][2] * B[2][j];
        }
    }

    // Target T: UV -> NDC
    constexpr double T[3][3] = {
        {2.0,  0.0, -1.0},
        {0.0, -2.0,  1.0},
        {0.0,  0.0,  1.0}
    };
    // T^-1: NDC -> UV
    constexpr double Tinv[3][3] = {
        {0.5,  0.0, 0.5},
        {0.0, -0.5, 0.5},
        {0.0,  0.0, 1.0}
    };

    double D[3][3]{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            D[i][j] = C[i][j] - T[i][j];
        }
    }

    // E = (A*B - T) * T^-1
    double normInf = 0.0;
    for (int i = 0; i < 3; ++i) {
        double rowSum = 0.0;
        for (int j = 0; j < 3; ++j) {
            const double eij = D[i][0] * Tinv[0][j] + D[i][1] * Tinv[1][j] + D[i][2] * Tinv[2][j];
            rowSum += std::fabs(eij);
        }
        if (rowSum > normInf) normInf = rowSum;
    }

    if (outNormInf) *outNormInf = static_cast<float>(normInf);
    return std::isfinite(normInf) && normInf < 1.0;
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
    if (!certifyDeferredUvToClipNonDegenerate(rows10to16, false)) {
        return false;
    }
    // Verify rows 10..13 match jitteredForward16 within strict float32 gamma bound
    for (int r = 0; r < 4; ++r) {
        const float* actual = rows10to16 + r * 4;
        const float* expected = jitteredForward16 + r * 4;
        const float tol = flatDeferredFloatBound(expected);
        for (int c = 0; c < 4; ++c) {
            if (!std::isfinite(actual[c]) || std::fabs(actual[c] - expected[c]) > tol) {
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
        diffUnjittered = (std::max)(diffUnjittered,
            std::fabs(rows10to16[16 + i] - unjitteredInverseRays12[i]));
        diffJittered = (std::max)(diffJittered,
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
    if (!certifyDeferredUvToClipNonDegenerate(rows10to16, false)) {
        return false;
    }
    for (int r = 0; r < 4; ++r) {
        const float* actual = rows10to16 + r * 4;
        const float* expected = unjitteredForward16 + r * 4;
        const float tol = flatDeferredFloatBound(expected);
        for (int c = 0; c < 4; ++c) {
            if (!std::isfinite(actual[c]) || std::fabs(actual[c] - expected[c]) > tol) {
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

// Extracts and validates the camera world position stored directly inside a
// row-major deferred lighting CB2 (b2[2..4] = WorldView, b2[6..8] = InvView).
// Relying on this self-consistent world position avoids rejecting valid CB2
// updates when the main CB1 staged pose differs by a few millimeters during motion.
inline bool extractWorldCamPosFromDeferredCb2(
    const float* b2Floats,
    size_t floatCount,
    edpe::Vec3* outCamPos) {
    if (!b2Floats || !outCamPos || floatCount < 17 * 4) return false;
    for (size_t i = 8; i < 40; ++i) {
        if (!std::isfinite(b2Floats[i])) return false;
    }
    const float* wv = b2Floats + 8;   // b2[2..5]: WorldView + [0,0,0,1]
    const float* iv = b2Floats + 24;  // b2[6..9]: InvView   + [0,0,0,1]
    if (std::fabs(wv[12]) > 1e-3f || std::fabs(wv[13]) > 1e-3f ||
        std::fabs(wv[14]) > 1e-3f || std::fabs(wv[15] - 1.0f) > 1e-3f ||
        std::fabs(iv[12]) > 1e-3f || std::fabs(iv[13]) > 1e-3f ||
        std::fabs(iv[14]) > 1e-3f || std::fabs(iv[15] - 1.0f) > 1e-3f) {
        return false;
    }
    for (int r = 0; r < 3; ++r) {
        for (int r2 = r; r2 < 3; ++r2) {
            const double dp =
                static_cast<double>(wv[r * 4 + 0]) * wv[r2 * 4 + 0] +
                static_cast<double>(wv[r * 4 + 1]) * wv[r2 * 4 + 1] +
                static_cast<double>(wv[r * 4 + 2]) * wv[r2 * 4 + 2];
            const double target = (r == r2) ? 1.0 : 0.0;
            if (std::fabs(dp - target) > 5.0e-3) return false;
        }
        for (int c = 0; c < 3; ++c) {
            if (std::fabs(iv[r * 4 + c] - wv[c * 4 + r]) > 5.0e-3f) return false;
        }
    }
    const double det =
        static_cast<double>(wv[0]) * (static_cast<double>(wv[5]) * wv[10] - static_cast<double>(wv[6]) * wv[9]) -
        static_cast<double>(wv[1]) * (static_cast<double>(wv[4]) * wv[10] - static_cast<double>(wv[6]) * wv[8]) +
        static_cast<double>(wv[2]) * (static_cast<double>(wv[4]) * wv[9] - static_cast<double>(wv[5]) * wv[8]);
    if (det < 0.85 || det > 1.15) return false;

    const edpe::Vec3 cand{iv[3], iv[7], iv[11]};
    for (int r = 0; r < 3; ++r) {
        const float* row = wv + r * 4;
        const double dotPos =
            static_cast<double>(row[0]) * cand.x +
            static_cast<double>(row[1]) * cand.y +
            static_cast<double>(row[2]) * cand.z;
        const double absTerms =
            std::fabs(static_cast<double>(row[0]) * cand.x) +
            std::fabs(static_cast<double>(row[1]) * cand.y) +
            std::fabs(static_cast<double>(row[2]) * cand.z);
        const double tol = (std::max)(absTerms * static_cast<double>(float32Gamma(64) * 12.0f), 1.0e-3);
        if (std::fabs(dotPos + static_cast<double>(row[3])) > tol) {
            return false;
        }
    }
    *outCamPos = cand;
    return true;
}

inline bool matchRowMajorCols0to2(const float* actual16, const float* expected16) {
    if (!actual16 || !expected16) return false;
    for (int r = 0; r < 4; ++r) {
        const float* a = actual16 + r * 4;
        const float* e = expected16 + r * 4;
        const float tol = flatDeferredFloatBound(e);
        for (int c = 0; c < 3; ++c) {
            if (!std::isfinite(a[c]) || std::fabs(a[c] - e[c]) > tol) {
                return false;
            }
        }
        if (!std::isfinite(a[3])) return false;
    }
    return true;
}

inline bool matchMatrix4x4(
    const float* actual16,
    const float* expected16,
    const edpe::Vec3* worldCamPos = nullptr) {
    bool allCamRel = true;
    for (int r = 0; r < 4; ++r) {
        const float* a = actual16 + r * 4;
        const float* e = expected16 + r * 4;
        const float tol = flatDeferredFloatBound(e);
        for (int c = 0; c < 3; ++c) {
            if (!std::isfinite(a[c]) || std::fabs(a[c] - e[c]) > tol) {
                return false;
            }
        }
        if (!std::isfinite(a[3])) {
            return false;
        }
        if (std::fabs(a[3] - e[3]) > tol) {
            allCamRel = false;
        }
    }
    if (allCamRel) {
        return true;
    }
    if (!worldCamPos ||
        !std::isfinite(worldCamPos->x) ||
        !std::isfinite(worldCamPos->y) ||
        !std::isfinite(worldCamPos->z)) {
        return false;
    }
    // Deferred lighting CB2 can store world-space Proj * WorldView instead of
    // camera-relative coordinates: row.w == expected.w - dot(row.xyz, worldCamPos).
    for (int r = 0; r < 4; ++r) {
        const float* a = actual16 + r * 4;
        const float* e = expected16 + r * 4;
        const float tol = flatDeferredFloatBound(e);
        const double dotPos =
            static_cast<double>(a[0]) * worldCamPos->x +
            static_cast<double>(a[1]) * worldCamPos->y +
            static_cast<double>(a[2]) * worldCamPos->z;
        const double absTerms =
            std::fabs(static_cast<double>(a[0]) * worldCamPos->x) +
            std::fabs(static_cast<double>(a[1]) * worldCamPos->y) +
            std::fabs(static_cast<double>(a[2]) * worldCamPos->z);
        const float worldTol = (std::max)({tol, static_cast<float>(absTerms) * float32Gamma(64) * 10.0f, 1.0e-4f});
        const double expectedWorldW = static_cast<double>(e[3]) - dotPos;
        if (std::fabs(static_cast<double>(a[3]) - expectedWorldW) > worldTol) {
            return false;
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
    const float scale = (std::max)({std::sqrt(lenX2), std::sqrt(lenY2), 1.0f});
    const float allow = (std::max)(scale * float32Gamma(64) * 64.0f, 1e-4f);

    float diffUnjittered = 0.0f;
    float diffJittered = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const float unjitOrig = forwardVec[c] - 0.5f * rows[c][0] - 0.5f * rows[c][1];
        const float jitOrig = unjitOrig - dxUv * rows[c][0] - dyUv * rows[c][1];
        diffUnjittered = (std::max)(diffUnjittered, std::fabs(rows[c][3] - unjitOrig));
        diffJittered = (std::max)(diffJittered, std::fabs(rows[c][3] - jitOrig));
    }
    return diffUnjittered <= allow && diffUnjittered < diffJittered * 0.5f;
}

inline bool repairRowMajorDeferredMissingPhase(
    float* b2Floats,
    size_t floatCount,
    const float* colMajorJitteredForward16,
    float dxUv,
    float dyUv,
    const edpe::Vec3* worldCamPos = nullptr) {
    if (!b2Floats || floatCount < 17 * 4 || !colMajorJitteredForward16) return false;
    if (std::fabs(dxUv) <= 1e-7f && std::fabs(dyUv) <= 1e-7f) return false;

    float* rows10to16 = b2Floats + 10 * 4;
    if (!certifyDeferredUvToClipNonDegenerate(rows10to16, true)) return false;

    float expectedRowMajor[16]{};
    transposeBlock4x4(colMajorJitteredForward16, expectedRowMajor);
    edpe::Vec3 cb2CamPos{};
    const bool hasCb2Pos = extractWorldCamPosFromDeferredCb2(b2Floats, floatCount, &cb2CamPos);
    const bool matched =
        (hasCb2Pos && matchMatrix4x4(rows10to16, expectedRowMajor, &cb2CamPos)) ||
        matchMatrix4x4(rows10to16, expectedRowMajor, worldCamPos);
    if (!matched) return false;
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
    float dyUv,
    const edpe::Vec3* worldCamPos = nullptr) {
    if (!b2Floats || floatCount < 17 * 4 || !colMajorUnjitteredForward16) return false;
    if (std::fabs(dxUv) <= 1e-7f && std::fabs(dyUv) <= 1e-7f &&
        std::fabs(jitter.ndcX) <= 1e-7f && std::fabs(jitter.ndcY) <= 1e-7f) return false;

    float* rows10to16 = b2Floats + 10 * 4;
    if (!certifyDeferredUvToClipNonDegenerate(rows10to16, true)) return false;

    float expectedRowMajor[16]{};
    transposeBlock4x4(colMajorUnjitteredForward16, expectedRowMajor);
    edpe::Vec3 cb2CamPos{};
    const bool hasCb2Pos = extractWorldCamPosFromDeferredCb2(b2Floats, floatCount, &cb2CamPos);
    const bool matched =
        (hasCb2Pos && matchMatrix4x4(rows10to16, expectedRowMajor, &cb2CamPos)) ||
        matchMatrix4x4(rows10to16, expectedRowMajor, worldCamPos);
    if (!matched) return false;

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

#ifdef _WIN32

// ============================================================================
// SEH-Protected Memory Helpers (no C++ destructors in __try frames)
// ============================================================================
inline bool sehReadU32(const void* addr, uint32_t* out) noexcept {
    if (!addr || !out) return false;
#if defined(_MSC_VER)
    __try {
        uint32_t v = 0;
        std::memcpy(&v, addr, sizeof(v));
        *out = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(out, addr, sizeof(uint32_t));
    return true;
#endif
}

inline bool sehReadF32(const void* addr, float* out) noexcept {
    if (!addr || !out) return false;
#if defined(_MSC_VER)
    __try {
        float v = 0.0f;
        std::memcpy(&v, addr, sizeof(v));
        *out = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(out, addr, sizeof(float));
    return true;
#endif
}

inline bool sehReadU64(const void* addr, uint64_t* out) noexcept {
    if (!addr || !out) return false;
#if defined(_MSC_VER)
    __try {
        uint64_t v = 0;
        std::memcpy(&v, addr, sizeof(v));
        *out = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(out, addr, sizeof(uint64_t));
    return true;
#endif
}

inline bool sehWriteU32(void* addr, uint32_t val) noexcept {
    if (!addr) return false;
#if defined(_MSC_VER)
    __try {
        std::memcpy(addr, &val, sizeof(val));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(addr, &val, sizeof(val));
    return true;
#endif
}

inline bool sehWriteF32(void* addr, float val) noexcept {
    if (!addr) return false;
#if defined(_MSC_VER)
    __try {
        std::memcpy(addr, &val, sizeof(val));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(addr, &val, sizeof(val));
    return true;
#endif
}

inline bool sehWriteU64(void* addr, uint64_t val) noexcept {
    if (!addr) return false;
#if defined(_MSC_VER)
    __try {
        std::memcpy(addr, &val, sizeof(val));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(addr, &val, sizeof(val));
    return true;
#endif
}

// ============================================================================
// Gate-First Relay & Thread-Local State (Zero I/O, Zero Allocations on Hot Path)
// ============================================================================
// When g_gate == 0, relay transparently jumps to resumeTrampoline without
// clobbering any general-purpose or XMM register.
inline std::atomic<uintptr_t> g_gate{0};

// Thread-local state for ABI-safe stubA / stubB return-address interception.
// `realRet` MUST remain at offset 0 for stubB's `mov rcx, [rax+0]` instruction.
struct RefreshTls {
    void* realRet = nullptr;       // offset 0
    uint8_t* camera = nullptr;     // offset 8
    float savedBoundX = 0.0f;      // offset 16
    float savedBoundY = 0.0f;      // offset 20
    uint32_t savedFlags = 0;       // offset 24
    uint32_t layoutVariant = 0;    // offset 28 (0 = none, 1 = +0x28C/+0x290)
};

inline __declspec(thread) RefreshTls t_refreshTls{};

struct FrameJitterWindow {
    std::atomic<DWORD> renderThreadId{0};
    std::atomic<bool> windowOpen{false};
    std::atomic<bool> pauseRequested{false};
    std::atomic<bool> mapModeActive{false};
    std::atomic<bool> stoodDown{false};
    std::atomic<uint32_t> renderWidth{0};
    std::atomic<uint32_t> renderHeight{0};
    std::atomic<float> jitterPxX{0.0f};
    std::atomic<float> jitterPxY{0.0f};
    std::atomic<uint64_t> injectedFrameCount{0};
    std::atomic<uint32_t> injectedDrawsThisFrame{0};
    std::atomic<uint64_t> flushedCamerasTotal{0};
    std::atomic<uint64_t> readFaultsTotal{0};
    std::atomic<uint64_t> writeFaultsTotal{0};
    std::atomic<uint64_t> rollbacksTotal{0};
    std::atomic<uint64_t> rejectedKindTotal{0};
};

inline FrameJitterWindow& frameWindow() noexcept {
    static FrameJitterWindow w;
    return w;
}

inline InjectedCameraSet& injectedCameras() noexcept {
    static InjectedCameraSet s;
    return s;
}

inline WriteFaultTracker& writeFaultTracker() noexcept {
    static WriteFaultTracker t;
    return t;
}

inline void recordHookWriteFault() noexcept {
    auto& win = frameWindow();
    win.writeFaultsTotal.fetch_add(1, std::memory_order_relaxed);
    if (writeFaultTracker().recordFault(GetTickCount64())) {
        win.stoodDown.store(true, std::memory_order_release);
        win.windowOpen.store(false, std::memory_order_release);
        g_gate.store(0, std::memory_order_release);
    }
}

inline RefreshTls* __fastcall getRefreshTls() noexcept {
    return &t_refreshTls;
}

// Modifies the camera frustum tangent bounds (boundX, boundY) and raises the dirty flags
// (0x4 | 0x8) right before the engine's own matrix finalizers compute all forward/inverse
// matrices and inverse frustum rays. All memory accesses are SEH-protected with rollback.
inline bool applyTransientCameraBoundJitter(uint8_t* camera, RefreshTls& tls) noexcept {
    if (!camera) return false;
    auto& win = frameWindow();
    if (win.stoodDown.load(std::memory_order_acquire)) return false;
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

    // SEH-protected reads for verified camera layout (+0x264 kind, +0x28C/+0x290 bounds, +0x250 flags).
    uint32_t kind1 = 0;
    float bx1 = 0.0f, by1 = 0.0f;
    if (!sehReadU32(camera + 0x264, &kind1) ||
        !sehReadF32(camera + 0x28C, &bx1) ||
        !sehReadF32(camera + 0x290, &by1)) {
        win.readFaultsTotal.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    auto validBounds = [&](float bx, float by) -> bool {
        if (!std::isfinite(bx) || !std::isfinite(by)) return false;
        if (std::fabs(bx) < 0.05f || std::fabs(bx) > 5.0f) return false;
        if (std::fabs(by) < 0.05f || std::fabs(by) > 5.0f) return false;
        const float camAspect = std::fabs(bx / by);
        const float targetAspect = static_cast<float>(rw) / static_cast<float>(rh);
        return std::fabs(camAspect - targetAspect) <= 0.12f * targetAspect;
    };

    if (kind1 != 3) {
        win.rejectedKindTotal.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!validBounds(bx1, by1)) {
        return false;
    }

    const size_t offBx = 0x28C;
    const size_t offBy = 0x290;
    const uint32_t variant = 1;
    tls.savedBoundX = bx1;
    tls.savedBoundY = by1;

    uint32_t flags = 0;
    if (!sehReadU32(camera + 0x250, &flags)) {
        win.readFaultsTotal.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const float newBx = tls.savedBoundX + jx / static_cast<float>(rw);
    const float newBy = tls.savedBoundY - jy / static_cast<float>(rh);
    const uint32_t newFlags = flags | 0x4u | 0x8u;

    if (!sehWriteF32(camera + offBx, newBx)) {
        recordHookWriteFault();
        return false;
    }
    if (!sehWriteF32(camera + offBy, newBy)) {
        sehWriteF32(camera + offBx, tls.savedBoundX);
        win.rollbacksTotal.fetch_add(1, std::memory_order_relaxed);
        recordHookWriteFault();
        return false;
    }
    if (!sehWriteU32(camera + 0x250, newFlags)) {
        sehWriteF32(camera + offBx, tls.savedBoundX);
        sehWriteF32(camera + offBy, tls.savedBoundY);
        win.rollbacksTotal.fetch_add(1, std::memory_order_relaxed);
        recordHookWriteFault();
        return false;
    }

    tls.camera = camera;
    tls.savedFlags = flags;
    tls.layoutVariant = variant;

    injectedCameras().recordInjected(camera);
    win.injectedDrawsThisFrame.fetch_add(1, std::memory_order_relaxed);
    win.injectedFrameCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

inline void __fastcall refreshPost() noexcept {
    RefreshTls& tls = t_refreshTls;
    if (tls.camera && tls.layoutVariant == 1) {
        const bool okX = sehWriteF32(tls.camera + 0x28Cu, tls.savedBoundX);
        const bool okY = sehWriteF32(tls.camera + 0x290u, tls.savedBoundY);
        const bool okF = sehWriteU32(tls.camera + 0x250, tls.savedFlags);
        if (!okX || !okY || !okF) {
            recordHookWriteFault();
        }
    }
    tls.camera = nullptr;
    tls.layoutVariant = 0;
}

inline void* g_stubBAddr = nullptr;

inline void* __fastcall refreshPre(uint8_t* camera, void* realRet) noexcept {
    RefreshTls& tls = t_refreshTls;
    tls.realRet = realRet;
    tls.camera = nullptr;
    tls.layoutVariant = 0;

    if (g_stubBAddr && applyTransientCameraBoundJitter(camera, tls)) {
        return g_stubBAddr;
    }

    // flushCamera: on the first non-injected call for a previously injected camera
    // (jitter disabled, stand-down, mapModeActive / GuiFocus 6..9), raise dirty bits
    // 0x4 | 0x8 once so Cobra recomputes a clean unjittered projection matrix.
    auto& win = frameWindow();
    const DWORD tid = win.renderThreadId.load(std::memory_order_relaxed);
    if (camera && (tid == 0 || GetCurrentThreadId() == tid)) {
        if (injectedCameras().consumeForFlush(camera)) {
            uint32_t flags = 0;
            if (sehReadU32(camera + 0x250, &flags)) {
                if (sehWriteU32(camera + 0x250, flags | 0x4u | 0x8u)) {
                    win.flushedCamerasTotal.fetch_add(1, std::memory_order_relaxed);
                } else {
                    recordHookWriteFault();
                }
            } else {
                win.readFaultsTotal.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Only close g_gate on pause once all previously injected cameras have been flushed.
        if (win.pauseRequested.load(std::memory_order_acquire) && injectedCameras().empty()) {
            g_gate.store(0, std::memory_order_release);
        }
    }

    return realRet;
}

// Emits gate-first relay + ABI-safe stubA (16B-aligned with 32B Win64 shadow space) +
// stubB (16B-aligned return epilogue preserving RAX & XMM0), switches the stub page
// to PAGE_EXECUTE_READ, and flushes the instruction cache BEFORE any hook is committed.
inline bool buildRefreshStubs(
    uint8_t* page,
    size_t pageSize,
    void* resumeTrampoline,
    uint8_t** outRelay,
    uint8_t** outStubA,
    uint8_t** outStubB) {
    if (!page || pageSize < 384 || !resumeTrampoline) return false;
    std::memset(page, 0xCC, pageSize);

    uint8_t* relay = page + 0;
    uint8_t* stubA = page + 64;
    uint8_t* stubB = page + 192;

    // relay (entered via JMP from FUN_1405921f0 prologue):
    //   If g_gate == 0: transparently jumps to resumeTrampoline with zero GPR/XMM clobber.
    //   If g_gate != 0: jumps to stubA.
    size_t r = 0;
    auto emitR = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t by : bytes) relay[r++] = by;
    };
    emitR({0x50});                               // push rax
    emitR({0x48, 0xB8});                         // mov rax, imm64 (&g_gate)
    const uintptr_t gateAddr = reinterpret_cast<uintptr_t>(&g_gate);
    std::memcpy(relay + r, &gateAddr, sizeof(gateAddr));
    r += 8;
    emitR({0x48, 0x8B, 0x00});                   // mov rax, [rax]
    emitR({0x48, 0x85, 0xC0});                   // test rax, rax
    emitR({0x58});                               // pop rax (does not modify RFLAGS)
    emitR({0x0F, 0x85});                         // jnz rel32 (stubA)
    const int32_t jnzDisp = static_cast<int32_t>(stubA - (relay + r + 4));
    std::memcpy(relay + r, &jnzDisp, sizeof(jnzDisp));
    r += 4;
    emitR({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp qword ptr [rip+0]
    const uintptr_t resumeVal = reinterpret_cast<uintptr_t>(resumeTrampoline);
    std::memcpy(relay + r, &resumeVal, sizeof(resumeVal));
    r += 8;
    if (r > 64) return false;

    // stubA (entered with RSP % 16 == 8 from caller's CALL):
    //   Saves RCX/RDX/R8/R9 in caller's home space, aligns RSP with sub rsp, 0x28
    //   (32B shadow space + 8B alignment -> RSP % 16 == 0 before call refreshPre).
    size_t a = 0;
    auto emitA = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t by : bytes) stubA[a++] = by;
    };
    emitA({0x48, 0x89, 0x4C, 0x24, 0x08});       // mov [rsp+0x08], rcx
    emitA({0x48, 0x89, 0x54, 0x24, 0x10});       // mov [rsp+0x10], rdx
    emitA({0x4C, 0x89, 0x44, 0x24, 0x18});       // mov [rsp+0x18], r8
    emitA({0x4C, 0x89, 0x4C, 0x24, 0x20});       // mov [rsp+0x20], r9
    emitA({0x48, 0x8B, 0x14, 0x24});             // mov rdx, [rsp] (realRet)
    emitA({0x48, 0x83, 0xEC, 0x28});             // sub rsp, 0x28 (32B shadow + 16B align)
    emitA({0x48, 0xB8});                         // mov rax, imm64 (&refreshPre)
    const uintptr_t preAddr = reinterpret_cast<uintptr_t>(&refreshPre);
    std::memcpy(stubA + a, &preAddr, sizeof(preAddr));
    a += 8;
    emitA({0xFF, 0xD0});                         // call rax
    emitA({0x48, 0x83, 0xC4, 0x28});             // add rsp, 0x28
    emitA({0x48, 0x89, 0x04, 0x24});             // mov [rsp], rax
    emitA({0x48, 0x8B, 0x4C, 0x24, 0x08});       // mov rcx, [rsp+0x08]
    emitA({0x48, 0x8B, 0x54, 0x24, 0x10});       // mov rdx, [rsp+0x10]
    emitA({0x4C, 0x8B, 0x44, 0x24, 0x18});       // mov r8,  [rsp+0x18]
    emitA({0x4C, 0x8B, 0x4C, 0x24, 0x20});       // mov r9,  [rsp+0x20]
    emitA({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp qword ptr [rip+0]
    std::memcpy(stubA + a, &resumeVal, sizeof(resumeVal));
    a += 8;
    if (a > 128) return false;

    // stubB (entered via `ret` from FUN_1405921f0, so RSP % 16 == 0):
    //   sub rsp, 0x40 keeps RSP % 16 == 0 and provides 32B shadow space [0x00..0x1F],
    //   16B-aligned XMM0 save [0x20..0x2F], and RAX save [0x30..0x37].
    size_t b = 0;
    auto emitB = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t by : bytes) stubB[b++] = by;
    };
    emitB({0x48, 0x83, 0xEC, 0x40});             // sub rsp, 0x40
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
    emitB({0x48, 0x83, 0xC4, 0x40});             // add rsp, 0x40
    emitB({0xFF, 0xE1});                         // jmp rcx
    if (b > 128) return false;

    g_stubBAddr = stubB;
    if (outRelay) *outRelay = relay;
    if (outStubA) *outStubA = stubA;
    if (outStubB) *outStubB = stubB;

    DWORD oldProt = 0;
    if (!VirtualProtect(page, pageSize, PAGE_EXECUTE_READ, &oldProt)) {
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), page, pageSize);
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
        auto& poller = instance();
        poller.poll();
        return poller.m_cachedMapOpen;
    }

    static bool isOnFootActive() {
        auto& poller = instance();
        poller.poll();
        return poller.m_cachedOnFoot;
    }

private:
    static MapFocusPoller& instance() {
        static MapFocusPoller s_poller;
        return s_poller;
    }

    MapFocusPoller() {
        PWSTR savedGames = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &savedGames)) && savedGames) {
            swprintf_s(m_statusPath, L"%ls\\Frontier Developments\\Elite Dangerous\\Status.json", savedGames);
            CoTaskMemFree(savedGames);
        }
    }

    void poll() {
        const ULONGLONG now = GetTickCount64();
        if (now - m_lastPollTick < 250) {
            return;
        }
        m_lastPollTick = now;
        if (!m_statusPath[0]) return;

        HANDLE h = CreateFileW(m_statusPath, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;

        FILETIME ftWrite{};
        if (GetFileTime(h, nullptr, nullptr, &ftWrite)) {
            if (ftWrite.dwLowDateTime == m_lastWriteTime.dwLowDateTime &&
                ftWrite.dwHighDateTime == m_lastWriteTime.dwHighDateTime) {
                CloseHandle(h);
                return;
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
            if (const char* f2 = std::strstr(buf, "\"Flags2\"")) {
                f2 += 8;
                while (*f2 == ' ' || *f2 == '\t' || *f2 == ':') ++f2;
                const unsigned long flags2 = std::strtoul(f2, nullptr, 10);
                // Flags2 bits: 0x1=OnFoot, 0x8=OnFootInHangar, 0x10=OnFootInStation,
                // 0x20=OnFootOnPlanet, 0x1000=OnFootSocialSpace, 0x2000=OnFootExterior
                m_cachedOnFoot = (flags2 & 0x3039u) != 0;
            } else {
                m_cachedOnFoot = false;
            }
        }
        CloseHandle(h);
    }

    wchar_t m_statusPath[MAX_PATH]{};
    ULONGLONG m_lastPollTick = 0;
    FILETIME m_lastWriteTime{};
    bool m_cachedMapOpen = false;
    bool m_cachedOnFoot = false;
};

// ============================================================================
// Engine Hook Manager (Race-Free Gate-First Installation + Hold-Open Lifetime)
// ============================================================================
class EngineHookManager {
public:
    static EngineHookManager& instance() {
        static EngineHookManager inst;
        return inst;
    }

    bool ensureInstalled() {
        if (m_attempted.exchange(true, std::memory_order_acq_rel)) {
            return isViewRefreshUsable();
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
        const auto baseAddr = reinterpret_cast<uintptr_t>(exeMod);

        // 1. Verify prologue AND camera field offset witnesses before touching executable memory.
        auto* refreshTarget = reinterpret_cast<uint8_t*>(baseAddr + kViewConstRefreshEntryRva);
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(refreshTarget, &mbi, sizeof(mbi)) &&
            mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            const size_t availBytes = (regionEnd > reinterpret_cast<uintptr_t>(refreshTarget))
                ? static_cast<size_t>(regionEnd - reinterpret_cast<uintptr_t>(refreshTarget))
                : 0;
            if (verifyRefreshFunctionBytes(refreshTarget, availBytes)) {
                g_gate.store(0, std::memory_order_release);
                constexpr size_t kStubPageSize = 512;
                m_stubPage = static_cast<uint8_t*>(CodeHook::allocateNear(refreshTarget, kStubPageSize));
                if (m_stubPage) {
                    void* resumeTramp = nullptr;
                    uint8_t* relay = nullptr;
                    uint8_t* stubA = nullptr;
                    uint8_t* stubB = nullptr;
                    m_refreshHook.setHoldOpen(true);
                    if (m_refreshHook.prepareTrampoline(refreshTarget, &resumeTramp)) {
                        if (buildRefreshStubs(m_stubPage, kStubPageSize, resumeTramp, &relay, &stubA, &stubB) &&
                            m_refreshHook.commitPatch(relay)) {
                            m_viewRefreshHooked.store(true, std::memory_order_release);
                            g_gate.store(1, std::memory_order_release);
                            EdpeLog(L"EDPE: Cobra view-constant refresh hook installed (gate-first, RVA 0x5921F0)");
                        } else {
                            m_refreshHook.discardPrepared();
                            VirtualFree(m_stubPage, 0, MEM_RELEASE);
                            m_stubPage = nullptr;
                        }
                    } else {
                        VirtualFree(m_stubPage, 0, MEM_RELEASE);
                        m_stubPage = nullptr;
                    }
                }
            } else {
                EdpeLog(L"EDPE: Cobra view-constant refresh prologue/witness mismatch; using D3D11 CB fallback");
            }
        }

        // 2. Optional vehicle rig hook at RVA 0x1096640 (hold-open for process lifetime).
        auto* rigTarget = reinterpret_cast<uint8_t*>(baseAddr + kShipSetTargetRva);
        if (VirtualQuery(rigTarget, &mbi, sizeof(mbi)) &&
            mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            const uint8_t b0 = rigTarget[0];
            const uint8_t b1 = rigTarget[1];
            if ((b0 == 0x48 && (b1 == 0x89 || b1 == 0x83 || b1 == 0x8B)) ||
                (b0 == 0x40 && (b1 >= 0x50 && b1 <= 0x57))) {
                void* origRig = nullptr;
                m_rigHook.setHoldOpen(true);
                if (m_rigHook.prepareTrampoline(rigTarget, &origRig)) {
                    g_origShipSetTarget = reinterpret_cast<ShipSetTargetFn>(origRig);
                    if (m_rigHook.commitPatch(reinterpret_cast<void*>(&detourShipSetTarget))) {
                        EdpeLog(L"EDPE: Cockpit vehicle rig target hook installed (RVA 0x1096640)");
                    } else {
                        g_origShipSetTarget = nullptr;
                    }
                }
            }
        }

        return isViewRefreshUsable();
    }

    bool isViewRefreshHooked() const noexcept {
        return m_viewRefreshHooked.load(std::memory_order_acquire);
    }

    bool isStoodDown() const noexcept {
        return frameWindow().stoodDown.load(std::memory_order_acquire);
    }

    bool isViewRefreshUsable() const noexcept {
        return m_viewRefreshHooked.load(std::memory_order_acquire) &&
               !frameWindow().stoodDown.load(std::memory_order_acquire);
    }

    // Hold-open for process lifetime: NEVER call VirtualFree(m_stubPage) or unmap
    // executable trampolines while game threads could still execute inside them.
    void shutdown() noexcept {
        frameWindow().windowOpen.store(false, std::memory_order_release);
        frameWindow().pauseRequested.store(true, std::memory_order_release);
        g_gate.store(0, std::memory_order_release);
        m_viewRefreshHooked.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> m_attempted{false};
    std::atomic<bool> m_viewRefreshHooked{false};
    CodeHook m_refreshHook;
    CodeHook m_rigHook;
    uint8_t* m_stubPage = nullptr;
};

#endif // _WIN32

} // namespace edpe::cobra_camera
