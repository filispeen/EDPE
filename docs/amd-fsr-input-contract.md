# AMD FSR temporal input contract

Date: 2026-09-27. Target: Elite Dangerous Odyssey 2D, observed build
`2026.09.03.332841`. Method: inspect AMD's current
[FSR 3.1.5 upscaler guide](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/techniques/super-resolution-upscaler.md)
and [FSR API guide](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/getting-started/ffx-api.md).
No FidelityFX evaluation was run in Elite.

## SDK-documented inputs and boundary

- FSR 3.1.5 receives current-frame color and single-component depth at
  render resolution, 2D motion at render resolution by default, and an
  output at presentation resolution. Motion describes a current pixel's
  position in the previous frame in screen pixels. `motionVectorScale`
  converts other stored units into that convention.
- Render-resolution color and depth should reflect the applied subpixel
  jitter; motion normally excludes jitter. AMD provides a Halton 2,3
  jitter query in pixel units and requires the applied offset to be passed
  at dispatch. A camera cut uses `reset=true` for its first frame.
- Creation flags describe inverted and infinite depth independently.
  Exposure may be provided as a 1x1 value or calculated automatically.
  Reactive and transparency/composition masks are optional inputs with
  distinct meanings; absence has a documented fallback but may reduce
  quality on blended or animated content. Frame delta is in milliseconds.
- Native AA uses equal render and presentation dimensions in
  [AMD's quality table](https://gpuopen.com/amd-fsr-upscaling/).
  The current signed FSR API effect DLLs expose a DirectX 12 backend;
  its guide does not provide a D3D11 backend. A community D3D11 route
  remains a separate provenance, quality, and overhead decision.

## EDPE mapping and limits

- **VERIFIED / MEASURED:** EDPE's experimental GPU motion output is
  current-to-previous in render pixels and its DSV preview supports
  inverted depth. A one-shot HDR color copy, depth copy, and motion pass
  share an observed `Present` interval. The HDR copy includes HUD graphics.
- **EXPERIMENTAL:** Exact depth far-plane behavior, moving-object motion,
  color/depth pixel correspondence, exposure, reactive regions, and camera
  cuts remain unverified. EDPE's Halton math has no verified raster write
  point or fail-open removal path. Do not enable FSR with these diagnostic
  resources yet.
- **IMPLICATION:** Keep EDPE motion in its measured pixel convention and
  translate only at the selected backend boundary. Compare the community
  D3D11 route with official D3D12 interoperability using measured quality,
  copies, cost, compatibility, and license before choosing production FSR.
