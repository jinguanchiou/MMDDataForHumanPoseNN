# Character Render Profiles — multi-game NPR rendering

Status/handoff reference for the per-character, per-game rendering system built on top of the
deferred renderer. Each game gets its own stylised shader; a character is routed to one by the
**game folder it lives in**.

Companion docs: `DATASET.md` (2D→3D pose data export), `endfield_style_d3d12_prompt.md`,
`wuwa_style_d3d12_prompt.md`, `data/絕區零/zzz_style_d3d12_prompt.md` (the per-game art specs).

---

## 1. Concept

`data/<game>/<character>/…pmx` — the **game folder decides the render method**:

| Game folder (keyword) | Profile | Shader | Method | Characters |
|---|---|---|---|---|
| `終末地`/`终末地`/`明日方舟`/`endfield` | `EndfieldPBR` | `shaders/Endfield.hlsl` | forward NPR+PBR (low contrast, hand-drawn feel) | 李织烟, 祀 |
| `鳴潮`/`鸣潮`/`wuwa`/`wuthering` | `WuwaPBR` | `shaders/Wuwa.hlsl` | forward PBR-based NPR (high contrast, full GGX, no self-shadow) | 达妮娅 |
| `絕區零`/`绝区零`/`zzz`/`zenless` | `ZzzNPR` | `shaders/Zzz.hlsl` | forward ramp + MatCap NPR (美漫, high sat, self-shadow) | 诺姆, 般岳, 铃 |
| anything else (`其他`, `星穹鐵道`, …) | `Cel` | deferred `Lighting.hlsl` `ShadeCel` | the original deferred cel path | LeMaline, 银狼 |

Both simplified and traditional Chinese keywords match. Mapping lives in
`dr::ProfileForPath()` (`src/Scene.cpp`) — add a game by adding one keyword line there.

---

## 2. Architecture

- `enum class RenderProfile { Cel, EndfieldPBR, WuwaPBR, ZzzNPR }` (`src/Scene.h`).
- Profile is set in `Renderer::LoadMmdModel` from `ProfileForPath(pmxPath)` (folder wins over the
  texture-based guess). Override at runtime: console `profile cel|endfield|wuwa` or GUI
  "Render profile" combo (`SetCharProfile`/`CharProfile`).
- **Dispatch** (`Renderer::Render`): `Cel` → deferred G-buffer + lighting pass. Everything else →
  a single **forward pass** drawn into `sceneHDR` after deferred lighting, depth-tested vs the
  scene depth. The forward pass selects the PSO trio by profile
  (`m_endfieldPSO`/`m_wuwaPSO`/`m_zzzPSO` + `…Outline…`/`…Blend…`).
- **Shared** across the three forward shaders: one root signature (`m_endfieldRS`) and three
  constant buffers — `PerFrame` (b0, camera/light/shadow), `EndfieldObjectCB` (b1, per-submesh),
  `EndfieldMaterialCB` (b2, look/knobs). Only the pixel shader differs. **All three HLSL object/
  material cbuffers MUST keep identical layout** to the C++ structs.
- Textures t0..t5: BaseColor, Normal, Packed(_P)/Toon(ZZZ), Mask(_M)/Sphere(ZZZ), Emissive,
  ShadowMap. Samplers s0 (linear) + s1 (PCF comparison). Built in
  `Renderer::CreateEndfieldPipeline` (all three PSO trios) with matching pipeline states.

### Per-submesh data (`Scene::Submesh`, filled in `BuildSceneFromMmd`)
- `srvHeapIndex` diffuse; `normalSrvIndex` (`_N`→`_HN` fallback); `srvPacked` (`_P`); `srvMask`
  (`_M`); `srvEmiss` (`_E`) — PBR-rip siblings found via `loadSibling` (searches same dir →
  `other tex/` → recursive).
- `srvToon` / `srvSphere` / `sphereMode` — MMD material `m_toonTexture` / `m_spTexture` (ZZZ ramp
  + MatCap).
- `matDiffuse` / `matAlpha` — PMX material colour + alpha. **Texture-less overlay meshes (eye/hair
  shadow) are dark diffuse + sub-1 alpha → alpha-blended** (3rd draw phase), NOT drawn opaque white.
