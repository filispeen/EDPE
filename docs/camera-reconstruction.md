# 2D scene camera reconstruction

**Date:** 2026-09-24

**Game executable:** Elite Dangerous Odyssey `332841`

**Status:** MEASURED geometry; per-frame capture and full projection convention remain unverified.

## Method and evidence

On a user-requested scene-depth snapshot, EDPE copies the 5376-byte dynamic
constant buffer bound to VS slot 1 at bind 6 of the selected scene DSV. The
copy is read once through a staging buffer without modifying the source. The
scene DSV is `2560×1440`, `R32G8X24_TYPELESS`; its session-local index changes
between game runs. The user confirmed that its image shows 3D geometry without
the HUD. Full measurements are in
[render-pipeline-observations.md](render-pipeline-observations.md).

In two captures of the same station during one game process, camera words
932–943 formed three approximately orthonormal 3×4 rows. Interpreting their
fourth components as translation and their third rotation column as forward
gives a `401.989`-unit camera displacement almost entirely along that forward
axis. The center-pixel depth changed from `0.0000113825899` to
`0.0000139270014`; the buffer held `0.02500000037` at word 1094 both times.
The candidate view-axis distance `0.025/depth` changed by `401.263` units.

As an independent geometric check, `translation + forward * (0.025/depth)`
placed the center-pixel surface at `(-30.951,167.302,29.204)` and
`(-30.606,166.698,29.605)` in the two captures. Those points differ by
`0.803` units while the camera moved `401.989` units. This supports a
row-major **view-to-world** interpretation with positive view Z for the
observed scene. It is a measured fit for one visible station surface, not a
validated transform for every draw.

Words 1080–1091 follow the camera rotation with X and Y factors near
`sqrt(3)/(2560/1440)` and `sqrt(3)`. Words 1092–1095 are
`(0,0,0.02500000037,0)`; words 1328–1331 are
`(2560,1440,1/2560,1/1440)`. This is a strong 2D scene projection candidate.
EDVR's VR projection words 794–795 are zero in these 2D captures and must
not be used here.

## Current working convention

For **offline analysis of these captures only**, let each camera row contain
three rotation entries and a translation entry. With column vectors:

```text
world = translation + rotation * view
viewZ at image center ≈ projectionZ / rawDepth
projectionZ = 0.02500000037 in the measured captures
```

The exact center sample is pixel `(1280,720)`, half a pixel from the geometric
image center. The small world-point residual can also include a change in
the visible surface hit point. The HUD station-distance change was 470 m,
which does not establish a scene-unit-to-metre conversion because the HUD
reference point and center-pixel surface point differ.

## Navigation-beacon range attempt — 2026-09-24

**MEASURED:** The user reported a navigation beacon at a HUD distance of
444 m. In the same game run, one successful scene candidate `#2` snapshot
used the `2560×1440` format-19 DSV, with eight binds between `Present`
37489 and 37490. Its center raw depth was `0.0000601842221`; the scene
buffer again contained `0.02500000037` at word 1094. The candidate
`0.025/depth` conversion gives `415.391` scene units, numerically 28.609
below the HUD value. The 8×8 grid had 36 nonzero cells and a maximum of
`0.0207383949`.

**VERIFIED (user observation):** The beacon was centered on screen but was
**not visible in the depth snapshot**. Therefore the center-depth value
cannot be attributed to the beacon. A single numerical proximity between
`415.391` and 444 did not test the beacon's distance or calibrate units.

**MEASURED (second capture, same beacon centered, new game process):** The
user approached to a HUD distance of 163 m. The main-camera candidate was
session-local DSV `#1`, again `2560×1440` format 19, with eight binds
between `Present` 22668 and 22669. Center raw depth was `0.00018744418`,
giving `133.373` scene units by `0.025/depth`; projection word 1094 again
held `0.02500000037`. The HUD distance fell by 281 m while the candidate
view-axis distance fell by `282.018` scene units, a differential mismatch
of `1.018`. The offsets from the HUD readings were `28.609` and `29.627`
at 444 m and 163 m respectively.

**INFERENCE / LIMIT:** This is strong differential evidence that the sampled
depth tracks a surface near the centered target with roughly one scene unit
per HUD metre over this range. The user still could not see the beacon in
the depth preview, and the two captures came from separate game processes.
The surface identity, HUD reference point, and exact scale remain unverified;
do not treat the approximately 29-unit offset as the beacon's radius or
feed this DSV to a temporal backend on this evidence alone.

## Early/late scene-buffer probe — 2026-09-24

**REFERENCE-CODE:** EDVR revision `96df075` records multiple writes to its
large scene buffer and selects a camera using scene-draw association and
continuity (`src/d3d11/temporal_pass.cpp`). Its first VR eye draw and
projection offsets do not identify EDPE's 2D scene camera.

**EXPERIMENTAL:** A requested depth snapshot now copies the 5376-byte VS
slot-1 buffer at bind ordinals 3 and 6 of that selected DSV, using the
existing `OMSetRenderTargets` observer. Both staging reads are nonblocking;
the log labels both row sets and 2D projection word 1094, while the full
hex dump remains limited to bind 6. This compares two points in one selected
frame without adding a hook, changing the source buffer, or reading back a
full frame. Different rows would prove that a single bind-time observation
is insufficient; equal rows would not prove they were used by every draw.

**MEASURED (Elite Odyssey `332841`, first gameplay capture with this probe):**
The main-camera depth image was session-local DSV `#1`, `2560×1440`, with
seven binds between `Present` 18377 and 18378. Bind 3 rows began
`(-0.965309,-0.0707018,-0.251356,19.5594)`; bind 6 rows began
`(-0.965288,-0.0706202,-0.251458,19.5594)`. The other two rotation rows
also changed, with maximum logged component difference about `0.000141`.
The row rotations differ by approximately `0.0113°` using the skew of their
relative 3×3 matrix; this estimate uses six-significant-digit log values.
All three translation entries printed identically at both binds, and word
1094 was `0.0250000004` at both. The game and F5 menu remained open.

**IMPLICATION:** The scene-camera candidate is updated between these two
binds within one `Present` interval, even when its position and depth
coefficient do not change. The probe does not show which update was active
at the scene draws that produced the copied depth. Production camera
selection needs draw/pass association before motion or jitter is enabled.

**EXPERIMENTAL NEXT CHECK:** The same requested DSV capture now logs RTV0
identity, format, dimensions, and bind flags at each target bind. This uses
the existing `OMSetRenderTargets` arguments and runs only for the requested
snapshot. Compare bind 3 and bind 6 color targets with their camera rows to
determine whether the observed camera change crosses a color-pass boundary.
An RTV association still does not identify the exact draw that wrote each
depth pixel.

## Required before runtime reprojection

Verify which buffer update and scene pass provide the camera for each depth
frame, including scene transitions. Verify projection X/Y, Y orientation,
near/far behavior, and whether a previous-camera transform can be captured
without a blocking readback. Keep camera selection tied to resource/pass
evidence rather than a session-local DSV number. Until then, do not feed this
depth or camera candidate to DLSS or FSR, and do not apply projection jitter.
