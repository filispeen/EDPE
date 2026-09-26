# GPU motion-vector prototype

**Date:** 2026-09-26

**Game build:** Not involved; standalone D3D11 WARP test.

**Status:** EXPERIMENTAL.

## Method and evidence

EDPE's first GPU motion shader reconstructs a world point from reversed-Z
depth and the current row-major view-to-world camera, projects it through
the previous camera, and writes current-to-previous motion in render pixels.
Positive Y points down. It targets `DXGI_FORMAT_R16G16_FLOAT` through a
render-target view; Microsoft lists render-target support for this format
among [required DXGI formats](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/required-dxgi-formats).
The test checks support at runtime with `CheckFormatSupport` as well.

**VERIFIED (Release WARP test):** A synthetic 4×4 depth texture uses Elite's
observed `R32G8X24_TYPELESS` resource format and an
`R32_FLOAT_X8X24_TYPELESS` shader-resource view. With `depthB=0.025`, raw
depth `0.0025` represents view Z=10. Moving the
current camera +10 units in X and Y while leaving the previous camera at
the origin produced exactly `(2,-2)` in the half-float output. A zero-depth
pixel produced `(0,0)`. All four project tests passed.

**VERIFIED (Release WARP state test):** The motion draw runs under a separate
`ID3DDeviceContextState`. After switching back, the test observes its prior
vertex shader, line topology, and viewport unchanged. Microsoft documents
[`SwapDeviceContextState`](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-swapdevicecontextstate)
as a way for plug-ins to save and restore application state on the immediate
context. The runtime path must check D3D11.1 availability and fail open.

**EXPERIMENTAL game capability probe:** On overlay initialization, EDPE now
queries the game's D3D11 device and immediate context for the 11.1 interfaces,
tries to create a same-feature-level context state, logs availability, and
releases the probe object. It does not activate that state or draw motion.
The WARP proxy smoke tests observe `available=1`.

**VERIFIED (2026-09-26, user-confirmed cockpit run; game build not
rechecked):** Elite logged `available=1` and `HRESULT=0x00000000` for D3D11
context state on overlay initialization. The user reported normal game
world, HUD, and F5 menu behavior. This verifies interface and state-object
creation on that device; no context swap or motion draw ran in Elite.

**VERIFIED (Release WARP module test):** `MotionPass` now owns a persistent
`R16G16_FLOAT` output and a separate D3D11 context state. The test invokes
this production module twice, checks `(2,-2)` and zero-depth output, confirms
that the same output resource is reused, and checks that the original viewport
and topology survive both draws. All four project tests passed.

**VERIFIED (Release WARP camera-matrix parity test):** With row-major camera
rows and projection scales recorded in a user-confirmed Odyssey scene capture,
the test applies a synthetic `(+2.5,-1.25)` world-space camera translation.
At one depth pixel it compares the GPU half-float motion to EDPE's CPU
`cameraDepthMotion` result; both axes agree within `0.01` render pixel. This
checks the shader's matrix order against the CPU implementation for an
observed orientation. It does not verify motion from two actual Elite frames.

## One-shot runtime candidate — 2026-09-26

**EXPERIMENTAL:** The F5 menu offers `Capture motion candidate`. It uses the
existing scene DSV signature and copies VS slot-1 camera buffers at bind 3
in two consecutive frame intervals. After the second interval, it retains a
GPU-only copy of that frame's depth. The camera staging buffers are polled
without waiting; only after both parse successfully and their frame labels
match the depth copy does `MotionPass` draw. A sparse 5×5 staging readback
logs center motion and a spatial range in render pixels. The original game
image is untouched.
The request expires if the pair remains incomplete for 120 Presents.

**VERIFIED (Release WARP proxy smoke):** Synthetic valid camera buffers,
a `64×64` typeless depth target, and a `+0.01` camera X translation yielded
center motion `(6.39844,0)` in the half-float GPU result. The frame labels
were consecutive, the GPU pass completed, and all four project tests passed.
This verifies the one-shot dispatch and resource lifetime in the harness;
it is not an Elite visual or camera-selection validation.

**MEASURED (Elite Odyssey, 2026-09-26; game build not rechecked):** The user
confirmed that the 3D world, HUD, and F5 menu remained normal after one
motion-candidate capture. Session-local DSV `#2` supplied two valid camera
samples labelled `afterPresent=13869/13870`, each with scales near
`(0.974279,1.732051)` and `depthB=0.025`. The matching `2560×1440` center
depths were `0.000304676854/0.000304607762`. The second frame's GPU depth
copy produced finite center motion `(-0.0128174,-0.0128174)` pixels. This
verifies one in-game dispatch and readback, not visual correctness of the
whole motion texture or the exact camera used for every depth draw.

**MEASURED observer fix:** In that run, the DSV observer's context-vtable
slot reverted to the original method after the motion pass and its bind
counter stopped. The WARP proxy reproduced this: the slot was active during
the motion draw but original again after `Present`. EDPE now conditionally
restores only its own observer slot after the original `Present` returns,
and leaves any unknown replacement untouched. The WARP smoke test confirms
that a later DSV bind reaches the observer and that the slot remains active.
The WARP result alone does not establish the restored behavior in Elite.