- `matClass` (0 cloth, 1 skin, 2 hair, 3 eye, 4 metal) from `isSkin`/`isHair`/`isEye`/`isMetal`
  (texture-name match: Skin/Face, hair/发/髮, EyeA, Metal/金属). **`isEye` also matches by MATERIAL
  NAME** (眼/目/瞳/eye) — needed for rips like ZZZ 诺姆 whose eye parts all share one face texture
  (`脸.png`) and differ only by material name — while EXCLUDING the dark inked parts 睫/线/線/影/眉
  (lash / lid-line / eye-shadow overlay / brow), which must stay dark. Without this the eyeball is
  classed as cloth → the ramp shadow paints a dark band across the eye ("眼睛偏黑").

### Forward pass draws in 3 phases (`drawEndfield` lambda in `Render`)
1. Outline (back-face expansion, CULL_FRONT) — opaque submeshes only.
2. Opaque main (toon/PBR shading).
3. Blended overlays (`matAlpha<1`: eye/hair shadow) — SRC_ALPHA, depth no-write.
Debug channel views (`edbg`) skip phases 1 & 3.

---

## 3. The three shaders (what each does)

**Endfield** (`Endfield.hlsl`): normal-mapped cel diffuse (low contrast, cool shadow tint),
NPR-narrow ↔ GGX specular blended by metallic, hair angel-ring, fresnel rim, `_E` emissive.
**Receives** the directional shadow map (self-shadow). **`_P` packing CONFIRMED** (per-channel
analysis of the Arknights Endfield rip — NOT standard ORM): **R=Metallic, G=hair-spec/highlight
mask, B=AO, A=Roughness**. Metal/rough channels are GUI-selectable and auto-set per profile by
`ApplyProfileChannelDefaults()` (Endfield → metal=R/rough=A on load); **AO (B)** occludes the
ambient fill fully + direct diffuse ×0.6 (cavity depth); **hair highlight is masked by `_P.G`** so
the anisotropic sheen only appears on the marked strand highlights. Materials with **no `_P`** (face
/ skin / iris) fall back to the white 1×1 but are correctly gated (`hasPacked=0` → metal 0, ao 1);
the `edbg` channel views are likewise gated → a map-less material shows BLACK, not a misleading white.

**Wuwa** (`Wuwa.hlsl`): PBR-based NPR — 2-band toon diffuse (higher contrast), COLD blue-violet
shadow tint (warm-red for skin), **full Cook-Torrance GGX for all materials** (stylised
`pow(ggx,1.3)`), hair aniso, fresnel rim, +1.35 saturation. **No self-shadow** (per spec).

**Zzz** (`Zzz.hlsl`): ramp + MatCap — MMD toon ramp drives the shadow tone (`toon.Sample(float2(
toneU,0.5))`, fallback tint if none), **self-shadow** (PCF), clean colour blocks, metal = MatCap
(camera-basis UV, contrast-boosted, base-tinted) + sharp glint, +1.8 saturation, **thick
hand-drawn outline** (per-vertex `hash12` width jitter 0.6–1.35× + varied ink colour).
**Texture-colour fidelity** (`texFidelity`, default 0.9): the character is composited into the HDR
scene, then run through the shared post chain (bloom→×exposure→ACES→gamma→vibrance) tuned for
Sponza — which BRIGHTENS and DESATURATES it away from its painted albedo (the "發白/whitening").
The shader pre-inverts that chain (`ACESInv` + ÷exposure + ÷vibrance, LDR range only; HDR glints
keep their excess so specular still blooms; compensation clamped to cap near-white bloom) so the
COMPOSITED character lands back on its texture colour. The global `m_exposure`/`m_vibrance` are
handed to the shader through the (formerly pad) trailing `EndfieldObject` CB slots — ZZZ-only, so
Endfield/Wuwa keep their identical layout and ignore them.
**Colour grade** (applied BEFORE the fidelity inversion so it survives to display): `deepen` (mild
gamma darken, shadows more than highlights), `warmth` (general warm bias + a *targeted* yellow→
orange push — pulls green down only where `saturate(min(R,G)-B)` is high, so hair turns orange while
skin/whites/blue are spared), `eyeLift` — **clean-eye control**: the eyeball (`matClass==3`) is
rendered with NO self-shadow and lifted toward fully-lit (`lerp(litValue,1,0.5+0.5·eyeLift)`) so the
painted iris/sclera shows instead of a dark ramp band, AND the semi-transparent `目影` eye-shadow
overlay (`transparentMode`) fades out (`shAlpha=base.a·(1−eyeLift)`, so eyeLift 1 = overlay gone).
These three live in the
appended `EndfieldMaterial`/`ZzzMaterial` CB slots — again Endfield/Wuwa read only the prefix.

