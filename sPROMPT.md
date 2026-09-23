You are working in the root of **EDPE — Elite Dangerous Performance Enhanced**.

EDPE is a Direct3D 11 performance-enhancement project for the **2D renderer of Elite Dangerous**, initially focused on adding NVIDIA DLSS Super Resolution and DLAA through an injected D3D11/DXGI rendering layer.

Read `AGENTS.md` and `PLAN.md` completely before doing anything else. Treat both files as authoritative project instructions.

## Project identity

Official project name:

```text
EDPE
Elite Dangerous Performance Enhanced
```

Use `EDPE` consistently in the codebase, runtime UI, logging, documentation, configuration, version information and release metadata.

Preferred filenames where appropriate:

```text
edpe.ini
edpe.log
```

## Goal

We want real NVIDIA DLSS Super Resolution in Elite Dangerous 2D.

This is not just an anti-aliasing mod.

The final pipeline should allow Elite to render the expensive 3D scene below native output resolution and then reconstruct it with DLSS:

```text
Elite scene at reduced resolution
    -> color
    -> scene depth
    -> generated motion vectors
    -> projection jitter
    -> NVIDIA NGX DLSS
    -> native-resolution output
    -> native-resolution HUD/UI where feasible
    -> IDXGISwapChain::Present
```

DLAA should also be supported as the 1:1 mode.

## Git workflow — mandatory

Git history must remain clean and useful throughout development.

After **every important, coherent change**:

1. review the change;
2. build the affected targets;
3. run relevant tests;
4. inspect `git diff`;
5. inspect `git status`;
6. create a Git commit before starting the next major change.

Examples requiring separate commits:

```text
initial EDPE scaffold
build-system setup
D3D11 proxy
DXGI Present hook
Dear ImGui initialization
Win32 input integration
render-target tracking
depth discovery
camera/projection discovery
projection jitter
motion-vector generation
motion-vector debug view
NGX initialization
DLAA
DLSS Super Resolution
render-scale control
HUD separation
reactive masks
GPU timing
important crash fix
important rendering fix
major refactor
verified renderer research documentation
```

Do not combine several unrelated important features into one giant commit.

Use concise descriptive messages such as:

```text
bootstrap EDPE project
hook DXGI Present
initialize Dear ImGui overlay
track D3D11 render targets
identify scene depth buffer
add reversed-Z depth visualization
capture projection constants
add temporal projection jitter
generate camera-depth motion vectors
initialize NVIDIA NGX
add DLAA evaluation
add DLSS quality selection
add render-scale controller
preserve native-resolution HUD
```

Avoid vague messages like:

```text
update
changes
fix stuff
wip
misc
```

Do not leave important completed work uncommitted while moving to another subsystem.

## Versioning — mandatory

EDPE uses Git tags for coherent stable milestones.

At the beginning of work, inspect existing tags.

The expected current condition is:

```text
no tags
```

If there are no tags, EDPE versioning begins at:

```text
v0.0.1
```

Use SemVer-style tags:

```text
vMAJOR.MINOR.PATCH
```

During early development, increment `v0.0.x` for meaningful milestones.

Possible progression:

```text
v0.0.1  first coherent working EDPE injection build
v0.0.2  Dear ImGui runtime controls
v0.0.3  renderer/resource diagnostics
v0.0.4  verified scene depth
v0.0.5  camera/projection capture
v0.0.6  temporal jitter
v0.0.7  camera+depth motion vectors
v0.0.8  NGX integration
v0.0.9  working DLAA
v0.0.10 working DLSS Super Resolution
```

This mapping is guidance only. Choose tag boundaries based on actual stable milestones.

Do NOT create a tag after every commit.

Rule:

```text
important change -> commit
stable coherent milestone -> version tag
```

Prefer annotated tags:

```bash
git tag -a v0.0.1 -m "EDPE v0.0.1"
```

Before tagging:

```text
inspect existing tags
ensure tag is unused
ensure working tree is clean
ensure correct commit is checked out
ensure project builds
run relevant tests
```

Never overwrite or force-move an existing version tag without explicit user approval.

When the first coherent EDPE milestone is working and no tags exist, create `v0.0.1`.

Eventually maintain the EDPE version in one central code location and expose it in:

```text
Dear ImGui
logs
diagnostics
release metadata
```

Do not scatter hardcoded version strings throughout the project.

## Runtime controls

Use **Dear ImGui** as the in-game configuration UI.

Reference:

```text
ocornut/imgui
```

Use the Win32 + DirectX 11 backends unless the current project already has an equivalent integration.

The UI must eventually allow changing:

```text
Enabled
DLAA
DLSS Quality
DLSS Balanced
DLSS Performance
DLSS Ultra Performance

DLSS render preset:
Auto
J
K
L
M

Sharpness
Render resolution
Output resolution
Motion-vector mode
UI handling mode

Depth debug view
Motion-vector debug view
Reactive-mask debug view
Jitter debug view
GPU timings
```

