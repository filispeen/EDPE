# EDPE — Elite Dangerous Performance Enhanced

## 1. Goal

Build **EDPE (Elite Dangerous Performance Enhanced)**, a standalone performance-enhancement injector/mod for the **2D Direct3D 11 renderer of Elite Dangerous**.

The initial focus is temporal upscaling and anti-aliasing:

* NVIDIA DLSS Super Resolution;
* NVIDIA DLAA;
* AMD FSR temporal upscaling / Native AA;
* vendor-neutral reconstruction infrastructure.

The architecture must also leave a clean path for later experimental Frame Generation support:

* NVIDIA DLSS Frame Generation / future supported DLSS-G modes;
* AMD FSR Frame Generation.

EDPE must not depend on native DLSS/FSR support from the Cobra engine.

The injector must eventually:

* run inside `EliteDangerous64.exe`;
* intercept Direct3D 11 / DXGI at appropriate points;
* identify the rendered 3D scene;
* obtain scene color;
* obtain scene depth;
* obtain camera/projection information;
* generate temporal motion vectors;
* apply projection jitter;
* execute the selected temporal upscaler;
* reconstruct a native-resolution image from a lower-resolution scene;
* preserve HUD/UI at native resolution where feasible;
* expose configuration through **Dear ImGui**;
* provide GPU timing and debug views;
* support safe runtime switching;
* fail open and preserve the original game rendering path.

EDPE is a **rendering project only**.

Do not modify gameplay, networking or game logic.

---

# 2. Project identity

Official project name:

```text
EDPE
Elite Dangerous Performance Enhanced
```

Use `EDPE` consistently in:

```text
window titles
Dear ImGui
logs
configuration
documentation
build output
release names
version strings
Git tags
```

Recommended names:

```text
EDPE.dll
edpe.ini
edpe.log
```

---

# 3. Architectural principle

Do not build:

```text
DLSS pipeline
FSR pipeline
NVIDIA FG pipeline
AMD FG pipeline
```

as four mostly duplicated systems.

Instead use shared EDPE-produced temporal inputs:

```text
                       Elite Dangerous
                              |
                              v
                       EDPE renderer hooks
                              |
             +----------------+----------------+
             |                |                |
             v                v                v
           Color            Depth        Camera/Projection
                                              |
                                              v
                                      Projection jitter
                                              |
                                              v
                                      Motion generation
                                              |
                       +----------------------+
                       |
                       v
              TemporalFrameInputs
                       |
             +---------+---------+
             |                   |
             v                   v
        NVIDIA DLSS          AMD FSR
             |                   |
             +---------+---------+
                       |
                       v
              Native-res scene
                       |
                       v
                  UI/HUD stage
                       |
                       v
             Frame Generation layer
             |                   |
             v                   v
       NVIDIA DLSS-G         AMD FSR FG
             |                   |
             +---------+---------+
                       |
                       v
                    Present
```

Depth, motion, jitter and camera reconstruction belong to **EDPE**, not to an individual vendor backend.

---

# 4. Temporal upscaler abstraction

Design around a vendor-neutral interface.

Conceptually:

```cpp
enum class UpscalerBackend {
    None,
    NvidiaDlss,
    AmdFsr
};

enum class UpscaleMode {
    Native,
    NativeAA,
    Quality,
    Balanced,
    Performance,
    UltraPerformance
};

struct TemporalFrameInputs {
    ID3D11Texture2D* color;
    ID3D11Texture2D* depth;
    ID3D11Texture2D* motionVectors;
    ID3D11Texture2D* reactiveMask;

    uint32_t renderWidth;
    uint32_t renderHeight;

    uint32_t outputWidth;
    uint32_t outputHeight;

    float jitterX;
    float jitterY;

    float frameTimeMs;

    bool reset;
};
```

Exact APIs may change.

The important architectural rule is:

```text
Elite-specific reconstruction
        !=
vendor-specific upscaler code
```

---

# 5. Frame Generation abstraction

Frame Generation must be a **separate subsystem** from temporal upscaling.

Conceptually:

```cpp
enum class FrameGenerationBackend {
    Off,
    NvidiaDlssG,
    AmdFsr
};
```

Do not assume:

```text
DLSS SR -> NVIDIA FG only
FSR SR  -> AMD FG only
```