---

## 4. Controls

**Console:** `char list` / `char <index>` (hot-swap) · `profile cel|endfield|wuwa|zzz` · `edbg 0..8`
(debug channels: 0 shaded, 1 BaseColor, 2 Normal, 3–6 Packed/Toon.RGBA, 7 Mask/Sphere, 8 Emissive) ·
`zzz fidelity <0..1>` (texture-colour fidelity; 1 = undo post → exact texture) · `zzz sat <v>` ·
`zzz deepen <0..1>` (overall darken) · `zzz warmth <0..1>` (yellow→orange) · `zzz eye <0..1>` (lift eye shadow).

**GUI "Character" section** (shown when the loaded char is a forward-PBR profile):
Render profile combo · Endfield debug channel · Outline px / **Outline ref** (proportional
falloff) · Toon thresh/feather · **Metal (matcap)** + **Saturation** + **Texture fidelity** + **Deepen** + **Warmth** + **Eye shadow lift** (ZZZ only) · Normal detail +
**Flip N.Y** · Spec strength · Rough bias · Metal/Rough channel · Rim strength/power · Hair ring ·
Shadow recv / Shadow depth · Emissive.

**GUI "Post / Light":** **FXAA** toggle (default on) · **SSAA** slider (1.0–2.0, default 2.0).
The Cel/deferred look sliders (Saturation/Contrast/Outline-dark/SSS/Specular/Skin-highlight) are
**hidden for forward-PBR chars** (they only drive the deferred cel shader → dead for Endfield/Wuwa/ZZZ).

**GUI "Character" (forward-PBR only):** **Leather sheen** (0..2, Endfield — the broad wet latex
specular) · **Spec focus** (0..1, concentrate the specular highlight) · **Highlights** / **Shadows**
(−1..1, luminance-masked detail recover/lift applied **ON THE CHARACTER ONLY**, multiplicatively in the
forward shader so it survives the global tonemap — Sponza unaffected). These live in the appended
`EndfieldMaterial` CB row (`charShadows/charHighlights/specFocus/sheenStrength`).

**Leather / latex sheen** (`Endfield.hlsl`): the leather look is NOT a tight glint — it is a **broad
soft specular lobe + a Fresnel edge-glow** on a dark, smooth surface. `sheen = (broad·lit + graze·0.5)
· smoothness² · sheenStrength`, where `broad = pow(NdotH, lerp(4,40, smoothness))` (wide lobe),
`graze = pow(1−NdotV, 4)` (silhouette glow), `smoothness = 1−_P.A`. Gated by `hasPacked` and scaled by
**smoothness²**, so it hits only the low-roughness leather (bodysuit) and is OFF on matte skin / face
(no `_P`) — reproducing the game's "衣服高光足、臉不亮" from the roughness data, not a global brightness.

**Console:** `ssaa <1.0..2.0>` · `tone <shadows> <highlights>` (character only) · `specfocus <0..1>` ·
`sheen <0..2>`.

---

## 5. Anti-aliasing (SSAA + FXAA) + outline distance

