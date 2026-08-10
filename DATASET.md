# Dataset Generation — 2D anime image → 3D pose training data

This renderer can export a synthetic training set for a model that infers 3D motion/pose from a
single 2D anime image. Each **sample** is one rendered image plus a JSON annotation holding the
full 3D skeleton pose and the 2D projection of every joint for that view.

## What a sample contains

The pose and its projection depend on `(frame, view, background)` — the styles only re-shade the
same geometry — so **one annotation covers all the style images of a view** instead of every image
carrying an identical copy of the joint array:

```
f0000_v000_iso.json               <- pose + camera, shared
f0000_v000_iso_cel_full.png
f0000_v000_iso_char_depth.png
…                                 <- 11 style images, all described by that one annotation
manifest.jsonl                    <- one line per IMAGE, each with "ann": "f0000_v000_iso.json"
```

- **Image** — `f{frame}_v{view}_{iso|scene}_{style}.png` (square). Isolated captures are true RGBA
  cut-outs (transparent background); scene captures place the character in Sponza.
- **Annotation** — `f{frame}_v{view}_{iso|scene}.json`:
  - `images[]` — `{style, file}` for every image this pose describes.
  - `camera`: eye/target/up, `fov_y`, aspect, near/far, and the `view` + `proj` matrices
    (world↔image is fully recoverable).
  - `joints[]` — the canonical set in a **fixed order** (see below):
    - `name` (MMD bone name), `canonical` (standard joint name), `parent` (index into this array),
    - `world_pos` (global 3D position), `world_rot` (global quaternion),
    - `local_pos` / `local_rot` (**relative to the parent joint** — the parent-relative angle),
    - `px`, `py` — 2D projection into the image,
    - `in_frame` / `occluded` / `visible` — three booleans, because "off-screen" and "hidden behind
      something" are different cases for a loss. `in_frame` = projects inside the image;
      `occluded` = in frame but something is drawn in front of it (tested against the rendered depth
      buffer, so it catches both scene occlusion and self-occlusion such as the far-side shoulder in
      a side-on pose); `visible` = in frame and not occluded.
    - `depth` / `surface_depth` — the two numbers the occlusion test compared (distance along the
      camera axis, world units): the joint itself, and the nearest surface rendered at its pixel
      (`-1` = nothing drawn there). Lets the decision be audited from the annotation alone.
  - `canonical_in_frame` / `canonical_visible` / `visible_fraction` — the occlusion summary, so a set
    can be filtered without opening every joint list (`visible_fraction` is in the manifest as `vis`).
  - `char_depth_near` / `char_depth_far` — the scale of the `char_depth` style image (below).

### The joint set is fixed, the rig is not
Bone counts across this project's characters range from **48 to 1117**, with per-model naming, so raw
bone arrays are not comparable between characters. The export is therefore the canonical set in a
fixed order — body chain, head + eyes, then both hands finger by finger — with each `parent`
re-pointed at its nearest exported ancestor (twist and helper bones in between are dropped, the
hierarchy is preserved). Same joints, same order, same parents for every character.

| group | joints | coverage across the 47 registered characters |
| --- | --- | --- |
| core body (`pelvis`…`R_toe`) | 21 | **all of them** |
| fingers (5 per hand × 3 segments) | 30 | 47 of 48 rigs complete |
| eyes (`L_eye`, `R_eye`, `eyes_center`) | 3 | 45 of 48 |
| finger tips (`*_tip`) | 10 | only 13 of 48 — **do not rely on these** |
| `root`, `center`, `chest2` | 3 | `chest2` is rare (`上半身3`) |

A joint the rig does not have is simply **absent** from the array — never a zero-filled placeholder.
Check `[char] rig:` on load, or the `bones [filter]` console command, to see what a model carries.
`dataset joints all` switches the export back to every bone in the rig when you need the raw
skeleton; the two models whose rigs cannot produce pose data (a motorcycle prop, and a variant whose
arm chain is non-standard) are excluded from registration entirely.

### Occlusion — read this before using `bg scene`
`in_frame` only says the joint projects inside the image; it says nothing about whether something is
in front of it. Isolated captures come out near-fully visible (typically 0.7–0.95 of the canonical
joints, the remainder self-occluded). **Scene captures inside Sponza are usually the opposite**:
framing the whole body puts the camera ~470 units out, which is past the colonnade, so columns and
curtains end up between camera and character — measured `visible_fraction` there is frequently 0.
Filter on `visible_fraction` (e.g. keep ≥ 0.5), or train only on joints with `visible: 1`.

