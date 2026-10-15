# Elite input conventions: observation plan

Date: 2026-10-07. Target: Elite Dangerous Odyssey 2D, build `2026.09.03.332841` for the stored blocks. Scope: STATUS.md milestone 2 (jitter sign, motion scale, depth flag, reset semantics). Everything here is observation only. NGX is not called in Elite, no jitter is applied in Elite, DLAA and DLSS SR are not started.

## Open conventions and what is already known

- Jitter: EDPE Halton offsets are in render pixels (`projectionJitter`). Sign and `MVJittered` handling at the NGX boundary are unresolved. Whether Elite already folds a sub-pixel offset into its own projection block was not measured before step 1.
- Motion: EDPE writes current to previous in render pixels, +Y down (`cameraDepthMotion`, WARP and CPU parity checked). Direction and scale versus NGX are unresolved; no image-level check exists.
- Depth: reversed Z with `depth = depthB / viewZ`, `depthB = 0.025`, is supported by camera-reconstruction.md. Clear value and sky behavior were seen only in sparse samples.
- Reset: no measurement of camera discontinuities exists. Only one-shot adjacent pairs are captured.

## Plan

1. DONE, no game: `estimateEliteBlockNdcOffset` estimates the NDC offset already folded into a projection block, and the existing `scene camera candidate` log line now prints it as `nativeNdc` and `nativeValid`. Test pins the EDPE jitter sign by round trip.
2. GAME A, jitter included or excluded, depth flag and clear value. In the F5 menu press `Capture adjacent camera frames (experimental)` and then `Capture depth candidate (one frame)` once in each of three scenes: (a) cockpit, stationary, (b) flying straight forward at steady speed toward a large static surface (station or planet), (c) looking at empty sky only. Send back, labeled a/b/c, the `edpe.log` lines matching `scene camera candidate|adjacent depth|depth sample grid|adjacent camera probe`. Decides: `nativeNdc` near zero in every scene means the game does not jitter; center depth rising in (b) and equal to 0 in (c) confirms reversed Z with clear 0; `0.025/depth` change versus forward displacement gives the scale check.
3. GAME B, motion units and direction. Press `Capture motion candidate (experimental)` (a) during a steady pure yaw, (b) during steady pure forward flight. Send back the `motion grid 5x5` line, both `scene camera candidate` lines, the four `scene CB 2D xy/zw` lines of each sample, the `adjacent depth` lines, and the `edpe-captures` BMP plus the F5 snapshot screenshot. Decides: GPU grid versus CPU `cameraDepthMotion` within 0.01 px, yaw value versus `W/2 * scaleX * tan(angle)`. Limit: this does not prove direction against the real image; that needs two color frames and stays open.
4. DONE, no game: read-only, throttled, nonblocking per-frame camera delta log. See "Step 4 result" below.
5. GAME C, reset semantics. No F5 action is needed; the log runs whenever the scene MRT pass is drawn. Perform, with a few seconds of normal flight between them: hyperspace jump, supercruise entry, supercruise exit, docking, undocking, main menu to cockpit, external camera on and off, galaxy map open and close. Note each event name and rough wall-clock time. Send back all `edpe.log` lines matching `camera delta` with their timestamps. Decides: the per-frame `move`/`turnDeg` range of normal flight (from `summary`) versus the values at scene cuts (from `event`), which is the basis for `InReset`; also whether camera translation is world-absolute or floating-origin (`pos` jumps without motion).

## Step 1 result

Date: 2026-10-07. Method: for a block with rows 0 to 2 as view-space inputs and columns clip X, Y, Z, W, column W is the view Z axis and columns X and Y are orthogonal to it for a rigid camera. The component of columns X and Y along column W is therefore an offset already folded into the block, with the same sign as `ProjectionJitter::ndcX/ndcY`. The estimator uses only the block, so the lag between camera rows and block seen during fast turns does not enter it.

Evidence from this session:

- `ctest -C Release -j 1`: 6 of 6 pass, including `temporal_math`.
- `temporal_math` checks that the two stored Odyssey 332841 blocks give an offset below `1e-6` NDC on both axes, that Halton offsets from `jitterEliteProjectionBlock` at 2560x1440 are recovered within 0.01 px with the same sign (`pixelX = ndcX*W/2`, `pixelY = -ndcY*H/2`), and that a block without a W column and a null pointer are rejected.
- Offline double arithmetic on the same two blocks (9 digit values): `(4.7e-9, -8.4e-10)` and `(-8.9e-9, -3.8e-9)` NDC.
- Release WARP proxy smoke log shows the new fields: a zero buffer prints `valid=0 ... nativeNdc=(0,0) nativeValid=0`.

Confidence: MEASURED on two stored blocks from one build; no live Elite data yet. The estimator accuracy depends on the camera axes being orthogonal, which the parser only checks to 0.02.

Implications:

