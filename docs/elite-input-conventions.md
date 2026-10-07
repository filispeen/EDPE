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
4. NO GAME, later: add a read-only, throttled, nonblocking per-Present camera delta log (translation distance, rotation angle) on the existing staged constant-buffer path. Needed because step 2 and 3 only see one adjacent pair.
5. GAME C, reset semantics, after step 4. Perform jump or supercruise entry and exit, dock and undock, menu to cockpit, view switch. Send back the delta log lines around each event with the event name and rough time. Decides: the camera delta that separates normal motion from scene cuts, which is the basis for `InReset`.

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