The architecture should permit supported combinations such as:

```text
DLSS SR + NVIDIA FG
DLSS SR + AMD FG

FSR SR + AMD FG
FSR SR + NVIDIA FG
```

only when the vendor API and generated inputs make the combination valid.

AMD explicitly separates FSR upscaling and Frame Generation, so cross-upscaler operation should remain architecturally possible.

Never expose an unsupported combination merely because the UI can represent it.

Capability detection decides what is actually available.

---

# 6. Important D3D11 / Frame Generation limitation

Elite Dangerous currently presents the target workload through Direct3D 11.

Modern Frame Generation paths cannot be treated as ordinary D3D11 post-process shaders.

Current vendor architecture must be treated as follows:

```text
EDPE base renderer:
Direct3D 11

DLSS SR / DLAA:
D3D11-capable temporal path

FSR temporal upscale:
implementation-dependent;
investigate D3D11 path and modern official FidelityFX path

NVIDIA DLSS Frame Generation:
requires Direct3D 12 integration

AMD FSR Frame Generation:
modern FidelityFX Frame Generation path uses
Direct3D 12 or Vulkan presentation infrastructure
```

Therefore:

**Do not make Frame Generation a requirement for the first EDPE releases.**

Frame Generation is a later research phase.

Likely architecture:

```text
Elite D3D11
     |
     v
EDPE D3D11 interception
     |
     +--> temporal reconstruction
     |
     v
D3D11 / D3D12 shared resources
     |
     v
D3D12 presentation / FG bridge
     |
     v
FG swapchain / presentation path
```

Possible technologies to investigate:

```text
D3D11On12
shared NT handles
D3D11_RESOURCE_MISC_SHARED_NTHANDLE
ID3D12Device::OpenSharedHandle
shared fences
wrapped resources
secondary D3D12 command queue
swapchain replacement/proxy
```

Do not implement this bridge until the ordinary D3D11 temporal-upscaling path is stable.

---

# 7. Primary reference — EDVR

Primary reverse-engineering/reference project:

```text
characterecho-sean/edvr-unofficial-patch
```

Do not port EDVR wholesale.

Study and selectively adapt/reimplement useful concepts.

## NVIDIA temporal path

Important files:

```text
src/d3d11/dlaa.cpp
src/d3d11/dlaa.h

src/d3d11/temporal_pass.cpp
src/d3d11/temporal_pass.h

src/d3d11/native_temporal.cpp
src/d3d11/depth_probe.cpp

src/common/temporal_math.h
```

Study:

```text
NGX initialization
DLSS/DLAA feature creation
NVSDK_NGX_D3D11_DLSS_Eval_Params
quality selection
render/output dimensions
MVLowRes
DepthInverted
jitter
depth capture
motion vectors
reactive masks
history reset
GPU timing
safe fallback
```

## AMD FSR path

Also study:

```text
src/d3d11/fsr3_engine.cpp
src/d3d11/fsr3_engine.h
docs/fsr-upscaler-design-2026-09-16.md
```

EDVR's FSR work is especially relevant because EDPE is also dealing with a legacy D3D11 renderer.

Determine precisely:

* which AMD code is official;
* which D3D11 portions originate from community ports;
* what OptiScaler/metarutaiga code is used;
* what license applies to each component;
* which assumptions EDVR made;
* which components are reusable;
* which should instead be rewritten.

Do not assume EDVR's D3D11 FSR integration is equivalent to AMD's current official FidelityFX API.

Validate behavior against AMD's official reference implementation.

## Other EDVR documentation

Study:

```text
docs/anti-aliasing.md
docs/per-object-motion.md
docs/foveated-dlss-design-2026-09-14.md
docs/fsr-upscaler-design-2026-09-16.md
docs/kinematic-motion-injection-2026-09-19.md
docs/crisp-ui-handoff.md
```

Do not reuse EDVR's NGX Project ID.

Do not port its:

```text
OpenVR runtime
OpenXR runtime
HMD submission
VR compositor
headset-specific code
VR foveation
```

unless a tiny isolated utility is directly relevant.

---

# 8. NVIDIA references

Use the current official NVIDIA:

```text
DLSS SDK / NGX
Streamline
DLSS Super Resolution documentation
DLSS Frame Generation documentation
Reflex documentation
```