- EDPE side jitter convention is now pinned by a deterministic test: positive `pixelX` moves scene content right, positive `pixelY` moves it down, `ndcY = -2*pixelY/height`. What NGX expects remains unresolved.
- The stored blocks show no built-in offset. Whether this holds across scenes is the question of step 2. `MVJittered` stays unset in the plan until step 2 is read.

## Step 4 result

Date: 2026-10-15. No game run.

Method: on the first bind per frame of a single-sample `R32G8X24_TYPELESS` DSV with the scene MRT pattern (`R10G10B10A2`, `R8G8B8A8`, `R8G8B8A8`, `R11G11B10`), EDPE retains the 5376-byte dynamic VS slot-1 buffer. At the next `OMSetRenderTargets` (end of that interval) it queues one `CopyResource` into a ring of four persistent staging buffers, created once each. At `Present` it maps finished copies in order with `D3D11_MAP_FLAG_DO_NOT_WAIT` and stops at the first still drawing. A full ring drops the frame and counts it. `parseEliteCamera` reads each copy; `edpe::CameraDeltaTracker` (`src/camera_delta.h`) compares it with the last valid camera. Nothing is written to the game, no jitter is applied, NGX is not called.

Log lines (all contain `camera delta`):

- `first sample frame= valid= window=120`: once per swap chain; proves the path ran.
- `summary time= frames=a-b samples= invalid= gaps= dropped= maxMove= meanMove= maxTurnDeg= meanTurnDeg= events= suppressed=`: every 120 samples.
- `event time= frame= flags= valid= gap= move= turnDeg= projection= depthB= pos=(x,y,z) scale=(sx,sy)`: at most 8 per window. Flags: `0x1` turn above 2 degrees, `0x2` move above 50 scene units, `0x4` scale or depthB change above 1e-4, `0x8` Present frames skipped since the last valid sample, `0x10` parser rejected after a valid sample, `0x20` first valid sample after rejections.

Conventions: `move` is the distance between the translation columns of the row-major view-to-world `[R | C]` in scene units; `turnDeg` is the angle of `R_prev^T * R_now`. `frame` is the Present count before the sampled scene pass. Thresholds only choose which lines to print; they are not reset thresholds.

Evidence:

- `ctest -C Release -j 1`: 7 of 7 pass, including the new `camera_delta` test (translation, yaw both orders, FOV change, gap, lost and regained, rate limit, window rollover).
- Release WARP proxy smoke log contains `EDPE: camera delta log first sample frame=7 valid=0 window=120`; the synthetic buffer is not a valid camera, so `valid=0` is expected.

Confidence: CPU math and the D3D11 copy and readback path are tested; the sampling point in Elite is not. Limits: the camera is taken from the first scene-MRT interval only. Earlier Unmap hashing (camera-reconstruction.md, 2026-09-27) found identical camera and projection hashes across all writes within a frame, so this should be the frame camera, but this is not proven for every scene. Frames without that MRT pattern (menus, loading, galaxy map) produce no sample and appear as a `gap` on return.

## In-Game Capture Results (2026-10-15)

Date: 2026-10-15. Target: Elite Dangerous Odyssey 2D, build `2026.09.03.332841`, 2560×1440.
Method: direct D3D11 staging readbacks of candidate textures via `edpe-captures/`.

1. **Scene Depth (DSV #2):**
   - Candidate: DSV #2 (`DXGI_FORMAT_R32G8X24_TYPELESS`).
   - Verified: Decoded to 32-bit BMP (`depth-dsv2-*.bmp`) using $z / (z + 0.001)$ contrast curve. Yields clear, continuous monochrome geometry across cockpit and world geometry (values 35 to 177, avg 90.7).
   - Convention: Reversed-Z confirmed with clear 0.0, near values approaching small non-zero float, and $B = 0.025$.

2. **Emissive Pass / Reactive Mask Source (`early-color`):**
   - Candidate: MRT RTV3 (`DXGI_FORMAT_R11G11B10_FLOAT`), copied at first RTV3 unbind.
   - Verified: In-game visual inspection of `early-color-*.bmp` confirms this is **not** outer space or an early scene snapshot. It is the engine's **emissive / illumination pass**, where non-emissive scene elements are pure black ($R=G=B=0$) and all glowing elements (cockpit lamps, dials, indicators, ship lights, laser/shield glows) are isolated and bright (up to RGB 254).
   - Architectural role: Direct native candidate for `TemporalFrameInputs::reactiveMask`. Eliminates ghosting and smearing behind moving glowing elements in both DLSS and FSR without relying on costly heuristic luminance edge-detection passes.

3. **Motion Buffer Baseline:**
   - Candidate: `DXGI_FORMAT_R16G16_FLOAT` render target from `MotionPass`.
   - Verified: Decodes to neutral gray $(128, 128, 128)$ on static frames, confirming baseline displacement of 0.0 pixels.

