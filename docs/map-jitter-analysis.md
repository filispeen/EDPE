# Galaxy Map & System Map Jitter Analysis (Issue #24)

Date: 2026-11-22. Target: Elite Dangerous Odyssey (2D Direct3D 11, FSR Native AA / DLSS / TAA, `PreserveNativeUi=1`).

Method: RenderDoc frame inspection (`galmap_upscaled.rdc`, `galmap_bypass.rdc`, `sysmap_upscaled.rdc`, `sysmap_bypass.rdc`), constant-buffer data inspection, DXBC vertex-shader disassembly, pixel history, and runtime log correlation (`edpe (upscaled).log`, `edpe (bypass).log`).

Confidence: High (all three root causes directly verified in captured D3D11 state, shader disassembly, and buffer contents).

---

## 1. False `cb0_is_jittered` Viewport Cancellation in `UiLayer`

- **Evidence**:
  - In `galmap_upscaled.rdc`, Pass 34 (EID `685..2548`, 102 draws) renders system names, nebula labels, grid coordinate labels, icons above systems, and the user cursor ring into EDPE's offscreen `UiLayer` HDR texture (`ResourceId::146618`) via `kMapCanvas` (`VS 0x12382D2EA45E9632`) and `kMapSprite` (`VS 0xC31238331D8AC3D4`, DXBC hash `5b363732-...`).
  - Both shaders declare `cb0[12]` (192/208-byte shared dynamic buffer `ResourceId::7562`) and `cb1[279]` (`ResourceId::146664` = EDPE `s_clean_camera_cb`), reading `cb0[9..11]` only as a 3x4 local/model matrix and projecting clip `SV_Position` via **`cb1[270..273]`** — which `replayHudDraw` already unjittered (`cb1_substituted = true`).
  - Because earlier scene draws in the frame (e.g. EID `419`) wrote a jittered transpose block to the same 208-byte `ResourceId::7562` and set `s_cb0_jittered_frame = motion_color_frame`, `replayHudDraw` checked only `cb0Desc.ByteWidth == 192 || cb0Desc.ByteWidth == 208` and falsely set `cb0_is_jittered = true`, shifting the `UiLayer` viewport by `(-jitterX, -jitterY)` on all already-clean draws.
- **Implication / Fix**:
  - Classify vertex shaders at creation time (`kEdpeVsProjCbGuid` via `edpe::classifyVsProjectionCb`) into `None`, `Cb0Dot4To7`, `Cb1Proj270To273`, `Both`, or `Unknown`.
  - Track the specific `ID3D11Resource*` in `s_jittered_cb0_buffer` (cleared on non-jittered/deferred writes), and require both `orig_cb0 == s_jittered_cb0_buffer` and `Cb0Dot4To7`/`Both` (or `Unknown && !cb1_substituted`) before applying viewport jitter cancellation.

---

## 2. System Map 3-Frame Camera Rejection Cycle

- **Evidence**:
  - In `sysmap_upscaled.rdc` (EID `1016`, `1356`) and `edpe (upscaled).log` (frames `104169..105887`), the System Map uses a telephoto perspective (`scaleX=4.476538`, `scaleY=7.958291`, `depthB=0.025`), while Cobra leaves the stale space/cockpit inverse projection in `cb1[35..38]` (`words[140..155]`, `scaleX≈0.9865`, `scaleY≈1.7538`, rotated ~133° relative to the map camera, rotation trace `< 0`).
  - `edpe::isEliteSubpassVersusWorldBlock16` checked only aspect ratio, FOV scale difference, and `depthB` without verifying orientation alignment against the candidate camera. It therefore misclassified System Map's main camera as a viewmodel sub-pass and rejected `cb1` on 2 out of every 3 frames until cut/fallback recovery forced acceptance (`streak=1 -> streak=2 -> step prevFb=1`).
- **Implication / Fix**:
  - Require viewmodel sub-pass detection (`isEliteSubpassVersusWorldBlock16`) and world-scale matching (`matchesEliteWorldProjectionScalesFromBlock16`) to verify camera rotation trace `>= 2.50f` between `cb1[35..38]` and `parsedCam`.
  - Gate `isCb1Subpass` in `isAuthoritativeMainSceneCamera` and `observedUnmap` with `!mapModeActive`.

---

## 3. Float32 Catastrophic Cancellation in `cb0[4..7]` on Absolute Galaxy Map Coordinates

- **Evidence**:
  - In `galmap_upscaled.rdc`, the grid draw (EID `419`, `VS 732015f8`) uses camera-relative coordinates (`cb0[4..7].w ≈ (0, -0.007812, 0.025, 0)`), giving 0 px rounding error under matrix jitter.
  - Stars (EID `436`, `448`, `459`, `649`) and route/vertical lines (EID `660`, `VS 3a26ca7b`) write absolute galaxy coordinates (`≈ (49982, 40879, 24106, 1)`) to `dp4 o.xyzw, cb0[4..7], r` with large translation terms (`cb0[4..7].w = (+56146.945, -80047.602, 0.025, +8569.804)`).
  - Adding subpixel NDC jitter directly into float32 `cb0[4..7]` (even if computed in float64 first and rounded back to float32) quantizes clip X/Y/W at `~2^-8`..`2^-7`, creating ±0.25..1.97 px non-Halton error at nearby star distances (`w = 5..40 LY`).
- **Implication / Fix**:
  - Detect large translations via `edpe::hasLargeEliteTransposeTranslation(block16)` (`max(|block[3]|, |block[7]|, |block[15]|) > 1024.0f`).
  - Leave `cb0[4..7]` clean (unjittered) and apply exact subpixel `(jitter.pixelX, jitter.pixelY)` via `D3D11_VIEWPORT` (`DeferredCb0ViewportJitterScope`) only on draws whose bound VS classifies as `Cb0Dot4To7` and binds that tracked `cb0` buffer.
