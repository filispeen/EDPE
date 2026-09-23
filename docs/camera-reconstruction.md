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
cannot be attributed to the beacon. The numerical proximity of `415.391`
and 444 is incidental; this capture does not test the beacon's distance,
calibrate scene units to metres, or verify the depth equation at short range.
Another check needs a static object visibly present at the sampled pixel
in the depth preview.

## Required before runtime reprojection

Verify which buffer update and scene pass provide the camera for each depth
frame, including scene transitions. Verify projection X/Y, Y orientation,
near/far behavior, and whether a previous-camera transform can be captured
without a blocking readback. Keep camera selection tied to resource/pass
evidence rather than a session-local DSV number. Until then, do not feed this
depth or camera candidate to DLSS or FSR, and do not apply projection jitter.
