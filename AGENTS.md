# AGENTS.md

## Project

This repository contains **EDPE — Elite Dangerous Performance Enhanced**.

EDPE is a rendering-performance enhancement project for the **2D renderer of Elite Dangerous**.

The project initially targets Direct3D 11 and adds temporal reconstruction technologies that the Cobra engine does not natively expose in this rendering path.

Primary planned technologies:

```text
NVIDIA DLAA
NVIDIA DLSS Super Resolution

AMD FSR Native AA
AMD FSR Super Resolution

Experimental later phases:
NVIDIA DLSS Frame Generation
AMD FSR Frame Generation
```

Read `PLAN.md` completely before making architectural changes.

`PLAN.md` is the authoritative roadmap.

---

# Project identity

Official project name:

```text
EDPE
Elite Dangerous Performance Enhanced
```

Use `EDPE` consistently in:

```text
source code
Dear ImGui
logs
configuration
documentation
version strings
release names
Git tags
diagnostics
```

Preferred project-owned filenames:

```text
EDPE.dll
edpe.ini
edpe.log
```

---

# Core architecture rule

Do not build independent duplicate pipelines for NVIDIA and AMD.

Elite-specific reconstruction belongs to EDPE.

Vendor-specific reconstruction/evaluation belongs behind backend interfaces.

The intended architecture is:

```text
Elite Dangerous / Cobra
        |
        v
EDPE renderer observation
        |
        +--> scene color
        +--> scene depth
        +--> camera/projection
        +--> frame state
        |
        v
EDPE temporal input generation
        |
        +--> projection jitter
        +--> motion vectors
        +--> reactive information
        +--> reset/history state
        |
        v
shared TemporalFrameInputs
        |
        +----------------------+
        |                      |
        v                      v
 NVIDIA backend            AMD backend
 DLAA / DLSS              FSR NativeAA / SR
        |                      |
        +----------+-----------+
                   |
                   v
          reconstructed world
                   |
                   v
             HUD/UI handling
                   |
                   v
        optional Frame Generation
        |                       |
        v                       v
 NVIDIA Streamline          AMD FidelityFX FG
        |                       |
        +-----------+-----------+
                    |
                    v
                  Present
```

Do not couple:

```text
Elite hooks
```

directly to:

```text
NGX
FidelityFX
Streamline
```

unless unavoidable at a very small boundary.

---

# Temporal input ownership

EDPE owns the common temporal inputs.

These should eventually be represented through a backend-neutral structure conceptually similar to:

```cpp
struct TemporalFrameInputs {
    TextureHandle color;
    TextureHandle depth;
    TextureHandle motionVectors;
    TextureHandle reactiveMask;

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

Exact resource types may initially remain D3D11-specific.

The architectural requirement is the separation, not this exact declaration.

---

# Upscaler abstraction

Use a backend abstraction conceptually equivalent to:

```cpp
enum class UpscalerBackend {
    None,
    NvidiaDlss,
    AmdFsr
};
```

The renderer-facing EDPE code should not need to know how NGX or FidelityFX performs reconstruction.

Backend implementations should translate EDPE inputs into their vendor API conventions.

---

# Frame Generation abstraction

Frame Generation is **not** part of the temporal upscaler module.

Keep it independent.

Conceptually:

```cpp
enum class FrameGenerationBackend {
    Off,
    NvidiaDlssG,
    AmdFsr
};
```

Do not hardwire combinations such as:

```text
DLSS -> NVIDIA FG
FSR  -> AMD FG
```

Architecture should permit cross-backend combinations if officially supported and technically correct.

Actual available combinations must be capability-driven.

Do not expose combinations known to be invalid.

---

# D3D11 and D3D12 boundary

Elite Dangerous is currently being targeted as a D3D11 application.

DLSS SR/DLAA can be integrated into this path.

Modern Frame Generation requires a substantially different presentation architecture.

Treat FG as a later experimental subsystem that may require:

```text
D3D11/D3D12 shared resources
shared NT handles
shared fences
D3D11On12 or equivalent interoperability
secondary D3D12 command queue
presentation/swapchain proxying
vendor FG presentation path
```

Do not contaminate the stable D3D11 SR implementation with premature FG complexity.

The expected order is:

```text
stable D3D11 injection
-> temporal inputs
-> DLAA
-> DLSS SR
-> FSR SR
-> HUD separation
-> D3D11/D3D12 interop
-> Frame Generation
```

---

# Required references

## EDVR

Primary technical reference:

```text
characterecho-sean/edvr-unofficial-patch
```

Important files:

```text
src/d3d11/dlaa.cpp
src/d3d11/dlaa.h

