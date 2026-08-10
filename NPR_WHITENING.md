# NPR Character "發白 / whitening" — status & open problem

Handoff doc for the unresolved character-whitening issue (example model: **诺姆 / Norma**,
`char` index **8**, profile **ZzzNPR**, shader `DeferredRenderer/shaders/Zzz.hlsl`).

Status: **NOT SOLVED.** The 2026-07-28 shader-side tuning below reduced but did **not** fix the
whitewash — the user still reports the character washing out to white and colours reading pale.

---

## 1. Symptom
- Character overall looks **washed toward white** ("整體發白").
- Concrete example: 诺姆's stockings/thighs — the source albedo has a **faint low-saturation
  peach skin tone**; in-render that subtle tone is lost and the area reads near-white.
- Separately, saturated cloth can read over-vivid ("鮮豔").

## 2. Confirmed facts (verified this session)
- **Not a texture/loading bug.** `诺姆1.pmx` dumps fine; `Texture/体1.png` shows the leg/thigh
  skin is genuinely a pale, low-saturation peach in the source art. So the input is near-white to
  begin with — very easy to tip over to pure white.
- **Not a stale-shader bug.** `FindShaderFile` (Renderer.cpp ~L232) only ever resolves to the one
  source dir `DeferredRenderer/shaders/`; HLSL is compiled at runtime from there. Edits are live.
- Character forward pass writes into `m_sceneHDR` (RGBA16F) — Renderer.cpp ~L3325 (`rtvHdrE =
  kSceneHdrRtvIndex`). It is then subject to the **same global post as the Sponza scene**.

## 3. Root-cause hypothesis (why the shader-side fix was insufficient)
The whitening is dominated by the **global post chain**, applied to the character after it is drawn
into sceneHDR, not by the character shader alone:

1. **Bloom bright-pass** reads sceneHDR with `m_bloomThreshold = 1.0` (HDR-linear). Bright skin
   ≥1.0 blooms and spreads a white glow.
2. **Tonemap** (`PostProcess.hlsl : TonemapPS`): `col = (hdr + 0.6*bloom) * exposure` with
   `m_exposure = 1.2`, then **ACES** (Narkowicz), then gamma, then **vibrance ×1.25**.
   - Exposure 1.2 multiplies **after** any in-shader roll-off, so a shader cap at ~1.1 becomes ~1.3.
   - **ACES' shoulder compresses R/G/B toward each other near/above 1.0 → desaturation → pale/white.**
     A pale peach at albedo ~0.9 × 1.2 = 1.08 lands squarely in that shoulder and comes out
     near-white with its hue crushed. This is the core mechanism.

Net: any character region whose lit value approaches/exceeds ~0.8 (very easy for near-white skin)
gets pushed into the ACES shoulder and desaturated. The character effectively has **no headroom**
because it shares the Sponza HDR exposure, which is tuned for the dark stone scene.

## 4. What was already changed (this session, in `Zzz.hlsl` + wiring) — kept, but not enough
- Rim now **tinted by local albedo** (`lerp(baseLin, rimColor, 0.25)`) and ×0.4 on skin/cloth
  (was pure bluish-white `0.80,0.85,1.0` at strength `m_efRim=0.15`).
- Dropped the brightening `pow(col,0.95)`.
- Default `m_zzzSat` 1.35 → **1.15**; GUI "Saturation" slider min = 1.0 (=faithful). Fed to
  shader `satBoost`.
- Added a **soft highlight roll-off** (non-metal): `pk=max chan; if pk>0.85 → compress top of
  range, hue preserved`. Caps peak ≈1.1 — but exposure(1.2)+ACES still whiten past it.
- (Unrelated, same session) outline colour = deep tint of interior albedo; FXAA upgraded to full
  3.11 QUALITY + "AA strength" slider. Those are fine/independent.

## 5. Proposed next fixes (ranked) — NOT yet implemented
The character needs **headroom under the ACES shoulder**, or to bypass the scene tonemap.

- **(A) Exposure compensation inside the NPR shaders (cheapest, do first).**
  Multiply the final character colour by ~`1/exposure` (≈0.7–0.8) so `value*exposure` lands in the
  ~[0,0.6] **linear** region of ACES where hue is preserved. Pass `m_exposure` (or a dedicated
  `charExposureComp`) into the EndfieldObject/Material CB and divide at the end of PSMain. Tune so
  pale skin stops clipping. Risk: character may look slightly dim vs Sponza — expose a slider.

- **(B) Keep the character out of bloom.** Either raise the character's effective brightness below
  `m_bloomThreshold`, or exclude the character from the bright-pass. Cleanest exclusion: write a
  **coverage/flag into sceneHDR alpha** for character pixels and have `BrightPS` zero them (the
  isolated-cutout path already round-trips an HDR alpha, so the plumbing exists).

- **(C) Don't run ACES on the character — flat NPR tonemap instead.** Tag character pixels (stencil
  or sceneHDR alpha) and in `TonemapPS` branch to a gentler curve (e.g. Reinhard or straight
  gamma) for them. Most faithful to the "美漫平面 / flat comic" look, more plumbing.

- **(D) Lower global `m_exposure` toward ~0.9–1.0 and re-check Sponza.** Simplest knob but affects
  the whole scene; user hasn't complained about Sponza, so this is a fallback, not first choice.

Recommended order: **A → B → (C if still not flat enough)**. A alone likely resolves most of it.

## 6. Verification recipe (headless)
```
cd <repo root>
(echo "char 8"; sleep 10; echo "campos -640 175 38"; sleep 2; echo "shot NAME"; \
 sleep 3; echo quit) | ./build/x64/Release/DeferredRenderer.exe
# → screenshots/NAME.png ; legs close-up: campos -430 70 38
```
诺姆 = char index 8 (ZzzNPR). Camera default is at (-1500,230,38) looking +x toward the char at
(-160,0,38); dolly in along +x. `char list` prints indices. Live tuning: GUI "Saturation",
"Spec strength", "Rim" sliders (ZZZ block only shows when profile == Zzz NPR).

## 7. Scope note
The same over-bright→ACES-desaturation applies to **Endfield (李织烟)** and **Wuwa (鸣潮)** —
they share the sceneHDR + global post. Fix (A) should be applied to all three forward NPR shaders
(they share the EndfieldObject/Material CBs), not just Zzz.

## 8. Build / rebuild reminder
Rebuild **both** configs after any C++/CB change (root-sig + CB layout parity), then smoke-test:
```
MSBuild DeferredRenderer.sln -m -p:Configuration=Debug   -p:Platform=x64
MSBuild DeferredRenderer.sln -m -p:Configuration=Release -p:Platform=x64
```
HLSL is runtime-compiled, so shader-only edits need no rebuild — just relaunch.
