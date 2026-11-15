# Phase 18 Research: NVIDIA DLSS Ray Reconstruction (DLSS-RR / `dlssd`)

**Date:** 2026-11-14  
**Scope:** Phase 18 ([PLAN.md](../PLAN.md) §44; [README.md](../README.md#roadmap); `AGENTS.md` primary planned technologies)  
**Prerequisites:** Phase 8/9 (NGX DLSS), Phase 2/3 (Cobra MRT G-Buffer census); optional Phase 15 (Streamline `sl.dlss_d`)  
**Status:** RESEARCH COMPLETE — **Conditional / Exploratory Scope** (critical engine-fit gaps identified; D3D11 NGX `dlssd` confirmed possible without D3D12 if pursued).

---

## 1. Nomenclature Clarification & Binary Rejection (`SDK-DOCUMENTED`)

1. **`AGENTS.md` / `README.md` Naming:** EDPE's roadmap lists *"NVIDIA DLSS-NR (Ray Reconstruction / Neural Denoising)"*.
2. **Official NVIDIA Feature Identity (`NVIDIA/DLSS` + `NVIDIA-RTX/Streamline` v2.14.16):**
   * Feature ID: **`NVSDK_NGX_Feature_RayReconstruction = 13`** (`include/nvsdk_ngx_defs.h`).
   * Binary: **`nvngx_dlssd.dll`** (NGX helper headers: `include/nvsdk_ngx_defs_dlssd.h`, `include/nvsdk_ngx_helpers_dlssd_d3d.h`; Streamline module: `sl.dlss_d.dll` / `sl_dlss_d.h`).
3. **Rule on Unofficial/Leaked Binaries:** Any non-SDK leaked binaries circulated online under names like `nvngx_dlssnr.dll` are **explicitly rejected** by EDPE (`AGENTS.md` Licensing & Security policy). Only official NVIDIA NGX SDK `nvngx_dlssd.dll` or Streamline `sl.dlss_d.dll` may be evaluated.

---

## 2. Crucial SDK Discovery: D3D11 Is Natively Supported (`SDK-DOCUMENTED`)

Before reading `NVIDIA/DLSS/include/nvsdk_ngx_helpers_dlssd_d3d.h` directly, it was widely assumed DLSS-RR required D3D12 or Streamline. **Direct inspection of lines 190–416 of `nvsdk_ngx_helpers_dlssd_d3d.h` disproves that assumption:**

* Official NGX defines **`NVSDK_NGX_D3D11_DLSSD_Create_Params`** and **`NVSDK_NGX_D3D11_DLSSD_Eval_Params`**, plus **`NGX_D3D11_CREATE_DLSSD_EXT`** and **`NGX_D3D11_EVALUATE_DLSSD_EXT`**!
* This means a feasibility probe for Phase 18 can run **directly inside EDPE's stable D3D11 `NgxContext`** (mirroring Phase 8 DLAA), **without** waiting for Phase 13 D3D12 interop — though Streamline `sl.dlss_d` remains available later for D3D12/FG paths (`sl_dlss_d.h`).

---

## 3. Exact DLSS-RR Input Contract vs. Cobra's Verified G-Buffer (`VERIFIED` + `SDK-DOCUMENTED`)

Inspecting `NVSDK_NGX_D3D11_DLSSD_Eval_Params` (`nvsdk_ngx_helpers_dlssd_d3d.h`) and Streamline's `ProgrammingGuideDLSS_RR.md` against EDPE's verified Cobra G-Buffer ([`docs/render-passes.md`](render-passes.md#L78-L81), [`docs/renderdoc-pipeline-analysis.md`](renderdoc-pipeline-analysis.md#L40-L43)):

| DLSS-RR Input Parameter (`NVSDK_NGX_D3D11_DLSSD_Eval_Params`) | Requirement | Cobra Availability (`VERIFIED` in RenderDoc) | Gap / Compatibility Assessment |
|:---|:---|:---|:---|
| `pInColor` / `pInOutput` | **Required** (noisy **pre-lighting/post-lighting HDR** before tonemap/HUD) | `s_clean_hdr_scene_texture` (`R11G11B10_FLOAT`) at Pass 34 exit **is** pre-tonemap, pre-HUD HDR! | **Available.** Must verify whether bloom (`Pass 36`) / auto-exposure runs before or after target capture. |
| `pInDepth` | **Required** (device depth) | `DSV #2` (`R32G8X24_TYPELESS` / `R32_FLOAT` SRV, reversed-Z $B=0.025$) | **Available & Verified.** |
| `pInMotionVectors` | **Required** (render-pixel displacement) | EDPE `MotionPass` (`R16G16_FLOAT`) | **Available & Verified.** |
| **`pInDiffuseAlbedo`** | **Required** (surface diffuse reflectance `[0,1]`) | **`MRT RTV0`** (`DXGI_FORMAT_R10G10B10A2_UNORM`) | **Available.** Need shader check for packed `.a` / metalness vs raw diffuse albedo (DLSS-RR expects diffuse albedo separable from metallic `F0` specular albedo!). |
| **`pInSpecularAlbedo`** | **Required** (Fresnel-modulated specular reflectance, e.g., `EnvBRDFApprox2`) | **Not stored directly in Cobra G-Buffer!** | **GAP:** Requires an extra EDPE compute pass synthesizing `EnvBRDFApprox2(F0, roughness, NoV)` from `RTV0` + `RTV2` + `RTV1` + `DSV #2` (matching `NVIDIA-RTX/Streamline` `ProgrammingGuideDLSS_RR.md` §3.4). |
| **`pInNormals`** | **Required** (world or view normals, explicit rough/smooth encoding flags) | **`MRT RTV1`** (`DXGI_FORMAT_R8G8B8A8_UNORM`, world normals) | **Partially Available.** Must verify exact packing (likely `N * 0.5 + 0.5` or octahedral/signed encoding) in Cobra's deferred geometry pixel shaders via DXBC inspection. |
| **`pInRoughness`** | **Required** (linear roughness `[0,1]` — **perceptual vs linear matters!**) | **`MRT RTV2`** (`DXGI_FORMAT_R8G8B8A8_UNORM`: roughness, metalness) | **Partially Available.** Must inspect which channel (`R` vs `G` vs `A`) stores roughness and whether it is $\alpha = r^2$ or $r$. |
| **`pInWorldToViewMatrix` & `pInViewToClipMatrix`** | **Required** for DLSS-RR (`NVSDK_NGX_D3D11_DLSSD_Eval_Params`) | Parsed every frame from 5376B `CB0` (`src/elite_camera.h`)! | **Available & Verified.** Convert EDPE row-major/right-handed camera matrices into exact column/row convention checked by NGX `DLSSD`. |
| `pInExposureTexture` / `PreExposure` | Strongly recommended (`IsHDR`) | Not currently extracted | Set `AutoExposure` flag or compute luminance reduction if HDR range varies wildly near stars/white dwarfs. |
| `pInSpecularHitDistance` / `pInSpecularMotionVectors` | **Optional** (dramatically improves moving glossy reflections) | **Zero** (`Cobra` is rasterized deferred + planar/cube probes, **NO DXR rays**) | **FUNDAMENTAL LIMITATION (see §4).** |
| `pInBiasCurrentColorMask` / `pInAlpha` | Optional (reactive/transparent glass) | Canopy glass hashes + `RTV3` emissive already isolated | Available for canopy glass / HUD-free masking. |

---

## 4. Technical Feasibility & Risk Analysis (`EXPERIMENTAL`)

### 4.1 What DLSS-RR Would Actually Receive in Elite Dangerous
Elite Dangerous Odyssey's 2D Cobra renderer is **purely rasterized D3D11** — it does **not** trace DXR stochastic shadow, GI, or glossy reflection rays that DLSS-RR was trained to denoise.

Specifically:
1. **Potential Upside:** DLSS-RR's Transformer/CNN architecture uses **explicit world normals (`RTV1`), roughness (`RTV2`), diffuse/specular albedo (`RTV0` + synthesized `EnvBRDFApprox2`), and camera matrices (`CB0`)** in addition to depth/MVs. In theory, supplying exact surface normals and view-dependent BRDF hints could reduce shimmering on fine metallic Coriolis station trusses, orbital rings, and rocky planetary normal maps compared to DLSS SR alone.
2. **Known Failure Risk (`SDK-DOCUMENTED` in `ProgrammingGuideDLSS_RR.md`):**
   * DLSS-RR assumes specular reflections come from noisy 1-spp raytraced inputs and relies on `SpecularHitDistance` / `SpecularMotionVectors` to avoid treating reflections on curved ship hulls or canopy glass as surface-locked diffuse paint.
   * When fed **rasterized screen-space reflections (SSR) or pre-convolved cubemap reflections without specular hit distances**, DLSS-RR can either **over-smooth fine brushed metal / film grain** ("plastic look") or **smear specular highlights along geometry motion vectors** worse than DLSS SR Transformer presets (`J`/`K`/`L`/`M`)!
3. **NVIDIA OTA & AppID Deployment Caveat (`SDK-DOCUMENTED` via NVIDIA Developer Forum `#388893`):**
   * Inspecting `nvsdk_ngx_defs.h` and NVIDIA developer guidance confirms that **`NVSDK_NGX_ENGINE_TYPE_CUSTOM` with an arbitrary ProjectID GUID does NOT receive OTA updates for `nvngx_dlssd.dll`** via the NVIDIA App / NGX CDN without an officially registered numeric NVIDIA Application ID!
   * Practically, testing DLSS-RR requires the user/developer to place an official SDK-redistributed `nvngx_dlssd.dll` alongside `dxgi.dll` (and `NvAppId` or explicit feature search path in `NVSDK_NGX_FeatureCommonInfo`), which EDPE can probe fail-open at runtime.

---

## 5. Recommended Phase 18 Verification Protocol (`EXPERIMENTAL`)

Before committing to full user-facing exposure (`NvidiaDlssRR` in `UpscalerBackend`), treat Phase 18 strictly as a **measurable 2-step R&D gate** (mirroring `PLAN.md` §44 *"Research"*):

### Step 18.1: G-Buffer Channel Verification (Read-Only DXBC + Snapshot Analysis)
* Using EDPE's existing shader probe (`dxbc_hash.cpp`, RenderDoc frame capture):
  1. Confirm exact channel assignment in **`RTV0` (`R10G10B10A2_UNORM`)**, **`RTV1` (`R8G8B8A8_UNORM`)**, and **`RTV2` (`R8G8B8A8_UNORM`)** at Pass 10 exit (`STATUS.md` lines 24–43).
  2. Determine world-normal encoding (range `[0,1]` vs signed/octahedral; `DLSSD` expects unit normals or specific `NVSDK_NGX_DLSS_Denoise_Mode_DLUnified` / `RoughnessMode_Packed`).
  3. Capture `RTV0..2` + `s_clean_hdr_scene_texture` into BMP/EXR snapshots (`F8` Dev mode) to verify no HUD/canopy glass contamination exists inside `RTV1`/`RTV2`.

### Step 18.2: Isolated `nvngx_dlssd.dll` Probe in `tools/ngx_probe/` & `NvidiaDlssRRBackend` (`DEV=1` Gated)
* Add a small D3D11 compute shader creating `R16G16B16A16_FLOAT` Diffuse Albedo, `SpecAlbedo` (`EnvBRDFApprox2`), `NormalRoughness`, and matrices from `CB0`.
* Compare **image quality + GPU frametime** in EDPE's automated comparison benchmark (`STATUS.md` line 49) across three scenes:
  * **Coriolis Station exterior** (dense metallic grates / specular reflections)
  * **Station hangar interior** (SSR floor + emissive lights)
  * **Odyssey rocky settlement** (high-frequency terrain normal maps + shadows)
* **Pass Criterion:** Accept DLSS-RR into main UI **only if** it measurably beats **DLSS SR Preset K/M** (`NvidiaUpscalerBackend`) on edge stability without smudging SSR floor reflections or inflating GPU cost above ~1.5 ms at 1440p; otherwise document the empirical result in `docs/` and keep DLSS SR Preset K–M as the recommended path (`PLAN.md` §52 *"never ship unverified quality claims"*).
