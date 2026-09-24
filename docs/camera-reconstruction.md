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

**MEASURED (2026-09-25, user-confirmed gameplay capture; game build not
rechecked):** The existing bind-3 and bind-6 readbacks logged all 16 float
words 1080–1095 in one requested frame. The four printed rows were identical
at both binds: `(-0.940040827,-0.119412839,0,-0.253565669)`,
`(0.0020389352,1.66782808,0,-0.269775897)`,
`(0.256004214,-0.451764196,0,-0.928937793)`, and
`(0,0,0.0250000004,0)`. The separately logged camera rows 932–943 differed
slightly between bind 3 and bind 6. The scene selector produced visible
depth and color snapshots, and Elite continued running. This establishes
only that the logged 2D candidate block was stable at these two observation
points; it does not establish its matrix layout, jitter convention, or value
at the intervening draws.

**MEASURED NUMERICAL FIT (same capture):** Taking camera rows 932–943 as
three rotation rows, the first three rows of the 2D block fit those rotation
rows with column multipliers `(sqrt(3)/(2560/1440), sqrt(3), 1)`, using
block columns 0, 1, and 3 respectively. The largest absolute difference
across nine compared values is `0.000000346` against the bind-6 camera,
versus `0.000015276` against the bind-3 camera; the camera log is rounded
to roughly six significant digits. This supports an association between
the 2D block and the later camera orientation in this frame. It does not
prove which camera values the actual depth-writing draws consumed, nor
whether the multipliers have the same meaning in other graphics settings.

**MEASURED (2026-09-25, second user-confirmed gameplay session; game build
not rechecked):** After rotating the camera, two further snapshots selected
session-local DSV `#2`. They had 11 and 10 binds respectively and produced
visible depth and HDR color images without closing Elite. In each capture,
the logged camera and 2D block values at binds 3 and 6 were identical at
their printed precision. The camera rotation differed substantially between
the two captures. The same column-multiplier calculation gave largest
absolute differences of `0.000011742` and `0.000002774` across the nine
rotation-related entries. The relation persists approximately across these
orientations, but the first residual exceeds camera-log rounding alone;
the block may reflect a slightly different camera update. A bind-time read
does not establish which values the depth-writing draws used.

**EXPERIMENTAL NEXT CHECK:** The requested-frame buffer probe now also
samples DSV bind 4, between the existing bind-3 and bind-6 samples. In the
observed seven-bind pattern, bind 4 is where the same HDR RTV moves from
MRT slot 3 to RTV slot 0. Comparing all three camera samples can locate a
buffer change relative to that pass boundary. It still cannot identify
the exact draw or prove that the buffer was bound throughout a pass.

**MEASURED (2026-09-25, user-confirmed gameplay capture; game build not
rechecked):** Session-local DSV `#1` had seven binds, and the user saw both
snapshots while Elite kept running. Camera words 932–943 printed identically
at binds 3 and 4. By bind 6, rotation entries changed by up to about
`0.000003`, while all three translation entries printed identically. All
16 logged words of the 2D block were identical at binds 3, 4, and 6. Thus
the observed camera-row change occurred after the bind-4 sample and before
the bind-6 sample in this frame. This does not identify the responsible
buffer write or associate either sample with particular draws.

**EXPERIMENTAL NEXT CHECK:** A requested snapshot now brackets the work after
DSV binds 3 and 6 with one-shot D3D11 pipeline-statistics queries. EDPE ends
each interval at the next `OMSetRenderTargets` call or `Present`, then polls
`GetData` once per later frame with `D3D11_ASYNC_GETDATA_DONOTFLUSH`. The
logged IA primitive and VS/PS invocation counts can establish whether
graphics work occurred in those intervals without another draw hook. They
do not identify individual draws, depth writes, or which camera values a
shader consumed. Query failure leaves the original rendering untouched.
See Microsoft's [query type](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query)
and [non-flushing readback](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_async_getdata_flag)
contracts.

**VERIFIED (Release WARP smoke test):** A test vertex shader and one triangle
between bind 3 and the next target bind produced `iaPrimitives=1` and
`vsInvocations=3`; the empty bind-6 interval produced zeros. Both direct
and staged-proxy smoke variants passed.

