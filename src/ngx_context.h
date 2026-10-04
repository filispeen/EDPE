#pragma once

#include <cstdint>
#include <d3d11.h>

namespace edpe {

// NGX context management.
// Handles NVIDIA DLSS Super Resolution and DLAA initialization, evaluation,
// and shutdown through the NVIDIA NGX D3D11 SDK.
//
// Critical constraints (per nvidia-dlaa-input-contract.md, 2026-09-27):
// 1. SDK probe hung >20s in NVSDK_NGX_D3D11_Shutdown1; cause unknown.
//    Do NOT rely on threading to hide this hang — move the entire probe
//    into a separate OS process with an external timeout. The SDK shutdown
//    thread may remain active after return; do not depend on process exit
//    to cleanly terminate NGX resources.
// 2. Successful Init is NOT sufficient for Elite runtime usage. Must also
//    verify: feature creation, evaluation, resource release, and shutdown.
//    Then validate Elite input conventions (see ELITE_INPUT_CONVENTIONS below).
// 3. Input conventions MUST be verified in Elite before NGX is consumed as
//    production input: jitter sign/motion-scale/depth-flag/reset semantics.
//    Equality of input/output dimensions (DLAA mode) removes scaling issues
//    but does not satisfy these convention requirements.
// 4. Fail open: if any requirement cannot be satisfied, disable affected
//    upscaler, disable jitter, restore original frame, Present normally.
//    Never black-screen Elite because a research assumption failed.
//
// ELITE_INPUT_CONVENTIONS — must be validated before any runtime NGX use:
//   • Jitter: EDPE Halton sequence (render pixels) vs NGX expects jitter in
//     input/render pixels. Must match sign and scale at the NGX boundary.
//   • Motion vectors: EDPE generates camera+depth motion in render-resolution
//     pixels (DXGI_FORMAT_R16G16_FLOAT). NGX expects MV scale conversion
//     (scale X/Y may be zero, substituted as 1.0 by SDK helper).
//   • Depth: EDPE depth format observed as R32G8X24_TYPELESS / D32_FLOAT_S8X24_UINT
//     or R24G8_TYPELESS / D24_UNORM_S8_UINT. NGX expects DepthInverted flag
//     evidence from actual resources.
//   • Reset: EDPE camera/projection changes detected via constant-buffer
//     inspection. NGX InReset=1 requests history reset on complete scene change.
//   • HUD: Copied HDR color visibly includes the HUD; NGX must handle or
//     ELPE must strip HUD before NGX evaluation (validated separately).
//
// Design principle: every subsystem must answer "Did it run? Which backend?
// What dimensions? What formats? What temporal conventions? Why did it fail?
// What did it cost?" (per AGENTS.md diagnostics section).
//
// Testing approach:
//   • NGX lifecycle (Init/Eval/Shutdown) must be validated in an ISOLATED
//     D3D11 test process with a wall-clock timeout on Shutdown1 (≥30s).
//     If Shutdown1 does not return within the timeout, terminate the process.
//     Do NOT attempt to hide the hang via threading — the SDK shutdown thread
//     may remain active. The isolated test proves (or disproves) safe shutdown
//     for the production Elite injector.
//
// Do not consume NGX as production input in Elite until:
//   (a) Shutdown lifecycle is validated in isolated process, AND
//   (b) All Elite input conventions are verified (see nvidia-dlaa-input-contract.md).

} // namespace edpe