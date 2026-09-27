# NVIDIA D3D11 DLAA input contract

Date: 2026-09-27. Target: Elite Dangerous Odyssey 2D, observed build
`2026.09.03.332841`. Method: inspect NVIDIA's current public NGX D3D11
[evaluation helper](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_helpers_d3d.h),
[feature definitions](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_defs.h),
and EDPE's [renderer observations](render-pipeline-observations.md).
No NGX evaluation was run in Elite.

## SDK revision and provenance

**SDK-DOCUMENTED (checked 2026-09-27):** The official
[NVIDIA/DLSS v310.9.1 release](https://github.com/NVIDIA/DLSS/releases/tag/v310.9.1)
at commit `374959484e79a640feaba44c93ac8cfb0a03f5b5` supplies the
D3D11 headers, x64 NGX import library, and DLSS runtime. Its D3D11
evaluation helper confirms the input semantics below. The SDK uses
[NVIDIA's proprietary RTX SDK license](https://github.com/NVIDIA/DLSS/blob/v310.9.1/LICENSE.txt);
the local copy stays in ignored `build/tools` and no SDK files or runtime
binaries are committed to EDPE.

The current `nvsdk_ngx.h` documents
`NVSDK_NGX_D3D11_Init_with_ProjectID` for custom engines using a unique
GUID-like project ID and `NVSDK_NGX_ENGINE_TYPE_CUSTOM`. EDPE must use its
own identifier; the EDVR ID is not reusable. NGX initialization and the
capability query were measured only in the isolated probe below.

## Standalone NGX lifecycle probe — 2026-09-27

**MEASURED (official SDK v310.9.1, RTX 3060, no Elite process):** An
ignored local D3D11 test executable used a fresh probe-specific GUID-like
project ID with `NVSDK_NGX_ENGINE_TYPE_CUSTOM`. Hardware
`D3D11CreateDevice` returned `0x00000000`; NGX D3D11 initialization and
`GetCapabilityParameters` each returned success (`0x00000001`).
`SuperSampling.Available` returned 0 without a feature DLL search path and
1 when `NVSDK_NGX_FeatureCommonInfo::PathListInfo` pointed at the SDK's
`Windows_x86_64/rel` directory. No NGX feature was created or evaluated.

**MEASURED / BLOCKED:** The isolated probe did not return from
`NVSDK_NGX_D3D11_Shutdown1` within more than 20 seconds, with either the
D3D11 device or `nullptr`; the test process consumed CPU until stopped.
This repeated across three runs. The cause is unknown. Capability success
does not establish safe initialization/shutdown for EDPE, so NGX remains
outside the game process until its lifecycle is understood. The local SDK
and probe executable remain ignored build artifacts.
The probe also produced `dlls/nvngx_dlss*.dll` in the repository root;
that SDK-created directory is ignored and none of its binaries are tracked.

**MEASURED (verbose callback, same date):** With SDK logging written to an
unbuffered local file, `DestroyParameters` returned success (`0x00000001`).
The probe then entered `Shutdown1(nullptr)` and timed out after 20 seconds.
The final SDK messages reported telemetry shutdown and two
`NGXCubinGeneric::Shutdown` resource lists of size 0. No feature had been
created. This narrows the stall to SDK shutdown but does not identify its
cause. The probe was terminated; its full log remains under ignored `build/`.

## SDK-documented D3D11 inputs

- `NGX_D3D11_CREATE_DLSS_EXT` sets input width/height, output width/height,
  quality value, and creation flags. Native-resolution DLAA must use equal
  input and output dimensions; that does not validate Elite's inputs by itself.
- `NGX_D3D11_EVALUATE_DLSS_EXT` binds distinct color, output, depth, and
  motion resources. The helper describes jitter in **input/render pixels**.
  Motion-vector scale converts stored values to pixel space; when either
  scale is zero, the helper substitutes `1.0` for that axis.
- `InReset=1` requests a history reset when the scene changes completely.
  The D3D11 evaluation structure also accepts an exposure texture and a
  current-color bias mask. Its transparency-mask field is marked unused and
  reserved. The helper passes frame delta in milliseconds where supplied.
- Creation flags include `IsHDR`, `MVLowRes`, `MVJittered`, `DepthInverted`,
  and `AutoExposure`. Each flag needs evidence from the resources and
  conventions actually submitted. The definitions alone do not establish
  motion direction, Y sign, or the correct flag combination for Elite.

## EDPE readiness

- **VERIFIED / MEASURED:** A one-shot `2560×1440` D3D11 capture associated
  depth, HDR color, and GPU camera-depth motion with the same observed
  `Present` interval. The copied HDR color visibly includes the HUD.
  WARP tests check the EDPE shader's current-to-previous motion in render
  pixels; an in-game snapshot shows finite motion during camera movement.
- **EXPERIMENTAL:** The depth/camera match is based on observed bind order
  and sparse samples. Per-pixel color/depth correspondence, scene resets,
  exposure, moving-object motion, and the optimal evaluation point remain
  unverified. The motion preview omits HUD shapes while HDR color contains
  them, so the current pair is not a complete temporal description of HUD.
- **BLOCKED FOR RUNTIME DLAA:** Read-only captures identify a camera-buffer
  write before the main scene pass. A one-frame shader census found that
  248/251 draws in its heavy pass used the observed projection block for
  clip X/Y/W, while three used a separate, transposed copy in VS slot 0.
  Other scenes and pass orders, plus a guaranteed way to remove jitter if
  evaluation fails, remain unverified. The current Halton sequence must
  remain inactive in Elite. NGX must not consume diagnostic captures as
  production inputs until that safety condition and the input conventions
  are verified.

**Implication:** Keep EDPE's scene observations and motion generation
backend-neutral. Translate measured EDPE conventions at the future NGX
boundary; do not infer them from EDVR or from a visually plausible snapshot.