as source of truth.

For DLSS SR / DLAA, investigate:

```cpp
NVSDK_NGX_D3D11_Init*
NVSDK_NGX_D3D11_GetCapabilityParameters
NVSDK_NGX_D3D11_AllocateParameters

NGX_DLSS_GET_OPTIMAL_SETTINGS

NGX_D3D11_CREATE_DLSS_EXT
NGX_D3D11_EVALUATE_DLSS_EXT

NVSDK_NGX_D3D11_DLSS_Create_Params
NVSDK_NGX_D3D11_DLSS_Eval_Params

NVSDK_NGX_D3D11_ReleaseFeature
NVSDK_NGX_D3D11_Shutdown*
```

Validate:

```text
Color
Depth
Motion vectors
Jitter
Render dimensions
Output dimensions
Frame time
Exposure
History reset
Reactive/bias mask
```

Do not reuse EDVR's Project ID.

---

# 9. NVIDIA Frame Generation

NVIDIA Frame Generation is a later optional EDPE feature.

Prefer the official **Streamline** integration path.

Do not attempt to fake DLSS-G by manually calling undocumented NGX entry points.

Research requirements including:

```text
Direct3D 12
Streamline interposer/integration
final color
HUD-less color
UI color
motion vectors
depth
camera matrices
frame index
camera reset state
dynamic-object information
resource lifetime
swapchain behavior
frame pacing
```

Also account for NVIDIA Reflex.

Treat correct Reflex/latency integration as part of the DLSS-G feature, not an unrelated optional checkbox.

DLSS-G should automatically stand down during:

```text
loading screens
menus where world rendering stops
video playback
major scene transitions
resolution changes
fullscreen/window-mode transitions
invalid temporal inputs
camera discontinuities
```

EDPE must query Streamline feature requirements rather than hardcoding GPU-generation support.

Future NVIDIA capabilities such as Multi Frame Generation should also be capability-driven.

Do not design EDPE around a fixed assumption that NVIDIA always inserts exactly one frame.

---

# 10. AMD references

Use current official:

```text
AMD GPUOpen
AMD FidelityFX SDK
AMD FSR SDK
FidelityFX API
FSR temporal upscaling documentation
FSR Frame Generation documentation
```

as source of truth.

FSR should be treated as two separate technologies:

```text
FSR Upscaling
FSR Frame Generation
```

Do not tightly couple them.

---

# 11. AMD FSR upscaling

EDPE should eventually offer:

```text
FSR Native AA
FSR Quality
FSR Balanced
FSR Performance
FSR Ultra Performance
```

where supported by the selected implementation.

FSR must consume the same EDPE temporal reconstruction data used for DLSS wherever conventions permit:

```text
Color
Depth
Motion vectors
Jitter
Render size
Output size
Frame delta
Reset
Reactive information
```

Build a dedicated AMD backend behind the generic upscaler interface.

Do not spread FidelityFX-specific structures throughout the Elite hooks.

---

# 12. FSR D3D11 strategy

Because EDPE targets a D3D11 game, FSR integration requires an explicit investigation phase.

Evaluate at least two approaches.

## Route A — proven D3D11 implementation

Study the D3D11 FSR implementation already exercised by EDVR and related open-source work.

Advantages:

```text
native D3D11 resources
low integration complexity
matches Elite's renderer
useful for proving the EDPE temporal inputs
```

Risks:

```text
community-maintained backend
version divergence from AMD reference
future compatibility
licensing/provenance complexity
```

## Route B — official current FidelityFX API

Investigate a D3D11 -> D3D12 interoperability layer that permits use of the current official FidelityFX backend.

Possible architecture:

```text
Elite D3D11 resources
        |
shared resources
        |
D3D12 EDPE device
        |
FidelityFX API
        |
D3D12 output
        |
shared back to D3D11
```

Advantages:

```text
current official AMD implementation
shared foundation with future AMD FG
better forward compatibility
```

Risks:

```text
interop complexity
resource synchronization
extra copies if zero-copy sharing fails
presentation complexity
```

Benchmark both before choosing the production path.

Do not choose based solely on implementation convenience.

---

# 13. AMD Frame Generation

AMD FSR Frame Generation is a later experimental phase.

Use the current official FidelityFX Frame Generation API where technically possible.