**VERIFIED (Elite Odyssey, 2026-09-26; game build not rechecked):** After
installing the observer fix, the user repeated the one-shot motion capture
and reported normal game, HUD, and F5 menu behavior. Session-local DSV `#2`
supplied valid cameras at `afterPresent=67395/67396`, with matching center
depth readings near `2.70103e-06` at `2560×1440`. GPU center motion was
`(-0.000244141,0.0000610352)` render pixels. The log recorded
`DSV observer restored after context-state swap`; subsequent census entries
at frames 67584 and 68608 both had `slotActive=1`, and total DSV binds rose
from 964729 to 982874. This verifies continued observation after the pass
in this run. The near-zero motion does not validate moving-scene quality.

**VERIFIED / MEASURED (Elite Odyssey, 2026-09-26; game build not rechecked):**
After the projection-block parser and scene-candidate UI fixes, the user
reported a stable F5 menu, normal game operation, and an `EDPE Motion
Snapshot` window. The supplied screenshot contains scene-shaped regions:
neutral gray near the top and large saturated yellow, green, magenta, and
blue regions elsewhere. The corresponding log records valid adjacent
camera samples at `afterPresent=87592/87593`, `2560×1440` depth center
values `3.05051412e-06/3.05094977e-06`, a successful GPU motion pass and
preview shader, UI handoff, and DSV observer restoration. The center motion
sample was `(-0.00012207,-0.000366211)` render pixels; later DSV census
entries retained `slotActive=1` and increasing bind counts. The saturated
regions suggest motion magnitudes above the preview's nonlinear display
range, but their numeric values and correctness are not yet measured.

**MEASURED (Elite Odyssey, 2026-09-26; game build not rechecked):** A
second user-confirmed capture produced the motion image while the game
continued normally. Both camera samples passed validation at
`afterPresent=23202/23203`, with X/Y scales near `0.9742787/1.7320512`
and center depth near `2.705e-06`. The one-shot 5×5 motion grid had 25
finite samples: 18 exceeded 1 render pixel on at least one axis, 10
exceeded 10 pixels, X ranged from `-2.34961` to `83.9375`, and Y from
`-48.8125` to `9.17969`. Center motion was `(0.995117,-2.36914)` pixels.
These magnitudes explain why the 0.1-pixel preview mapping nearly saturates
large regions; they do not establish that motion direction or scale is
correct for every pixel. The F5 preview now uses a 10-pixel mapping so
variation across the measured range is visible.

**VERIFIED / MEASURED (Elite Odyssey, 2026-09-26; game build not
rechecked):** The user repeated the capture with the 10-pixel preview and
reported visibly more color variation and normal game operation. The log
shows two valid camera samples at `afterPresent=23540/23541`, a successful
motion pass and UI handoff, and restoration of the DSV observer. Its 5×5
grid contained 25 finite values; 12 exceeded 1 pixel and 10 exceeded 10
pixels. X ranged from `0` to `72.125`, Y from `-51.125` to `0`, and the
center was `(0.993164,-2.48242)` render pixels. The preview is now useful
for seeing scene structure, while camera/depth association for every draw
remains unverified.

**MEASURED OFFLINE CHECK (same capture):** Using the logged projection
columns divided by their logged X/Y scales as the current and previous
orientation, the logged row translations, raw center depth
`2.70442024e-06`, `depthB=0.0250000004`, and pixel center `(1280.5,720.5)`,
CPU reprojection gives approximately `(0.99522,-2.37110)` render pixels.
The GPU half-float sample was `(0.995117,-2.36914)`. This close agreement
verifies that the GPU pass follows the documented reconstruction math at
this pixel. The matrix rows are rounded in the log; the comparison does
not prove that Elite used this camera for every depth-writing draw.

## Limits

**VERIFIED / MEASURED (Elite Odyssey, 2026-09-26; game build not
rechecked):** With the additional GPU camera-buffer copy, the user again
reported a visible motion snapshot and normal game operation. Both camera
samples passed parsing at `afterPresent=18329/18330`. Center raw depth was
about `4.192e-11` in both frames, which corresponds to a very distant
view-axis point under the experimental `0.025/depth` formula. The 5×5
motion grid had 25 finite values, 14 over 1 pixel and 12 over 10 pixels;
X ranged from `-84.3125` to `0.00891113`, Y from `-13.7422` to `21.1094`.
This capture supports GPU-copy availability and continued rendering. It
does not validate motion on nearby world geometry or classify the distant
center pixel as sky.

The module runs in Elite only after an explicit experimental F5 request.
The F5 motion snapshot displays its GPU texture through a signed-color
preview. It has been seen in Elite, but the large saturated regions need
numeric and moving-scene validation. The camera is still a
bind-3 candidate, not the verified camera for every depth-writing draw.
**WARP-VERIFIED (2026-09-26):** A second motion shader reads the two retained
5376-byte camera constant buffers directly on the GPU. A synthetic buffer
with the observed Elite offsets produces the same motion at a tested pixel
as the CPU-parsed path within 0.01 render pixel. The one-shot Elite capture
now selects this shader when both GPU copies exist and otherwise uses the
previous CPU path. Elite output from this new path still needs a game check.

The diagnostic still reads back 5376-byte camera buffers to validate their
contents and a sparse output grid asynchronously. These readbacks are
one-shot diagnostics, not part of a production GPU-only camera path. Jitter
is excluded. The in-game depth/camera pairing and moving-cockpit measurements
are in
[camera-reconstruction.md](camera-reconstruction.md). Runtime integration
still needs verification of this GPU-camera path in Elite and moving-scene
validation before any upscaler receives motion.