**MEASURED (user-confirmed gameplay capture, 2026-09-25; game build not
rechecked):** In the frame after `Present` 14279, the main-camera DSV bound
seven times. The interval after bind 3 reported 39,552 IA primitives,
69,616 VS invocations, and 39,259 PS invocations. The interval after bind 6
reported 90 IA primitives, 180 VS invocations, and 267,900 PS invocations.
The requested capture also copied `2560×1440` depth and HDR scene color;
the user saw both snapshots and reported normal gameplay.

**INFERENCE / LIMIT:** Bind 3 covers substantial geometry work, while bind
6 has a much smaller primitive count and more pixel-shader invocations.
This supports the earlier MRT-then-HDR pass interpretation. These query
intervals do not identify individual draws, depth writes, shader camera
inputs, or the exact point at which depth and color become a matched pair.
Camera selection for reprojection remains unverified.

**EXPERIMENTAL NEXT CHECK:** The same requested DSV capture now logs RTV0
identity, format, dimensions, and bind flags at each target bind. This uses
the existing `OMSetRenderTargets` arguments and runs only for the requested
snapshot. Compare bind 3 and bind 6 color targets with their camera rows to
determine whether the observed camera change crosses a color-pass boundary.
An RTV association still does not identify the exact draw that wrote each
depth pixel.

**MEASURED (Elite Odyssey `332841`, user-confirmed gameplay capture):**
Session-local main-camera DSV `#2` bound seven times between `Present`
12666 and 12667. Its RTV0 was the same `2560×1440`
`R10G10B10A2_UNORM` resource (DXGI format 24) at binds 1–3, then a different
`2560×1440` `R11G11B10_FLOAT` resource (format 26) at binds 4–7.
Those format names are defined by the installed Windows SDK `dxgiformat.h`.
The bind-3 and bind-6 camera rows differed by at most `0.000008` per logged
rotation component, approximately `0.00065°` from their rounded 3×3 rows;
translation and projection word 1094 printed identically. The depth copy
contained 64 finite nonzero grid samples, and the user reported normal
game, HUD, and F5 behavior.

**IMPLICATION / LIMIT:** The two camera samples straddle an RTV0 change,
so they do not represent two points in one unchanged color-target pass.
RTV0 alone does not reveal the purpose of either pass, all eight bound RTVs,
draw ordering, or which camera state produced the final depth. Do not choose
bind 3 or 6 as the production camera from this observation.

**EXPERIMENTAL NEXT CHECK:** The one-shot color-target log now enumerates
each of the up to eight RTV slots supplied with the selected DSV bind,
including null slots. This extends the already-tested RTV0 observation;
it does not intercept draws or modify any resource. The attachment sets
may distinguish geometry and later color passes more reliably than RTV0.

**MEASURED (Elite Odyssey `332841`, user-confirmed gameplay capture):**
Session-local main-camera DSV `#1` bound seven times between `Present`
14279 and 14280, always with `NumViews=8` but with null entries in unused
slots. Binds 1–3 had the same four nonnull `2560×1440` RTVs:
`R10G10B10A2_UNORM` at slot 0, `R8G8B8A8_UNORM` at slots 1 and 2, and
`R11G11B10_FLOAT` at slot 3. Bind 4 moved that **same slot-3 RTV object**
to slot 0 and paired it with an `R16_FLOAT` RTV at slot 1. Binds 5–7 kept
only that `R11G11B10_FLOAT` RTV at slot 0. All nonnull textures reported
bind flags `0xA8` (shader resource, render target, unordered access). The
installed Windows SDK `dxgiformat.h` defines the format names. Bind-3 and
bind-6 camera rows changed slightly while translation and projection word
1094 remained the same; the depth grid was nonuniform and the user reported
normal rendering.

**INFERENCE / LIMIT:** The four-target first phase is consistent with a
geometry/MRT pass, followed by processing into a shared HDR color target.
The exact shader purpose, draw counts, depth writes, and final scene-color
handoff are unverified. The stable RTV object link is stronger than a
dimension-only classification, but it still does not identify which camera
update matches the depth pixels or when the HDR texture is safe to sample.

## Experimental HDR color snapshot — 2026-09-24