It requires more than simply calling the FSR upscaler.

Plan for:

```text
two sequential rendered frames
motion vectors
depth
camera information
optical flow / FG preparation
frame interpolation context
presentation scheduling
frame pacing
UI composition
```

AMD FG should remain compatible with another temporal upscaler if the API supports the combination.

Therefore keep:

```text
UpscalerBackend
```

and:

```text
FrameGenerationBackend
```

independent.

Investigate AMD Anti-Lag 2 or the current AMD-recommended latency solution when implementing FG.

Do not advertise FG as improving input latency.

Generated frames improve displayed motion smoothness but are not equivalent to newly simulated game frames.

---

# 14. Frame Generation common inputs

Build a vendor-neutral description of FG inputs.

Conceptually:

```cpp
struct FrameGenerationInputs {
    // Final reconstructed world without UI.
    TextureHandle hudlessColor;

    // UI layer if separated.
    TextureHandle uiColor;

    TextureHandle depth;
    TextureHandle motionVectors;

    CameraState camera;

    uint32_t width;
    uint32_t height;

    uint64_t frameIndex;

    float frameTimeMs;

    bool cameraReset;
};
```

The exact backend resource types may differ between D3D11/D3D12.

Do not let that leak into the high-level EDPE renderer state.

---

# 15. HUD-less rendering becomes mandatory for FG

For temporal upscaling, native HUD separation is an image-quality improvement.

For Frame Generation it becomes a much more important requirement.

Target pipeline:

```text
LOW-RES WORLD
      |
      v
TEMPORAL UPSCALER
      |
      v
NATIVE-RES HUD-LESS WORLD
      |
      +---------------------+
      |                     |
      v                     v
    FG INPUT             UI LAYER
      |
      v
GENERATED / REAL FRAME
      |
      v
UI COMPOSITION
      |
      v
PRESENT
```

Do not enable production FG until EDPE can correctly distinguish world content from the HUD or otherwise provide the vendor API's supported UI handling mode.

Investigate:

```text
HUD shader signatures
HUD render targets
HUD alpha
pre-UI color
post-UI color
UI callback/composition
reactive mask
```

---

# 16. Dear ImGui

Use **Dear ImGui** for EDPE configuration.

Reference:

```text
ocornut/imgui
```

Initial backends:

```text
imgui_impl_win32
imgui_impl_dx11
```

If EDPE later replaces/proxies the presentation path through D3D12 for FG, preserve the ImGui abstraction so its renderer backend can be changed without rewriting settings logic.

Suggested UI:

```text
EDPE — Elite Dangerous Performance Enhanced
Version: v0.x.x

Rendering
[ ] Enabled

Upscaler
[ Native           ]
[ NVIDIA DLAA      ]
[ NVIDIA DLSS      ]
[ AMD FSR NativeAA ]
[ AMD FSR          ]

Quality
[ Native AA         ]
[ Quality           ]
[ Balanced          ]
[ Performance       ]
[ Ultra Performance ]

NVIDIA DLSS preset
[ Auto | J | K | L | M ]

Frame Generation
[ Off               ]
[ NVIDIA DLSS-G     ]
[ AMD FSR FG        ]

Render:
1707 x 960

Output:
2560 x 1440

Render scale:
66.7%

Motion vectors:
[ Camera + Depth ]
[ Enhanced       ]

UI handling:
[ In temporal input ]
[ Native separate   ]

Debug
[ ] Depth
[ ] Motion vectors
[ ] Jitter
[ ] Reactive mask
[ ] HUD-less image
[ ] Final world
[ ] Generated frames
[ ] GPU timings
[ ] Frame pacing

Performance
Scene render:       xx.xx ms
Motion generation:   x.xx ms
Upscaler:            x.xx ms
Frame generation:    x.xx ms
EDPE total:          x.xx ms

Rendered FPS:       xx.x
Presented FPS:      xx.x
```

If Frame Generation is enabled, clearly distinguish:

```text
Rendered FPS
Presented FPS
```

Never mislabel generated frames as additional game simulation frames.

---

# 17. Project layout

Suggested architecture:

```text
/
├── AGENTS.md
├── PLAN.md
├── CMakeLists.txt
├── README.md
│
├── external/
│   ├── imgui/
│   ├── nvidia/
│   └── amd/
│
├── src/
│   ├── dllmain.cpp
│   │
│   ├── hook/
│   │   ├── d3d11_proxy.cpp
│   │   ├── dxgi_hook.cpp
│   │   ├── present_hook.cpp
│   │   ├── device_hook.cpp
│   │   └── wndproc_hook.cpp
│   │
│   ├── renderer/
│   │   ├── frame_context.cpp
│   │   ├── render_target_tracker.cpp
│   │   ├── depth_tracker.cpp
│   │   ├── camera_tracker.cpp
│   │   ├── projection_jitter.cpp
│   │   └── resolution_controller.cpp
│   │
│   ├── temporal/
│   │   ├── temporal_inputs.cpp
│   │   ├── motion_vectors.cpp
│   │   ├── reactive_mask.cpp
│   │   ├── history_state.cpp
│   │   └── temporal_math.cpp
│   │
│   ├── upscaler/
│   │   ├── upscaler.h
│   │   ├── upscaler_manager.cpp
│   │   │
│   │   ├── nvidia/
│   │   │   ├── ngx_context.cpp
│   │   │   ├── dlss.cpp
│   │   │   └── dlss_resources.cpp
│   │   │
│   │   └── amd/
│   │       ├── fsr.cpp
│   │       ├── fsr_context.cpp
│   │       └── fsr_resources.cpp
│   │
│   ├── framegen/
│   │   ├── framegen.h
│   │   ├── framegen_manager.cpp
│   │   │
│   │   ├── nvidia/
│   │   │   ├── streamline.cpp
│   │   │   ├── dlss_fg.cpp
│   │   │   └── reflex.cpp
│   │   │
│   │   └── amd/
│   │       ├── fsr_fg.cpp
│   │       └── anti_lag.cpp
│   │
│   ├── interop/
│   │   ├── d3d11_d3d12.cpp
│   │   ├── shared_texture.cpp
│   │   └── shared_fence.cpp
│   │
│   ├── ui/
│   │   ├── imgui_layer.cpp
│   │   ├── settings_window.cpp
│   │   └── debug_views.cpp
│   │
│   ├── diagnostics/
│   │   ├── logger.cpp
│   │   ├── gpu_timer.cpp
│   │   ├── texture_dump.cpp
│   │   └── frame_debug.cpp
│   │
│   └── config/
│       ├── config.cpp
│       └── config.h
│
└── shaders/
    ├── motion_vectors.hlsl
    ├── depth_prepare.hlsl
    ├── reactive_mask.hlsl
    └── debug_visualize.hlsl
```

Do not create this whole hierarchy immediately.

Create modules only when the implementation reaches them.

---

# 18. Git workflow

Git history is part of the project deliverable.

Create a commit after **every important coherent change**.

Examples:

```text
bootstrap EDPE
add D3D11 proxy
hook DXGI Present
initialize Dear ImGui
track render targets
identify scene depth
add depth visualization
capture projection
add jitter
generate motion vectors
add temporal input abstraction
initialize NGX
add DLAA
add DLSS SR
add upscaler abstraction
add FSR backend
add native HUD separation
add D3D11/D3D12 interop prototype
add Streamline bootstrap
add NVIDIA FG prototype
add FidelityFX FG prototype
add frame pacing diagnostics
```

Do not place several unrelated substantial changes in one commit.

Before committing:

```text
build
test
review diff
inspect git status
check accidental files
```

Use descriptive messages.

---

# 19. Versioning and tags

EDPE uses:

```text
vMAJOR.MINOR.PATCH
```

There are currently no tags.

The first version must start at:

```text
v0.0.1
```

Use annotated tags.

Example:

```bash
git tag -a v0.0.1 -m "EDPE v0.0.1"
```

Rule:

```text
important change -> commit

stable coherent milestone -> tag
```

Do not tag every commit.

Possible early evolution:

```text
v0.0.1
basic EDPE injection

v0.0.x
renderer reconnaissance / temporal infrastructure

v0.0.x
working DLAA

v0.0.x
working DLSS Super Resolution

v0.0.x
working AMD FSR upscaling

v0.1.0
stable multi-upscaler EDPE architecture

v0.2.x
experimental D3D11/D3D12 presentation bridge

v0.3.x
experimental Frame Generation
```

Exact mapping depends on actual progress.