Use `Insert` as the default overlay toggle unless the project already defines another appropriate key.

The overlay title should use:

```text
EDPE — Elite Dangerous Performance Enhanced
```

and should eventually display the current EDPE version.

## Primary technical reference

Study:

```text
characterecho-sean/edvr-unofficial-patch
```

We are NOT porting the complete EDVR project.

We specifically want to understand and selectively adapt/reimplement the useful D3D11 temporal/DLSS pieces.

Inspect at least:

```text
src/d3d11/dlaa.cpp
src/d3d11/dlaa.h
src/d3d11/temporal_pass.cpp
src/d3d11/temporal_pass.h
src/d3d11/native_temporal.cpp
src/d3d11/depth_probe.cpp
src/common/temporal_math.h
```

and:

```text
docs/anti-aliasing.md
docs/per-object-motion.md
docs/foveated-dlss-design-2026-09-14.md
docs/fsr-upscaler-design-2026-09-16.md
docs/kinematic-motion-injection-2026-09-19.md
docs/crisp-ui-handoff.md
```

Particularly study how EDVR handles:

```text
NVIDIA NGX initialization
DLSS/DLAA creation
NVSDK_NGX_D3D11_DLSS_Eval_Params
DepthInverted
MVLowRes
projection jitter
render-size vs output-size
motion vector texture
camera/depth reprojection
reactive/bias-current-color mask
history reset
NGX errors
GPU timing
```

Do not reuse EDVR's NGX Project ID.

Do not bring over its OpenVR/OpenXR runtime, HMD compositor, headset logic or VR eye submission.

## NVIDIA reference

Use the current official NVIDIA DLSS/NGX SDK and documentation as the final source of truth.

Do not assume EDVR's behavior is automatically correct for the SDK version we use.

Verify the exact required meaning of:

```text
pInColor
pInDepth
pInMotionVectors
pInBiasCurrentColorMask

InJitterOffsetX
InJitterOffsetY

InMVScaleX
InMVScaleY

InRenderSubrectDimensions
InFrameTimeDeltaInMsec
InReset

DLSS feature creation flags
DLSS quality modes
optimal render-size queries
```

## Important requirement

We want **real upscaling**, not only DLAA.

The expensive scene must actually be rendered at a lower internal resolution.

Do not implement this:

```text
Elite renders 2560x1440
-> EDPE downsamples to 1707x960
-> DLSS back to 2560x1440
```

as the final solution.

That provides no meaningful scene-rendering performance saving.

Instead investigate whether Cobra already has a 2D supersampling/render-resolution scale.

Preferred order:

1. existing Elite/Cobra internal render-scale setting;
2. existing graphics configuration mechanism;
3. only then selected D3D11 render-target size interception.

The DXGI swapchain should remain at native output resolution.

## First task

Do NOT immediately start writing DLSS code.

Start with reconnaissance.

1. Inspect the entire current workspace.
2. Read all of `AGENTS.md` and `PLAN.md`.
3. Read any existing README and build files.
4. Inspect Git history.
5. Inspect all existing Git tags.
6. Determine what code already exists.
7. Determine the expected toolchain.
8. Identify whether a D3D11 proxy/hook already exists.
9. Inspect how the game-facing DLL is expected to be loaded.
10. Do not replace working infrastructure unnecessarily.

Then research the latest relevant code from the EDVR reference and official NVIDIA SDK.

Create or update:

```text
docs/render-pipeline-observations.md
```

with a concrete plan for the first vertical slice.

Commit meaningful reconnaissance/instrumentation work before moving on.

## First implementation milestone

The initial milestone should be only:

```text
EliteDangerous64.exe
    +
stable EDPE D3D11/DXGI injection
    +
IDXGISwapChain::Present hook
    +
D3D11 device/context discovery
    +
Dear ImGui overlay
    +
logging
```

No DLSS output modification yet.

The game must render normally.

Make separate commits for substantial steps instead of one giant Phase 1 commit.

When the first coherent working EDPE milestone is complete and there are still no version tags, create:

```text
v0.0.1
```

After that, proceed incrementally:

```text
resource census
-> scene depth
-> camera/projection
-> projection jitter
-> camera+depth motion vectors
-> NGX DLAA
-> real reduced-resolution rendering
-> DLSS Super Resolution
-> native HUD separation
```

Commit after each important step.

## Reverse engineering principles

Prefer D3D11 observation over executable patching.

Instrument where useful:

```cpp
ID3D11Device::CreateTexture2D
ID3D11Device::CreateRenderTargetView
ID3D11Device::CreateDepthStencilView
ID3D11Device::CreateShaderResourceView

ID3D11DeviceContext::OMSetRenderTargets
ID3D11DeviceContext::RSSetViewports

Draw
DrawIndexed
DrawInstanced
DrawIndexedInstanced

Dispatch

CopyResource
CopySubresourceRegion

IDXGISwapChain::Present
```

