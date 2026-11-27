#include "weapon_motion.h"
#include "celestial_motion.h"

#include <cmath>

int main() {
    // 1. WeaponDrawIdentity matching ignores volatile pool-slot byte (0x00FF0000).
    edpe::WeaponDrawIdentity wA{128u, 0x12345678u, 1500u, 1u};
    edpe::WeaponDrawIdentity wB{128u, 0x12AB5678u, 1500u, 1u};
    edpe::WeaponDrawIdentity wDiffBone{132u, 0x12345678u, 1500u, 1u};
    edpe::WeaponDrawIdentity wDiffSig{128u, 0x12345679u, 1500u, 1u};
    if (!edpe::sameWeaponIdentity(wA, wB)) return 1;
    if (edpe::sameWeaponIdentity(wA, wDiffBone)) return 2;
    if (edpe::sameWeaponIdentity(wA, wDiffSig)) return 3;

    // 2. Canonical weapon depth rescaling: 0.0675m -> 0.025m.
    const float rawWeaponDepth = 0.27f; // z = 0.0675 / 0.27 = 0.25m
    const float canonical = edpe::computeCanonicalWeaponDepth(rawWeaponDepth, 0.025f, 0.0675f);
    if (std::fabs(canonical - 0.10f) > 1e-5f) return 4; // 0.025 / 0.25 = 0.10
    if (edpe::computeCanonicalWeaponDepth(0.0f) != 0.0f) return 5;
    if (edpe::computeCanonicalWeaponDepth(NAN) != 0.0f) return 6;

    // 3. Weapon motion jitter phase stripping: mv + (jitterNow - jitterPrev).
    float mvX = 2.0f, mvY = -1.5f;
    edpe::unjitterWeaponMotion(mvX, mvY, 0.25f, -0.125f, -0.25f, 0.375f, true);
    if (std::fabs(mvX - 2.5f) > 1e-5f || std::fabs(mvY - (-2.0f)) > 1e-5f) return 7;
    // NaN safety clamp.
    float nanX = NAN, nanY = 1.0f;
    edpe::unjitterWeaponMotion(nanX, nanY, 0.0f, 0.0f, 0.0f, 0.0f, false);
    if (nanX != 0.0f || nanY != 0.0f) return 8;

    // 4. Status.json Supercruise flag parser (Flags bit 4 = 0x10 = 16).
    bool sc = false;
    if (!edpe::parseStatusJsonSupercruise(R"({"timestamp":"2026-11-21T10:00:00Z","event":"Status","Flags":16842768})", &sc) || !sc)
        return 9; // 16842768 & 16 == 16
    if (!edpe::parseStatusJsonSupercruise(R"({"Flags": 16842752})", &sc) || sc)
        return 10; // docked / normal space (bit 4 clear)
    if (edpe::parseStatusJsonSupercruise(R"({"event":"Status"})", &sc))
        return 11; // missing Flags

    // 5. CelestialRecord builder: [R_rel | t_rel], 3px slack on box & radial shell.
    const float curPose[12] = {
        1.0f, 0.0f, 0.0f, 100.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 1000.0f
    };
    const float prevPose[12] = {
        1.0f, 0.0f, 0.0f, 110.0f,
        0.0f, 1.0f, 0.0f, -5.0f,
        0.0f, 0.0f, 1.0f, 1020.0f
    };
    const auto rec = edpe::buildCelestialRecord(
        curPose, prevPose,
        400.0f, 200.0f, 800.0f, 600.0f,
        0.0001f, 0.005f,
        600.0f, 400.0f,
        0.0f, 200.0f);
    // For pure translation, t_rel = t_prev - t_cur = (+10, -5, +20).
    if (std::fabs(rec.relRow0[3] - 10.0f) > 1e-4f ||
        std::fabs(rec.relRow1[3] - (-5.0f)) > 1e-4f ||
        std::fabs(rec.relRow2[3] - 20.0f) > 1e-4f) return 12;
    // 3px slack verified.
    if (std::fabs(rec.box[0] - 397.0f) > 1e-4f || std::fabs(rec.box[2] - 803.0f) > 1e-4f) return 13;
    if (std::fabs(rec.span[3] - 203.0f) > 1e-4f || rec.center[2] < 0.5f) return 14;

    // 6. Issue #30: Stream-Output vertex capacity bounds for TRIANGLELIST and TRIANGLESTRIP.
    if (edpe::computeWeaponSoVertexCapacity(false, 0u) != 0u) return 15;
    if (edpe::computeWeaponSoVertexCapacity(false, 2u) != 0u) return 16;
    if (edpe::computeWeaponSoVertexCapacity(false, 3u) != 3u) return 17;
    if (edpe::computeWeaponSoVertexCapacity(false, 250000u) != 250000u) return 18;
    if (edpe::computeWeaponSoVertexCapacity(false, 250001u) != 0u) return 19;
    if (edpe::computeWeaponSoVertexCapacity(true, 0u) != 0u) return 20;
    if (edpe::computeWeaponSoVertexCapacity(true, 2u) != 0u) return 21;
    if (edpe::computeWeaponSoVertexCapacity(true, 3u) != 3u) return 22;
    if (edpe::computeWeaponSoVertexCapacity(true, 4u) != 6u) return 23;
    if (edpe::computeWeaponSoVertexCapacity(true, 100u) != 294u) return 24;
    if (edpe::computeWeaponSoVertexCapacity(true, 83335u) != 249999u) return 25;
    if (edpe::computeWeaponSoVertexCapacity(true, 83336u) != 0u) return 26;

    return 0;
}