src/d3d11/temporal_pass.cpp
src/d3d11/temporal_pass.h

src/d3d11/native_temporal.cpp
src/d3d11/depth_probe.cpp

src/d3d11/fsr3_engine.cpp
src/d3d11/fsr3_engine.h

src/common/temporal_math.h
```

Important documentation:

```text
docs/anti-aliasing.md
docs/per-object-motion.md
docs/foveated-dlss-design-2026-09-14.md
docs/fsr-upscaler-design-2026-09-16.md
docs/kinematic-motion-injection-2026-09-19.md
docs/crisp-ui-handoff.md
```

Study EDVR for concepts including:

```text
depth discovery
projection jitter
camera/depth reprojection
motion vectors
reactive masks
temporal history
DLAA
DLSS
FSR
GPU timing
safe fallback behavior
resource lifetime
```

EDVR is a **reference implementation**, not EDPE's architecture.

Do not copy its complete VR stack.

Do not port:

```text
OpenVR runtime
OpenXR runtime
HMD compositor
VR eye submission
headset logic
VR foveation
```

unless a very small isolated component is directly reusable.

Do not reuse EDVR's NGX Project ID.

---

# NVIDIA references

Use current official NVIDIA documentation and SDK code as the final source of truth for NVIDIA behavior.

Relevant technologies include:

```text
NVIDIA NGX
DLSS Super Resolution
DLAA
Streamline
DLSS Frame Generation
NVIDIA Reflex
```

Do not assume an EDVR implementation detail is correct for EDPE's current SDK version.

Verify vendor API requirements independently.

---

# AMD references

Use current official AMD/GPUOpen documentation and SDK code as the final source of truth.

Relevant technologies include:

```text
AMD FidelityFX SDK
FidelityFX API
FSR temporal upscaling
FSR Native AA
FSR Frame Generation
current AMD latency technology recommended for FG
```

EDVR's D3D11 FSR implementation is useful evidence, but may include community D3D11 ports.

Determine code provenance and licensing before reusing anything.

---

# Dear ImGui

Dear ImGui is EDPE's primary runtime control surface.

Reference:

```text
ocornut/imgui
```

Initial backend:

```text
imgui_impl_win32
imgui_impl_dx11
```

Do not implement a separate launcher unless later requested.

The UI should eventually expose:

```text
EDPE version

Enable/disable

Upscaler:
Native
NVIDIA DLAA
NVIDIA DLSS
AMD FSR Native AA
AMD FSR

Quality:
Native AA
Quality
Balanced
Performance
Ultra Performance

NVIDIA preset:
Auto
J
K
L
M

Frame Generation:
Off
NVIDIA
AMD

Render resolution
Output resolution
Scale

Motion-vector mode
UI/HUD handling

Debug views

GPU timings

Rendered FPS
Presented FPS

backend capability/status information
```

Unsupported features must be greyed out with an explanation.

Default menu hotkey:

```text
Insert
```

When hidden, Dear ImGui must not interfere with game input.

---

# Git commits are mandatory

Create a Git commit after **every important, coherent change**.

This requirement applies throughout the entire project.

Examples requiring their own commit:

```text
bootstrap EDPE
configure build system
add D3D11 proxy
hook Present
initialize Dear ImGui
add resource census
identify scene depth
add depth viewer
capture camera matrices
add projection jitter
generate motion vectors
add temporal input abstraction
initialize NGX
add DLAA
add DLSS SR
add upscaler abstraction
add AMD FSR
separate HUD
add reactive masks
add GPU timings
prototype D3D11/D3D12 sharing
bootstrap Streamline
prototype NVIDIA FG
prototype AMD FG
fix major crash
fix major rendering bug
perform architectural refactor
document important verified reverse-engineering result
```

Normal cycle:

```text
inspect
-> implement
-> build
-> test
-> review diff
-> commit
-> continue
```

Do not move onto another major subsystem while important completed work remains uncommitted.

---

# Commit messages

Use concise descriptive messages.

Good:

```text
bootstrap EDPE project