Collect enough information to distinguish resources by more than resolution.

Do not modify every display-sized texture.

## Depth

Find the real 3D scene depth target.

Determine:

```text
DXGI format
dimensions
bind flags
clear value
standard-Z vs reversed-Z
near plane
far plane
whether an SRV can be created directly
when it can safely be sampled
```

Add a Dear ImGui depth-debug visualization.

Do not feed depth into NGX until it has been independently verified.

Commit the verified depth path before proceeding to unrelated work.

## Camera/projection

Locate stable camera/projection data.

Prefer D3D11 constant-buffer inspection.

Document:

```text
matrix layout
matrix order
coordinate system
projection convention
near/far planes
camera transform
frame association
```

Avoid process-address hacks unless no stable graphics-level approach works.

Commit the verified implementation and corresponding documentation.

## Projection jitter

Implement subpixel temporal jitter only after the projection source is understood.

Use a deterministic low-discrepancy sequence initially.

The exact jitter supplied to the game must also be passed to NGX.

If DLSS/DLAA stands down, projection jitter must also stand down.

Commit jitter as a standalone important feature.

## Motion vectors

Create a render-resolution motion-vector texture, probably:

```text
DXGI_FORMAT_R16G16_FLOAT
```

Start with camera+depth reprojection.

For each current pixel:

```text
depth
-> reconstruct position
-> transform through current/previous camera state
-> project to previous frame
-> current-to-previous pixel displacement
```

Validate:

```text
sign
scale
units
jitter convention
coordinate orientation
```

before adding object-specific motion.

Use EDVR's `dlMv` implementation and temporal math as a reference.

Commit the verified camera+depth motion path before implementing enhanced/object motion.

## NGX vertical slice

Implement **DLAA first**.

That means:

```text
renderWidth  == outputWidth
renderHeight == outputHeight
```

DLAA lets us validate temporal inputs without also debugging internal-resolution scaling.

The first NGX success criterion is not simply "NGX returned success".

We need visually stable output under:

```text
station interior
cockpit
ship rotation
camera yaw
camera pitch
fast turn
station UI
HUD text
moving external ships
```

Commit NGX initialization and DLAA work at sensible coherent boundaries.

Only after DLAA is credible should we introduce smaller render dimensions.

## DLSS Super Resolution

When implementing true DLSS:

Use NGX's optimal-settings query where available.

Support:

```text
Quality
Balanced
Performance
Ultra Performance
```

Do not hardcode approximate ratios if NGX can give us valid render dimensions.

The Dear ImGui panel should clearly display:

```text
Output: 2560x1440
Render: 1707x960
Scale: 66.7%
```

Commit the internal render-scale mechanism separately from the DLSS integration when they are substantial independent changes.

## UI

Eventually prefer:

```text
low-res scene
-> DLSS
-> native-res HUD
-> Present
```

but do not make HUD separation a blocker for the first prototype.

The first prototype may reconstruct the complete finished frame.

Keep UI handling modular so native-resolution composition can be introduced later.

EDVR's HUD/depth/reactive-mask handling should be treated as a reference for solving temporal instability, not something to copy blindly.

## Performance

No blocking GPU readback in the normal frame path.

Avoid:

```text
Map on full-resolution GPU resources
blocking GetData loops
CPU copies of frames
per-frame shader compilation
per-frame texture recreation
per-frame NGX feature recreation
```

Use GPU-only compute where practical.

Add asynchronous D3D11 timestamp queries for:

```text
motion-vector generation
NGX DLSS
final composition
total EDPE GPU cost
```

## Failure behavior

All EDPE rendering features must fail open.

If any requirement cannot be satisfied:

```text
disable DLSS/DLAA
remove jitter
log exact reason
Present original Elite frame
```

Never black-screen the game because a research assumption failed.

## Documentation

As you discover facts about Elite's renderer, record them under `docs/`.

For each reverse-engineered finding record:

```text
date
game build/version if available
method
evidence
confidence
implications
```

Do not leave important discoveries only in terminal output.

Commit meaningful verified documentation updates.

## Working style

Work incrementally.

The normal rhythm should be:

```text
inspect
-> form hypothesis
-> instrument
-> verify
-> implement
-> build/test
-> review diff
-> commit
-> continue
```

At stable milestones:

```text
verify clean working tree
verify build/tests
inspect existing tags
create next annotated EDPE version tag
```

Before making a large change, explain what was discovered and why that change is the smallest next step.

Do not perform broad speculative rewrites.

When a hypothesis is unverified, label it as such.

When there are several possible injection points, instrument them first rather than choosing one by intuition.

Start now by inspecting the workspace, reading `AGENTS.md` and `PLAN.md`, checking Git history and tags, then researching the relevant current EDVR and NVIDIA implementations. After reconnaissance, summarize the actual current state of EDPE and begin the smallest safe Phase 1 implementation.