- **SSAA (supersampling — the primary AA)** `m_ssaa` (default **2.0**): the whole internal pipeline
  (G-buffer, depth, sceneHDR, bloom, SSAO, Forward+ tiles) renders at `m_rw×m_rh = window×m_ssaa`
  (`UpdateRenderResolution`), and the tonemap box-downsamples SS→window (window viewport sampling the
  SS sceneHDR via the FST UV → bilinear = exact 2×2 average at 2×). `m_ldrRT` + back buffer stay at
  the window size. This is the real fix for the NPR **outline + toon-break shimmer / "馬賽克"** that
  FXAA (a post filter) can't stabilise. Change at runtime → `RequestSsaa` sets `m_pendingSsaa`, applied
  at the next frame top (GPU idle) which rebuilds the SS RTs. ~55–60 fps at 2× on an RTX 3060; drop to
  1.5/1.0 if fill-bound. The outline `screenSize` stays the WINDOW size so the line keeps a fixed px
  width through the downsample; the ZZZ toon break is additionally fwidth-AA'd (`aaW = max(toonFeather,
  fwidth(litValue)*0.75)`) and the outline width is now **uniform** (the old per-vertex hash jitter
  read as an uneven "有粗有細" line — removed).
- **FXAA** (`FxaaPS` in `PostProcess.hlsl`, `m_fxaaPSO`): tonemap → window LDR RT (`m_ldrRT`,
  RTV `kLdrRtvIndex`, SRV `kSrvLdr`) → FXAA → back buffer, when `m_fxaa`. Polishes any residual edges
  on the already-supersampled image. **Preserves the coverage alpha** so the dataset cut-out stays transparent.
- **Outline** is PROPORTIONAL: width scales with the character's on-screen height
  (`outlineScale = clamp(charHeightPx / (m_height * m_outlineRefFrac), 0, 1)`, computed CPU-side in
  `Render`), so a far/small character keeps a constant *relative* thickness and fully drops the
  outline when tiny (no thick/aliased far line).

---

## 6. Adding a new game

1. `src/Scene.h`: add an `enum class RenderProfile` value.
2. `src/Scene.cpp`: `RenderProfileName` case + `ProfileForPath` keyword(s).
3. `shaders/<Game>.hlsl`: copy `Wuwa.hlsl`/`Zzz.hlsl` (keep the b0/b1/b2 cbuffer layout + t0..t5),
   write the pixel shader; reuse `VSMain`/`VSOutline`/`PSOutline`.
4. `Renderer::CreateEndfieldPipeline`: build a PSO trio from `<Game>.hlsl` (mirror the Wuwa/Zzz
   blocks) + members `m_<game>PSO/Outline/Blend` + `Shutdown` resets.
5. Forward-pass dispatch in `Render`: add the profile → PSO selection.
6. If the game needs extra textures, add `Submesh` fields + load in `BuildSceneFromMmd` + bind in
   the forward draw loop (per-profile `t2/t3` binding pattern already exists).

---

## 7. Asset scanning (`RegisterCharacters`, `src/Main.cpp`)

- Handles nested `data/<game>/<character>/…pmx` (a folder with a PMX directly = character folder;
  a folder whose children hold PMX = game folder → recurse one level).
- Per character folder: register the LARGEST PMX (the body) + same-dir variants ≥35% of its size.
  **PMX <600KB or <35% of the body are skipped** (weapons/props — "主要角色的就好").
- Named by the character's own folder. Startup loads `CharacterPath(0)` if the hard-coded path is
  gone. Motions: every `*.vmd` in `motion/` (RegisterMotions).

---

## 8. Build / run / gotchas

- Build: `MSBuild DeferredRenderer.sln -m -p:Configuration={Debug|Release} -p:Platform=x64`.
  HLSL compiles at RUNTIME (D3DCompileFromFile) — shader errors show at launch, not build.
- **After any root-signature or shared-CB change, rebuild BOTH configs** — a stale binary + a new
  shader on disk = `E_INVALIDARG` at init (bit us twice).
- **`line` is a reserved HLSL keyword** — don't name a variable that.
- Missing `_N`/`_P`/`_M`/`_E` fall back to a WHITE 1×1 → per-submesh `hasNormal/hasPacked/
  hasEmissive` flags gate them (else metal reads 1 / white self-glow).
- The base renderer historically showed 3 pre-existing D3D12 debug-layer errors (708 ×3, 1003) on
  the deferred path — not from this work; scrub if the §7 "no ERRORs" bar matters.

### Pending / next milestones
- ZZZ toon-ramp UV orientation is a guess (`float2(toneU,0.5)`); MMD toons are sometimes vertical →
  may need `float2(0.5,1-toneU)` (check via `edbg 3`).
- Endfield `_P` channels CONFIRMED (R=metal, G=hair-spec, B=AO, A=rough) + wired. Wuwa `_P` packing
  still unconfirmed (kept metal=R/rough=G); load Wuwa `MC` masks (per-region metal/ID).
- Face SDF shadow, eye star highlight, planar floor reflection, comic post (halftone/speed-lines/
  LUT), TAA/SMAA — per the three art specs.