Do not reserve version numbers too rigidly.

Before tagging:

```text
working tree clean
build succeeds
tests pass
tag does not exist
correct commit checked out
```

Never force-move an existing release tag without explicit approval.

---

# 20. Phase 0 — reconnaissance

Before implementing reconstruction:

1. inspect the workspace;
2. read `AGENTS.md`;
3. inspect build files;
4. inspect Git history;
5. inspect tags;
6. verify x64;
7. determine game D3D11/DXGI behavior;
8. identify swapchain;
9. identify HWND;
10. determine backbuffer format;
11. determine presentation mode;
12. determine relevant RT sizes;
13. enumerate depth targets;
14. determine command-list usage;
15. observe resource lifetimes.

Create:

```text
docs/render-pipeline-observations.md
```

Do not alter output yet.

---

# 21. Phase 1 — D3D11 injection

Implement:

```text
EDPE loading
D3D11 device discovery
immediate context discovery
swapchain discovery
Present hook
logging
Dear ImGui
```

No temporal reconstruction.

Success:

> Elite operates normally with EDPE loaded and ImGui can be toggled safely.

Create `v0.0.1` once the first coherent working milestone exists and no earlier EDPE tag exists.

---

# 22. Phase 2 — resource census

Instrument:

```cpp
CreateTexture2D
CreateRenderTargetView
CreateDepthStencilView
CreateShaderResourceView

OMSetRenderTargets
RSSetViewports

Draw*
Dispatch

CopyResource
CopySubresourceRegion

Present
```

Track:

```text
dimensions
format
bind flags
usage
sample count
draw frequency
depth association
viewport
shader association
frame timing
copy relationships
```

Do not classify resources solely by dimensions.

---

# 23. Phase 3 — scene depth

Identify the real scene depth.

Measure:

```text
format
clear value
standard/reversed Z
near plane
far plane
SRV capability
resource lifetime
safe sampling point
```

Add:

```text
Debug -> Depth
```

Do not feed unverified depth to either DLSS or FSR.

---

# 24. Phase 4 — camera and projection

Locate:

```text
view
projection
view-projection
camera position
near/far
previous/current camera
```

Prefer constant-buffer inspection.

Avoid executable patching when graphics-level identification is possible.

Document findings in:

```text
docs/camera-reconstruction.md
```

---

# 25. Phase 5 — projection jitter

Implement temporal projection jitter.

Initial sequence:

```text
Halton 2,3
```

unless testing justifies another sequence.

Requirements:

```text
one offset per rendered frame
same offset for all associated scene work
known pixel convention
same jitter passed to temporal upscaler
automatic disable when reconstruction fails
```

---

# 26. Phase 6 — motion vectors

Initial texture:

```text
DXGI_FORMAT_R16G16_FLOAT
```

Initial convention to verify:

```text
current -> previous
render-resolution pixels
```

Generate from:

```text
per-pixel depth
current camera
previous camera
projection
```

Add:

```text
Debug -> Motion vectors
```

Do not add object-motion hacks before camera+depth reprojection is verified.

Later investigate:

```text
ships
station parts
HUD
particles
terrain
smoke
animated geometry
```

using EDVR as a reference.

---

# 27. Phase 7 — shared temporal input layer

Before maintaining multiple upscalers, formalize EDPE-owned resources:

```text
temporal color
depth
motion
jitter
reactive mask
camera state
history reset
render dimensions
output dimensions
```

This is the handoff point used by both:

```text
NVIDIA
AMD
```

Commit this abstraction independently.

---

# 28. Phase 8 — NVIDIA DLAA

Implement NVIDIA first at native resolution.

```text
input == output
```

Validate:

```text
NGX initialization
depth
motion
jitter
history
reset
output
```

Test:

```text
cockpit
station
space
yaw
pitch
roll
fast rotation
HUD
moving ships
```

Do not proceed merely because NGX returned success.

---

# 29. Phase 9 — NVIDIA DLSS Super Resolution

Enable:

```text
input < output
```

Support:

```text
Quality
Balanced
Performance
Ultra Performance
```

Use NGX optimal settings when possible.

Do not fake reduced-resolution rendering by first rendering the game at native resolution and then shrinking the completed frame.

Real GPU savings require Cobra to render fewer scene pixels.

