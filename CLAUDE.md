# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

NTUST CG **Assignment 1 — Deferred Renderer** (D3D12). Implement a Deferred Shading pipeline that loads the Sponza scene via Assimp and shades with Blinn-Phong. Authoritative spec: `Assignment1 Deferred Renderer.md` — read it whenever requirements are unclear; do not paraphrase from this file.

Repository state at hand-off: only the assignment markdown and the `sponza/` asset folder (`sponza.obj`, `sponza.mtl`, `.dds` textures, also `.tga` duplicates). There is no `.sln`, no source tree yet — the project is built from scratch.

## Operating directives (standing user requirements)

These five points govern every session on this project unless the user overrides them:

1. **Cover every test item in the spec.** Walk the requirements in `Assignment1 Deferred Renderer.md` end-to-end (Sections 3 + 7), don't pick a subset.
2. **VS2019 or VS2022 — your choice.** Generate a real `.sln` / `.vcxproj` for whichever you pick; don't change toolchain mid-stream.
3. **Run autonomously, no interruptions.** Don't pause for confirmation on routine implementation/build steps. Only stop for genuinely irreversible actions or true ambiguity in the spec.
4. **Self-grade against §7 of the spec.** When work reaches a runnable state, attempt to launch the binary, capture screenshots (G-buffer Depth / Normal / Albedo / final composite), and produce a § 7 rubric estimate (Correctness 40 / Pipeline 30 / Functionality 15 / Report 5, plus the 10 unallocated). The D3D12 Debug Layer rule under § 7.1 is strict: any ERROR-level message costs points — treat it as a build failure, not a warning.
5. **Console + window side-by-side; clean console; accept commands.** Build as a Console subsystem app (or `AllocConsole` from a Windows app) so a stdout console opens alongside the render window. The console must show **no unexpected warnings or errors** during normal operation, and must accept typed commands (e.g. toggle debug views, reload shaders, dump buffers, quit). The render-window `Z` key cycle from § 3.D is in addition to — not a replacement for — console commands.

## Hard constraints from the spec

Lifting the load-bearing rules so future sessions don't have to re-read § 2 / § 3 every time:

- **No third-party D3D12 framework.** No MiniEngine, no canned engines. Base only on the course samples or Microsoft's `D3D12HelloWorld` series.
- **G-Buffer formats are fixed:** Depth `D32_FLOAT`, Normal `R16G16B16A16_FLOAT`, Albedo `R8G8B8A8_UNORM`.
- **Lighting Pass uses a full-screen triangle**, not a quad.
- **Vertex layout:** Position + Normal + UV.
- **Directional light:** intensity `(1,1,1,1)`, direction-to-light `(-0.577, -0.577, -0.577, 1.0)` (already normalized — don't re-normalize and don't flip).
- **`Z` cycles views in this exact order:** Depth → Normal → Albedo → Color. Normal must be remapped `[-1,1] → [0,1]` for display only (storage stays signed).
- **Camera:** WASD move, LMB rotate, RMB drag-pan, first-person.
- **D3D12 Debug Layer must be silent of ERRORs at runtime** (warnings tolerated but worth scrubbing). Enable `ID3D12Debug` + `D3D12_MESSAGE_SEVERITY_ERROR` break-on for development.

## Build / run

Solution is `DeferredRenderer.sln` (VS2022, toolset v143, x64 only). Project lives in `DeferredRenderer/` with sources under `DeferredRenderer/src/`.

```
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" \
  DeferredRenderer.sln -m -p:Configuration=Debug -p:Platform=x64
```

Build outputs are redirected to `build/x64/{Debug,Release}/DeferredRenderer.exe` (so deleting `build/` cleans everything; `bin/`, `obj/`, `x64/` next to the project are not used). Console subsystem is on, so launching the exe gives both the console (stdout/stderr/stdin live) and a separate render window.

- Working directory at launch must be the repo root or wherever `sponza/sponza.obj` is reachable — Sponza references textures by relative path through the `.mtl`. Bootstrap doesn't load assets yet, so this only matters once Phase 1 is wired.
- Verified adapter on this machine: NVIDIA GeForce RTX 3060, feature level 11_0+ (debug layer + InfoQueue break-on-error wired in `Renderer::CreateDeviceResources`).
- Smoke test: `printf 'cycle\nview depth\nquit\n' | DeferredRenderer.exe` exits cleanly with no D3D12 ERRORs.

### Dependencies — vcpkg manifest mode

Manifest at `vcpkg.json` (solution root) declares `assimp`, `directxtk12`, `directx-headers`. Pinned to a public master baseline (the SHA in the bundled VS vcpkg-version is internal to MS and cannot be fetched from github upstream — don't reuse it).

**Two non-obvious gotchas burned into this setup:**

1. **The VS-bundled vcpkg (`<VS>\VC\vcpkg\vcpkg.exe`) is read-only / artifacts-mode.** It can consume binary-cache hits but never builds a port from source — failures look like `BUILD_FAILED` in 1.3 s with no stderr. Use the cloned `C:\dev\vcpkg\vcpkg.exe` instead.
2. **The project root contains Unicode (`工作\課`) and vcpkg post-build steps choke on it.** Compilation succeeds; the silent failure is in pkgconf/`.pc` fixup, which can't round-trip non-ASCII paths. Workaround: every vcpkg path (build trees, packages cache, **and install root**) must be on an ASCII-only path. Don't try to put `vcpkg_installed/` under the project.

Standard install command for this project:

```
"C:\dev\vcpkg\vcpkg.exe" install \
    --triplet=x64-windows \
    --x-manifest-root="<project>" \
    --x-install-root=C:\vcpkg-cache\installed \
    --x-buildtrees-root=C:\vcpkg-cache\bld \
    --x-packages-root=C:\vcpkg-cache\pkg \
    --disable-metrics
```

The vcxproj reflects this with `<VcpkgInstalledDir>C:\vcpkg-cache\installed\</VcpkgInstalledDir>`, `<VcpkgManifestInstall>false</VcpkgManifestInstall>` (we run install manually, MSBuild just consumes), and explicit `AdditionalIncludeDirectories` / `AdditionalLibraryDirectories` pointing at that install root.

## Submission hygiene (§ 6 of spec)

Before any "ready to submit" claim, ensure these are absent from the tree: `.vs/`, `bin/`, `obj/`, `x64/`, `Debug/`, `Release/`, and any `*.user`, `*.pdb`, `*.ilk`, `*.exe`. Source + `.sln`/`.vcxproj`/`.vcxproj.filters` + `.hlsl` + report PDF only.

## Suggested implementation order (from § 4 of spec)

Phase 1: Forward render Sponza with Blinn-Phong and textures correct.
Phase 2: Split into Geometry Pass writing G-buffer + Lighting Pass full-screen triangle reading G-buffer.

Don't skip Phase 1 — it isolates Assimp/DDS/resource bugs from deferred-pipeline bugs.
