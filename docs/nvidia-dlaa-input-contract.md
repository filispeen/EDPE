# NVIDIA D3D11 DLAA input contract

Date: 2026-09-27. Target: Elite Dangerous Odyssey 2D, observed build
`2026.09.03.332841`. Method: inspect NVIDIA's current public NGX D3D11
[evaluation helper](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_helpers_d3d.h),
[feature definitions](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_defs.h),
and EDPE's [renderer observations](render-pipeline-observations.md).
No NGX evaluation was run in Elite.

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
- **BLOCKED FOR RUNTIME DLAA:** EDPE has no verified raster-projection write
  point and no guaranteed way to remove jitter if evaluation fails. Its
  current Halton sequence must remain inactive in Elite. NGX must not consume
  the diagnostic captures as production inputs until that safety condition
  and the input conventions are verified.

**Implication:** Keep EDPE's scene observations and motion generation
backend-neutral. Translate measured EDPE conventions at the future NGX
boundary; do not infer them from EDVR or from a visually plausible snapshot.