---

# 30. Phase 10 — true render-scale control

Investigate in this order:

## A. Cobra's own scaling mechanism

Preferred.

## B. Existing Elite configuration

Use if it provides the correct internal/output split.

## C. Selective render-target scaling

Only if required.

Never resize all backbuffer-sized textures blindly.

Swapchain should remain native.

---

# 31. Phase 11 — AMD FSR upscaling

Once EDPE's temporal inputs are independently validated, implement AMD FSR.

First objective:

```text
FSR Native AA
```

Then:

```text
FSR Quality
FSR Balanced
FSR Performance
FSR Ultra Performance
```

Compare using identical EDPE-produced:

```text
depth
motion
jitter
render size
output size
```

against DLSS where possible.

This phase must explicitly evaluate:

```text
EDVR-style D3D11 backend

vs.

modern official FidelityFX backend through D3D11/D3D12 interop
```

Record:

```text
image quality
GPU cost
CPU cost
memory
latency
complexity
compatibility
```

before deciding which is the production EDPE AMD path.

---

# 32. Phase 12 — native-resolution HUD

Investigate HUD separation.

Possible detection mechanisms:

```text
shader hashes
blend states
textures
render targets
constant buffers
draw ordering
```

Target:

```text
low-res world
     |
upscale
     |
native-res world
     |
native-res HUD
     |
Present
```

This phase significantly improves SR quality and is a prerequisite for production-quality Frame Generation.

---

# 33. Phase 13 — D3D11 / D3D12 interoperability research

Do not start FG before this point.

Build a contained experiment proving:

```text
D3D11 texture
     |
shared handle
     |
D3D12 access
     |
D3D12 compute
     |
shared result
     |
D3D11 access
```

Measure:

```text
synchronization cost
copy count
GPU time
CPU overhead
VRAM overhead
frame pacing
```

Zero-copy sharing is strongly preferred.

If interop cost removes the performance benefit, stop and document the result before attempting FG.

---

# 34. Phase 14 — AMD Frame Generation prototype

AMD FG is a reasonable first FG research target because FSR Frame Generation is intentionally separated from the upscaler.

Integrate:

```text
FidelityFX FG context
FG preparation
required camera information
motion/depth inputs
HUD-less color
UI composition
FG swapchain
frame pacing
```

Do not assume the first prototype is shippable.

Measure:

```text
generated FPS
rendered FPS
latency
GPU overhead
VRAM usage
pacing stability
UI artifacts
disocclusion artifacts
```

---

# 35. Phase 15 — NVIDIA Streamline / DLSS Frame Generation

Integrate Streamline only after D3D12 presentation/interop is understood.

First integrate Streamline **without FG enabled** and validate:

```text
manual hooking
resource tracking
feature requirements
resource tags
camera constants
frame indices
```

Then add NVIDIA Frame Generation.

Required inputs include:

```text
depth
motion vectors
HUD-less color
UI color
camera matrices
frame index
camera reset
```

Integrate NVIDIA Reflex correctly.

Do not ship NVIDIA FG with a broken or missing latency path.

Use capability queries to determine supported hardware/features.

Do not hardcode:

```text
RTX generation
FG multiplier
MFG multiplier
```

---

# 36. Frame pacing

Frame Generation requires its own diagnostics.

Measure and display:

```text
game simulation FPS
rendered FPS
presented FPS
generated frame count
CPU frame time
GPU frame time
FG GPU cost
present intervals
```

Do not calculate FPS purely from application `Present()` calls after FG is active.

A generated frame is not a new simulation frame.

---

# 37. Latency controls

FG must be paired with vendor-appropriate latency handling.

For NVIDIA investigate/integrate:

```text
Reflex
```

For AMD investigate:

```text
current recommended Anti-Lag / Anti-Lag 2 integration
```

Do not claim "lower latency" simply because presented FPS increased.

Dear ImGui should eventually show separate:

```text
Frame Generation
Low Latency
```

status fields.

---

# 38. Runtime compatibility matrix

EDPE should construct capability state dynamically.

Example:

```text
NVIDIA DLSS SR       Available
NVIDIA DLAA          Available
AMD FSR              Available
NVIDIA Frame Gen     Unavailable: D3D12 bridge disabled
AMD Frame Gen        Experimental
```