**SDK-DOCUMENTED:** [Microsoft's `CopyResource` contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource)
permits an asynchronous GPU copy between distinct resources of the same
type and dimensions with compatible formats; neither resource may be mapped.

**EXPERIMENTAL:** On a requested depth snapshot, EDPE retains the last RTV0
bound with that DSV. At the following `Present`, it accepts only a single-
sample 2D `R11G11B10_FLOAT` texture, copies it once into an EDPE-owned
shader-resource texture, and opens a separate raw-color ImGui window. The
source RTV is not modified. Failure to obtain a compatible source, copy
texture, or SRV leaves the game's original frame untouched. A visible image
would identify the candidate's contents at this point, but would not prove
that it is HUD-free or that `Present` is the eventual temporal handoff.

**VERIFIED (user observation, 2026-09-25 gameplay session; game build not
rechecked):** The `EDPE Scene Color Snapshot` window showed the 3D world
without the HUD, while Elite kept running. The user requested one snapshot
of the main-camera DSV. The corresponding log records session-local DSV
`#1`, seven binds between `Present` 17899 and 17900, a `2560×1440`
format-19 depth copy, and a `2560×1440` format-26
(`R11G11B10_FLOAT`) color copy. The last RTV0 bound with that DSV was the
same HDR RTV object present at slot 3 during binds 1–3 and slot 0 during
binds 4–7. The log also confirms ImGui submitted both snapshot images.

**IMPLICATION / LIMIT:** This identifies a usable HUD-free scene-color
*candidate* in this gameplay frame. The screenshot was not saved; the
HUD-free assessment is the user's direct visual report. The copy occurred
at `Present`, after the observed DSV binds, so it does not establish the
earliest safe copy point, whether later game passes overwrite the source,
or exact color/depth/camera correspondence. The DSV index is session-local.

**EXPERIMENTAL NEXT CHECK:** The same requested snapshot now reports
whether its exact HDR RTV is still bound to any output-merger RTV slot at
`Present`. This uses D3D11's existing
[`OMGetRenderTargets`](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omgetrendertargets)
query and releases its returned references. The result checks output binding
at that point only; it cannot prove that all scene-color writes are complete
or that the source is no longer bound through another view.

**VERIFIED / MEASURED (user-confirmed gameplay capture, 2026-09-25; game
build not rechecked):** The main-camera depth candidate was session-local
DSV `#2` this time. It bound seven times between `Present` 15167 and 15168.
The same `2560×1440` `R11G11B10_FLOAT` RTV object appeared at slot 3 in
binds 1–3 and at slot 0 in binds 4–7. EDPE copied its color and the
format-19 depth at `Present` 15168, submitted the color image to ImGui, and
logged `scene color RTV unbound at Present`. The user reported that the game
and HUD-free HDR world snapshot remained normal. This verifies that the
*exact retained RTV view* was not bound to an output-merger RTV slot at this
copy point in this run. It does not check alias views, UAV binding, later
writes, or exact temporal correspondence.

## Experimental scene-depth selection — 2026-09-25

**MEASURED BASIS:** In two user-confirmed gameplay captures, the main-camera
DSV was `#1` and then `#2`. Both had a `R32G8X24_TYPELESS` depth texture
and the same four-target MRT pattern: `R10G10B10A2_UNORM`, two
`R8G8B8A8_UNORM` targets, and `R11G11B10_FLOAT`. The exact HDR RTV then
moved from MRT slot 3 to slot 0 within the same `Present` interval.

**EXPERIMENTAL:** EDPE now offers an optional scene-candidate snapshot
button. It selects a single recent DSV only when that depth format,
single-sample layout, MRT format pattern, and same-frame HDR RTV identity
all match. No match or multiple matches disable the button; the manual
index remains available. This recognizes an observed pass pattern, not a
production-safe scene-depth identity. It has a WARP smoke test; the first
Elite check is recorded below.

**VERIFIED / MEASURED (user-confirmed gameplay test, 2026-09-25; game
build not rechecked):** The experimental button was active, and pressing
it produced visible depth and scene-color snapshots without
closing Elite. The resulting log selected session-local DSV `#1` twice.
Both captures had seven DSV binds in one `Present` interval (`14248`–`14249`
and `14498`–`14499`), copied `2560×1440` format-19 depth and format-26
HDR color, and submitted both ImGui images. The same HDR RTV appeared at
MRT slot 3 during binds 1–3 and slot 0 during binds 4–7. This validates
the experimental selector for this gameplay process; it does not establish
that the signature is unique across all game scenes or versions.

## Required before runtime reprojection

Verify which buffer update and scene pass provide the camera for each depth
frame, including scene transitions. Verify projection X/Y, Y orientation,
near/far behavior, and whether a previous-camera transform can be captured
without a blocking readback. Keep camera selection tied to resource/pass
evidence rather than a session-local DSV number. Until then, do not feed this
depth or camera candidate to DLSS or FSR, and do not apply projection jitter.