hook DXGI Present

initialize Dear ImGui overlay

track D3D11 render targets

identify scene depth buffer

capture projection constants

add temporal projection jitter

generate camera-depth motion vectors

add temporal input abstraction

initialize NVIDIA NGX

add DLAA evaluation path

add DLSS Super Resolution

add upscaler backend interface

add AMD FSR backend

separate native-resolution HUD

add D3D11-D3D12 shared texture prototype

initialize NVIDIA Streamline

add AMD frame generation prototype
```

Avoid:

```text
update
changes
fix
stuff
wip
misc
```

Before committing:

```text
build affected targets
run relevant tests
review git diff
review git status
check for accidental generated files
```

Never commit:

```text
SDK credentials
secrets
personal machine paths
large temporary captures
build caches
unnecessary binaries
temporary texture dumps
```

---

# Versioning and Git tags

EDPE uses:

```text
vMAJOR.MINOR.PATCH
```

The repository is expected to begin without tags.

If no tags exist, the first stable EDPE milestone must be:

```text
v0.0.1
```

Use annotated tags:

```bash
git tag -a v0.0.1 -m "EDPE v0.0.1"
```

Rule:

```text
important coherent change -> commit

stable coherent milestone -> tag
```

Do not create a version tag for every commit.

During early development, prefer:

```text
v0.0.x
```

Possible larger milestones:

```text
first stable injector
working temporal input reconstruction
working DLAA
working DLSS SR
working FSR SR
multi-upscaler architecture
experimental D3D12 bridge
experimental Frame Generation
```

Exact version numbers should follow actual progress rather than a permanently fixed table.

Before tagging:

1. inspect existing tags;
2. ensure the new tag does not already exist;
3. ensure the working tree is clean;
4. ensure the intended commit is checked out;
5. build;
6. run relevant tests.

Never overwrite or force-move an existing release tag without explicit user approval.

---

# Central version source

Eventually define EDPE's version in one central location.

Do not scatter:

```text
"v0.0.8"
```

through many files.

Expose the central version through:

```text
Dear ImGui
logs
diagnostics
release metadata
```

---

# Reverse-engineering rules

Prefer graphics API observation over executable patching.

Preferred tools:

```text
resource tracking
shader hashes
constant-buffer inspection
draw classification
render-target tracking
depth-target tracking
viewport tracking
copy tracking
compute-shader analysis
```

Avoid hardcoded process addresses whenever a stable resource/signature approach exists.

If a process address or signature is unavoidable:

```text
isolate it
validate it
version-gate it
fail safely
document it
```

---

# Rendering-resource classification

Never modify resources purely because they have a certain resolution.

Forbidden logic:

```cpp
if (width == backbufferWidth &&
    height == backbufferHeight)
{
    modifyTexture();
}
```

Consider evidence including:

```text
format
bind flags
usage
sample count
creation order
draw frequency
depth association
viewport
shader hashes
copy relationships
frame position
resource lifetime
```

---

# Hooking rules

Keep hooks small.

Typical hook responsibility:

```text
observe call
update lightweight state
invoke isolated EDPE subsystem
call original function
```

Do not implement large rendering algorithms directly inside COM hook functions.

---

# Present

`IDXGISwapChain::Present` is the initial frame-boundary candidate.

Do not assume every operation belongs there.

If instrumentation proves that temporal reconstruction must occur earlier, use the correct point and document why.

---

# True Super Resolution requirement

Real DLSS/FSR performance improvement requires:

```text
scene render resolution < output resolution
```

Do not call this a successful Super Resolution implementation if Elite still performs its expensive scene render at native resolution and EDPE merely downsamples before reconstruction.

Investigate render scaling in this order:

1. existing Cobra scaling mechanism;
2. existing Elite graphics configuration;
3. selective D3D11 resource scaling.

Avoid global render-target rewriting.

---

# Depth

Never assume:

```text
standard Z
reversed Z
infinite projection
specific near plane
specific far plane
```

Measure them.

Verify:

```text
format
clear value
near/far
direction
sampling accessibility
correct frame association
```

before any temporal backend consumes the depth.

---

# Projection jitter

Jitter must:

```text
remain constant through one rendered frame
change only at the intended boundary
match the convention given to the selected upscaler
stand down when temporal reconstruction stands down
```

Never leave Elite rendering a jittered image if EDPE bypasses the reconstruction stage.

---

# Motion vectors

Start with camera + depth.

Suggested initial storage:

```text
DXGI_FORMAT_R16G16_FLOAT
```

Document and test:

```text
current -> previous or previous -> current
pixel or normalized units
render or output resolution
Y-axis orientation
jitter included or excluded
```

Do not change conventions by visual guessing.

Use deterministic tests.

Enhanced motion for:

```text
ships
station parts
terrain
particles
smoke
HUD
animated geometry
```

comes later.

---

# Upscaler backend validation

Each backend must explicitly translate EDPE temporal conventions into vendor conventions.

Do not assume DLSS and FSR expect identical:

```text
motion direction
jitter sign
depth convention
exposure convention
reactive-mask semantics
```

Validate each independently.

---

# DLAA first

Implementing NVIDIA DLAA before NVIDIA DLSS SR is encouraged because:

```text
input resolution == output resolution
```

This validates:

```text
depth
motion
jitter
history
NGX
```

without simultaneously debugging Cobra render scaling.

A vendor equivalent Native-AA validation step should also be used for AMD FSR where practical.

---

# FSR D3D11 implementation

Do not blindly select the first FSR implementation that compiles.

Investigate:

```text
EDVR/community D3D11 path
```

versus:

```text
current official AMD FidelityFX path
possibly through D3D11/D3D12 interop
```

Compare:

```text
image quality
GPU cost
CPU cost
copies
VRAM
maintenance
API currency
licensing
```

before choosing a production implementation.

---

# HUD/UI

For Super Resolution, native-resolution HUD separation is desirable.

For Frame Generation, HUD-less world access becomes significantly more important.

Architect toward:

```text
low-res world
-> temporal upscale
-> native-res HUD-less world
-> optional FG
-> UI composition
-> Present
```

Do not make HUD separation block the earliest DLAA/DLSS/FSR prototypes.

But do not postpone the architectural separation until after FG is implemented.

---

# Frame Generation

Frame Generation is experimental and later-stage.

Do not start it before:

```text
stable upscaling
validated temporal inputs
stable render scaling
stable HUD separation
D3D11/D3D12 interop measurements
frame-pacing diagnostics
```

FG must have its own backend interface.

---

# NVIDIA Frame Generation

Prefer official NVIDIA Streamline.

Do not implement DLSS-G using undocumented NGX behavior.

Research and correctly support requirements including:

```text
D3D12
Streamline
resource tags
motion
depth
camera state
HUD-less color
UI color
frame index
camera reset
Reflex
frame pacing
```

Capability detection must come from the vendor API.

Do not hardcode GPU generations or FG multipliers.

Future Multi Frame Generation support should also be capability-driven.

---

# AMD Frame Generation

Use the current official AMD FidelityFX Frame Generation path where practical.

Keep AMD FG independent of AMD FSR upscaling.

Research requirements including:

```text
D3D12 or Vulkan presentation path
depth
motion
camera
HUD-less world
UI
frame interpolation
swapchain integration
frame pacing
latency integration
```

Do not assume that enabling AMD FG requires AMD FSR SR.

---

# D3D11/D3D12 interop

Interop experiments must be isolated.

Before adopting an interop architecture, measure:

```text
copy count
GPU synchronization
CPU synchronization
GPU overhead
CPU overhead
VRAM
frame pacing
```

Prefer zero-copy shared resources.

If interop overhead eliminates the benefit, document the result before going further.

---

# Frame Generation metrics

When FG is active distinguish:

```text
simulation FPS
rendered FPS
presented FPS
generated frames
```

Never present generated FPS as equivalent to game simulation FPS.

Measure:

```text
real frame time
generated frame cost
presentation intervals
pacing stability
```

---

# Latency

Frame Generation requires latency analysis.

NVIDIA:

```text
Reflex
```

AMD:

```text
current vendor-recommended latency solution
```

Do not claim FG lowers latency merely because presented FPS increases.

---

# Diagnostics

Every subsystem must answer:

```text
Did it run?