Use vendor API capability queries wherever possible.

Grey out unsupported options.

Explain the reason in Dear ImGui.

---

# 39. Diagnostics

Log:

```text
EDPE version
GPU
vendor
driver information
backbuffer
render resolution
output resolution
depth format
motion format
upscaler
quality
FG backend
NGX status
FidelityFX status
Streamline status
history reset
resource recreation
interop state
fallback reason
```

Measure separately:

```text
depth preparation
motion generation
DLSS/FSR upscale
HUD composition
D3D11/D3D12 interop
Frame Generation
EDPE total
```

Never block the render thread waiting for GPU queries.

---

# 40. Failure behavior

Every feature must fail open.

If an upscaler fails:

```text
disable upscaler
disable jitter if necessary
restore original rendering
Present original frame
```

If FG fails:

```text
disable FG
continue rendering real frames
```

An FG error must **not** disable otherwise-working DLSS/FSR.

Do not black-screen the game because a vendor SDK rejects an input.

---

# 41. Performance rules

Avoid:

```text
blocking GPU readback
Map() in normal full-frame path
blocking GetData
per-frame shader compilation
per-frame texture creation
per-frame feature recreation
unnecessary full-resolution copies
CPU copies between D3D11/D3D12
```

Prefer:

```text
GPU-only work
persistent resources
resource pooling
shared textures
shared fences
asynchronous timing
stable backend contexts
```

---

# 42. Scope exclusions for initial versions

Not initial-release requirements:

```text
Frame Generation
Multi Frame Generation
Ray Reconstruction
Ray Regeneration
Vulkan game renderer
gameplay modifications
network modifications
anti-cheat bypassing
```

Unlike the previous plan, **Frame Generation is no longer excluded from the overall EDPE roadmap**.

It is an explicitly planned experimental later phase.

---

# 43. Development philosophy

Every significant graphics claim must be labelled:

```text
VERIFIED
MEASURED
SDK-DOCUMENTED
EXPERIMENTAL
```

Workflow:

```text
observe
-> log
-> reproduce
-> validate
-> implement
-> test
-> commit
```

Never:

```text
guess
-> patch
-> hope
```

---

# 44. Initial serious milestone

First major temporal milestone:

```text
Elite Dangerous 2D

+ EDPE D3D11 injector
+ Dear ImGui
+ valid depth
+ valid camera/projection
+ jitter
+ camera/depth motion vectors
+ temporal input abstraction
+ NVIDIA DLAA
+ runtime bypass
```

---

# 45. Multi-upscaler milestone

Next major target:

```text
NVIDIA DLAA
NVIDIA DLSS SR
AMD FSR Native AA
AMD FSR SR

sharing the same:

depth
motion
jitter
camera
render-resolution controller
debug infrastructure
```

At this stage EDPE should no longer conceptually be a "DLSS injector".

It is a **vendor-neutral Elite Dangerous temporal reconstruction framework**.

---

# 46. Frame Generation milestone

Only after:

```text
stable SR
stable motion vectors
stable camera data
native HUD separation
D3D11/D3D12 interop
frame pacing diagnostics
```

attempt:

```text
AMD FSR Frame Generation
NVIDIA DLSS Frame Generation
```

Production readiness requires:

```text
correct UI
correct pacing
acceptable latency
no large presentation stalls
stable scene transitions
safe disable path
capability detection
```

---

# 47. Final target

A mature EDPE UI may look approximately like:

```text
EDPE — Elite Dangerous Performance Enhanced
v0.x.x

Rendering
Enabled:             Yes

Upscaler:            NVIDIA DLSS
Mode:                Quality
Output:              2560x1440
Render:              1707x960
Scale:               66.7%

Frame Generation:    AMD FSR FG
Low Latency:         Enabled

Motion:              Enhanced
HUD:                 Native

Performance

Game render:         13.4 ms
Motion:               0.18 ms
Upscale:              1.10 ms
Frame generation:     1.65 ms
EDPE overhead:        3.02 ms

Rendered FPS:        68
Presented FPS:       132
```

The final goal is not merely increasing the FPS counter.

EDPE should improve:

```text
rendering performance
image quality
frame stability
presentation smoothness
```

while keeping additional:

```text
latency
GPU overhead
VRAM use
artifacts
```

measurable and visible to the user.