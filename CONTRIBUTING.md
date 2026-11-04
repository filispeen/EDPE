# Contributing to EDPE

Thanks for helping with **EDPE (Elite Dangerous Performance Enhanced)**, a Direct3D 11 proxy that adds temporal reconstruction (TAA, DLAA/DLSS, FSR) to the Elite Dangerous 2D renderer.

## Before you start

1. Read `README.md` for scope and current status.
2. Read `AGENTS.md` for architecture rules and naming.
3. Read `PLAN.md` before any architectural change. It is the authoritative roadmap.
4. Check `STATUS.md` and open issues so you do not duplicate work.

For anything larger than a small fix, open an issue first and describe the approach.

## Scope

EDPE observes the graphics API. It does not modify game binaries, game logic, or network code. Pull requests that do any of that will be rejected.

## Development setup

Requirements:

- Windows 10/11 x64
- Visual Studio 2022 with the Desktop development with C++ workload (x64 MSVC, Windows SDK)
- CMake 3.25 or newer
- Git

Clone and fetch dependencies:

```powershell
git clone https://github.com/filispeen/EDPE.git
cd EDPE
git submodule update --init --recursive
```

Or run `install-build-requirements.ps1` to install the toolchain and initialize submodules.

Vendor SDKs (NVIDIA DLSS in `external/DLSS`, FidelityFX DX11) are not stored in the repository. See `.github/workflows/CI.yml` for how CI fetches them.

Build in an x64 Developer PowerShell:

```powershell
cmake -S . -B build -G "NMake Makefiles" "-DCMAKE_MAKE_PROGRAM=$((Get-Command nmake).Source)"
cmake --build build
```

Output: `build/Release/dxgi.dll`. Staged runtime files: `build/stage/`. Full details are in `BUILD.md`.

## Tests

```powershell
ctest --test-dir build --output-on-failure
```

All tests must pass before you open a pull request. The GPU tests need a D3D11 device. If you change behavior, add or update a test in `tests/`.

In the pull request, list what you ran and what you verified in game. If you could not test something (for example a specific GPU vendor), say so.

## Architecture rules

- Elite-specific reconstruction belongs to EDPE. Vendor-specific evaluation belongs behind backend interfaces.
- Do not build separate duplicate pipelines for NVIDIA and AMD.
- Do not couple Elite hooks directly to NGX, FidelityFX, or Streamline, except at a very small boundary.
- Temporal inputs (color, depth, motion vectors, jitter, reset state) are owned by EDPE and passed to backends through shared structures.
- Keep the render thread hot path cheap. Avoid per-frame allocations, logging, and stale-resource scans in `Map()`, `Unmap()`, and draw hooks.

## Naming

Use `EDPE` consistently in source, logs, config, docs, version strings, release names, and tags. Project-owned files are `edpe.ini` and `edpe.log`.

## Code style

- C++20, MSVC.
- Match the style of the surrounding code. Do not reformat files you are not changing.
- Do not add dependencies without discussing them in an issue first.
- Keep pull requests focused. One logical change per pull request.

## Commits and pull requests

1. Fork the repository and branch from `master`.
2. Write short, lowercase, imperative commit messages that say what changed and why, for example: `fix single-frame HUD drops by rearming draw hooks on HDR target bind`.
3. Do not edit `.github/workflows/CI.yml` without opening an issue first.
4. Do not commit game files, captures (`.rdc`), videos, logs, or SDK binaries.
5. Open the pull request against `master` and fill in:
   - what changed and why
   - how you tested it (tests, game scenario, GPU and driver)
   - linked issue, if any

## Reporting bugs

Include:

- EDPE version (shown in the overlay, `F5`)
- GPU, driver version, Windows version
- Game mode (Odyssey or Horizons, VR or flat, upscaler backend and quality)
- Steps to reproduce
- `edpe.log`, `edpe.ini` and `crash_*-*-*_*-*-*.log`(if you have one)

For rendering bugs, a short video or a RenderDoc capture helps a lot. Attach them to the issue instead of the repository.

## AI-assisted contributions

AI coders are allowed. `AGENTS.md` is written for them. You are responsible for every line you submit: review it, build it, run the tests, and document what you tested.

## License

EDPE is licensed under Apache License 2.0. By contributing, you agree that your contribution is licensed under the same terms.