Which backend?

Which resources?

What dimensions?

What formats?

What temporal conventions?

Why did it fail?

What did it cost?
```

Log relevant state including:

```text
EDPE version
GPU
adapter vendor
backbuffer
scene render size
output size
depth
motion format
upscaler backend
quality
frame generation backend
NGX state
FidelityFX state
Streamline state
history resets
resource recreations
interop state
fallback reasons
```

---

# GPU timing

Measure independently where possible:

```text
depth preparation
motion generation
temporal upscale
HUD composition
D3D11/D3D12 interop
Frame Generation
total EDPE GPU overhead
```

Use asynchronous timestamp queries.

Do not block the render thread to collect timings.

---

# Failure behavior

EDPE must fail open.

Upscaler failure:

```text
disable affected upscaler
disable jitter when necessary
restore original frame
Present normally
```

Frame Generation failure:

```text
disable FG
continue presenting real frames
leave working SR enabled
```

Never black-screen Elite because one EDPE subsystem fails.

---

# Performance

Forbidden in the normal hot path:

```text
blocking full-frame readback
Map() on full-resolution GPU textures
blocking GetData loops
per-frame shader compilation
per-frame texture creation
per-frame NGX feature recreation
unnecessary D3D11/D3D12 copies
```

Prefer:

```text
persistent GPU resources
resource pooling
compute passes
shared textures
shared fences
async timing
stable backend contexts
```

---

# Testing

Create CPU-side tests when practical for:

```text
jitter sequences
projection offsets
matrix math
depth linearization
motion-vector direction
motion-vector scale
quality calculations
resource classification
backend-independent state transitions
```

Create standalone graphics harnesses where practical for:

```text
NGX initialization
DLSS evaluation
FSR evaluation
motion textures
D3D11/D3D12 sharing
debug visualization
```

Do not make every test require launching Elite Dangerous.

---

# Documentation

Stable reverse-engineering results belong under:

```text
docs/
```

For each meaningful renderer finding document:

```text
date
game build if known
method
evidence
confidence
implications
```

Do not leave important facts only in terminal output or chat history.

Commit verified research when it materially changes the project understanding.

---

# Licensing

Before copying code from:

```text
EDVR
OptiScaler
community FSR D3D11 ports
NVIDIA samples
AMD samples
other projects
```

inspect the license.

Record provenance of adapted code.

Do not redistribute proprietary SDK material contrary to license terms.

---

# Build discipline

Prefer:

```text
CMake
MSVC
x64
C++20 where useful
HLSL
```

Do not add dependencies without justification.

Approved conceptual dependencies:

```text
Dear ImGui
NVIDIA SDK components needed for selected NVIDIA features
AMD FidelityFX components needed for selected AMD features
```

Before adding anything else document:

```text
why it is needed
license
runtime cost
binary-size effect
maintenance impact
```

---

# Code style

Prefer:

```text
small focused modules
RAII for COM objects where practical
explicit ownership
explicit formats
explicit resource state/conventions
enums instead of magic values
minimal global state
```

Matrix/reprojection code must document:

```text
row/column-major
handedness
clip-space convention
multiplication order
motion-vector direction
jitter convention
```

---

# Do not

Do not:

* implement gameplay cheats;
* automate gameplay;
* modify network behavior;
* bypass anti-cheat/security mechanisms;
* port EDVR wholesale;
* reuse EDVR's NGX Project ID;
* call a spatial upscale DLSS/FSR temporal SR;
* claim generated frames are simulated frames;
* begin FG before the foundational temporal path is stable;
* leave major completed changes uncommitted;
* overwrite release tags without explicit approval.

---

# Decision priority

When multiple approaches exist, prefer:

1. existing Elite/Cobra capability;
2. documented D3D11 behavior;
3. documented vendor API behavior;
4. stable graphics-resource interception;
5. isolated reverse engineering;
6. D3D11/D3D12 interop;
7. executable patching only as a last resort.

---

# Definition of success

EDPE's temporal reconstruction foundation succeeds when Elite Dangerous can render the expensive 3D scene below output resolution and reconstruct it using either supported vendor backend with:

```text
valid depth
valid motion
valid jitter
correct camera state
stable temporal history
usable UI
measurable performance gain
```

Frame Generation succeeds only when it additionally provides:

```text
correct HUD handling
stable frame pacing
safe fallback
measured latency impact
acceptable GPU overhead
correct capability detection
```