Output layout: `dataset/<character>/<motion>/…`.

### Canonical joint order (67 slots)
`root, center, pelvis, spine, chest, chest2, neck, head, L_eye, R_eye, eyes_center,
L_clavicle, L_shoulder, L_elbow, L_wrist, R_clavicle, R_shoulder, R_elbow, R_wrist,
L_hip, L_knee, L_ankle, L_toe, R_hip, R_knee, R_ankle, R_toe`, then per hand and per finger
(`thumb, index, middle, ring, pinky`) the segments `1, 2, 3, _tip` — e.g. `L_thumb1 … R_pinky_tip`.

Mapped from the standard MMD bone names (`下半身`, `左ひじ`, `左人指１`, `両目`, …); the numerals in
finger bones are full-width. SMPL's 24 body joints are covered except `spine2` (MMD has two spine
segments, `spine`/`chest`, plus `chest2` on some rigs) and `L/R_hand` (use the finger roots).

## Views
Hemisphere sampling: `azimuthCount` around × `elevCount` elevation rings from `elevMin`
(negative = 仰角 / from below) to `elevMax` (positive = 俯角 / from above), each framed to fit the
character. Plus optional **truncated crops** (`cropProb` fraction) that tightly frame a random
body part (upper body / face / arms / legs / torso) so limbs fall outside frame — joints outside
the image are marked `in_frame: 0`.

## Styles (render-pipeline diversity)
`cel_full, flat_albedo, shaded_nopost, normal, depth, outline, high_sat, low_key, rim_light,
random_light, char_depth`. Empty selection = all. The G-buffer `normal`/`depth`/`char_depth` styles
double as supervision.

### `char_depth` — depth you can actually train on
`depth` is `linear_depth / zFar`. With the character ~500 units out and `zFar` = 10000 that is the
bottom 5% of the range: two or three grey levels after 8-bit quantisation. `char_depth` instead
normalises to the character's own near..far span (measured from the posed geometry, not a bounding
box) and masks everything else out by G-buffer material id — so the background never enters, at any
distance. Each annotation carries the span, so the image decodes back to world units:

```
depth = char_depth_near + (1 - pixel/255) * (char_depth_far - char_depth_near)
```

Verified against the joint annotations in `depth_check/` — agreement is at the quantisation limit
(~0.8 world units per grey level) except for joints sitting exactly on a silhouette edge.

Buffer read-out styles (`depth`, `normal`, `char_depth`) skip FXAA, the facial decals and the light
markers: an edge-blurring filter on a depth or normal map invents values that were never in the
buffer, right where the silhouette is. Capture them at `ssaa 1` for the same reason — supersampling
resolves by averaging, which also blends across depth discontinuities.

## How to run

**GUI** — the "Dataset generation" panel (control window): set frames / azimuths / elevation /
image size / crop fraction / backgrounds / cut-out colour / style checkboxes, then **Generate
dataset**. Progress prints to the console.

**Console** —
```
dataset frames N            # time samples across the motion
dataset az N                # azimuths per ring
dataset elev N MIN MAX      # elevation rings + degree range (MIN<0 = from below)
dataset size N              # square image resolution
dataset crop P              # truncated-crop fraction (0 = off)
dataset margin F            # framing headroom (1 = tight)
dataset bg iso|scene|both   # background(s)
dataset name CHAR MOTION    # output labels (auto-filled from the loaded char/clip otherwise)
dataset out DIR             # output root (default: dataset)
dataset seed N              # PRNG seed (reproducible)
dataset gen                 # run it (also: the panel button)
dataset                     # print current config + sample estimate
```

## Inputs & hot-swapping
- **Characters**: every `*.pmx` under `data/` or `characters/` is registered — switch live via the
  GUI "Character" combo or `char <index>` / `char list`.
- **Motions**: every `*.vmd` in `motion/` is registered as a dance clip — switch via the GUI
  "Dance" combo or `motion <index>`. (If no startup motion is found, clip 0 auto-loads.)

Generation runs on the render thread and blocks the window while it works; watch the console for
`[dataset] done/total` progress.
