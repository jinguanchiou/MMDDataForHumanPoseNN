#pragma once
// Synthetic training-data generation: renders a loaded MMD character across many hemisphere
// views and render styles at sampled animation frames, and writes, per (frame, view, style,
// background) sample, an RGBA image plus a JSON annotation carrying the full 3D skeleton pose
// (world global position, parent-relative local position, parent-relative rotation) and the 2D
// projection of every joint into that view. Consumed by a 2D-anime-image -> 3D-pose model.
//
// This module is intentionally free of any D3D12 dependency: it defines the configuration, the
// canonical joint map, the body-part groups (for truncated crops), the style table, and the
// JSON serialization. The Renderer owns the orchestration (seek -> aim camera -> apply style ->
// render -> screenshot) and feeds plain data here to be written.
#include <DirectXMath.h>
#include <string>
#include <utility>
#include <vector>

namespace dr {

// --------------------------------------------------------------------------------------------
// Canonical joints. MMD skeletons carry 100+ bones with Japanese names; downstream training
// usually wants a compact, named human-keypoint set. We export EVERY bone, but also tag those
// that map to a canonical joint so the consumer can filter to a standard subset trivially.
// --------------------------------------------------------------------------------------------

// Canonical human-joint name for an MMD bone name, or "" if the bone is not a standard joint.
// Handles the common Japanese names (and a few English fallbacks).
std::string CanonicalJointName(const std::string& mmdBoneName);

// The canonical joints in a fixed order: body chain, head + eyes, then both hands finger by finger.
// Bone COUNT varies enormously between models (48..1117 across this project's assets), so this is
// the stable, model-independent skeleton — same length, same order, same parents for every rig.
const std::vector<std::string>& CanonicalJointOrder();

// Core = the body chain a pose dataset is useless without. The rest (eyes, gaze, third spine
// segment, fingers) are exported when the rig has them and simply absent when it does not.
bool IsCoreCanonicalJoint(const std::string& canonical);

// Body-part groups, used to frame truncated / cropped views (only some joints in frame).
enum class BodyPart : int { FullBody = 0, UpperBody, Face, Arms, Legs, Torso, Count };
const char* BodyPartName(BodyPart p);
// True if a canonical joint name belongs to the given part group (FullBody accepts all).
bool CanonicalInPart(const std::string& canonical, BodyPart p);

// --------------------------------------------------------------------------------------------
// Render styles. Each is applied to the renderer before a capture so the dataset spans many
// looks (the model must not overfit to one shading style). The Renderer maps each id to concrete
// pipeline/knob state in Renderer::ApplyDatasetStyle.
// --------------------------------------------------------------------------------------------
enum class StyleId : int {
    CelFull = 0,   // full anime cel + SSAO + bloom + ACES tonemap (closest to a finished frame)
    FlatAlbedo,    // albedo only, no shading (clean base look)
    ShadedNoPost,  // lit, but bloom/vibrance off (plainer render)
    NormalMap,     // G-buffer normal, remapped [-1,1]->[0,1] (also usable as supervision)
    Depth,         // G-buffer depth (also usable as supervision)
    Outline,       // heavy dark cel outline over flat fill (line-art feel)
    HighSat,       // cel with pushed saturation/contrast (poppy idol look)
    LowKey,        // cel, dim exposure + single moody light (dramatic)
    RimLight,      // cel with a strong rim/back light (backlit anime look)
    RandomLight,   // cel with a per-sample randomized light direction + colour tint
    CharDepth,     // depth of the CHARACTER only, normalised to its own near..far (real supervision)
    Count
};
const char* StyleName(StyleId s);

// --------------------------------------------------------------------------------------------
// Configuration (driven by the GUI panel / `dataset` console command).
// --------------------------------------------------------------------------------------------
struct DatasetConfig {
    std::wstring outDir    = L"dataset";
    std::string  character = "char";     // label for output paths + manifest
    std::string  motion    = "motion";

    // Time sampling: frameCount poses evenly across [startSec, endSec]. endSec < 0 => full motion.
    double startSec   = 0.0;
    double endSec     = -1.0;
    int    frameCount = 24;

    // Hemisphere view sampling: azimuthCount around x elevCount rings, elevation swept from
    // elevMinDeg (negative = 仰角 / from below) to elevMaxDeg (positive = 俯角 / from above).
    int   azimuthCount = 8;
    int   elevCount    = 3;
    float elevMinDeg   = -25.0f;
    float elevMaxDeg   = 75.0f;
    float fitMargin    = 1.25f;   // framing headroom (1 = tight fit of the body bbox)

    // Truncated / cropped views: a fraction of views tightly frame a random body-part subset so
    // limbs fall outside the frame (only some joints visible), mimicking cropped anime art.
    bool  enableCrops = true;
    float cropProb    = 0.35f;

    // Backgrounds: isolated = character on a transparent/solid background (clean 2D-pose targets);
    // scene = character inside the Sponza scene (real occlusion + lighting variety). Both -> each
    // view is captured twice.
    bool  bgIsolated = true;
    bool  bgScene    = true;

    std::vector<int> styles;   // StyleId ints to use; empty => all styles
    int      imgSize = 512;    // square render/output resolution
    unsigned seed    = 1234;   // PRNG seed (reproducible view jitter / crop choices / random light)

    // Joint export. Rigs range from 48 to 1117 bones with per-model naming, so the default is the
    // canonical set in CanonicalJointOrder() order: same joints, same order, same parents for every
    // character, and ~5% of the bytes. `dataset joints all` dumps every bone instead (finger
    // twist bones, physics chains, IK targets…) when you need the raw rig.
    bool canonicalJointsOnly = true;
};

// Total sample count for a config (for progress display / dry runs).
long long EstimateSampleCount(const DatasetConfig& c, double motionDurationSec);

// --------------------------------------------------------------------------------------------
// One joint's full annotation in a captured sample: 3D pose (world global + parent-relative
// local) and its 2D projection into the view.
// --------------------------------------------------------------------------------------------
struct JointRecord {
    std::string       name;         // MMD bone name (raw)
    std::string       canonical;    // canonical joint name, or "" if non-standard
    int               parent;       // parent joint index into this array, -1 if root
    DirectX::XMFLOAT3 worldPos;     // global position in world space
    DirectX::XMFLOAT4 worldRot;     // global orientation quaternion (x,y,z,w)
    DirectX::XMFLOAT3 localPos;     // position relative to parent
    DirectX::XMFLOAT4 localRot;     // rotation relative to parent (x,y,z,w)
    float             px, py;       // 2D pixel coordinate in the image
    int               inFrame;      // 1 if projected inside the image and in front of camera
    // 1 if the joint is actually SEEN at that pixel — tested against the rendered depth buffer, so
    // it catches both scene occlusion (a Sponza column/curtain between camera and character) and
    // self-occlusion (an arm behind the torso). 0 whenever inFrame is 0. Without this a scene
    // capture happily labels joints that the image does not show.
    int               visible;
    // The two numbers the visibility test compared, kept so the decision can be audited (and
    // plotted) from the annotation alone. Both are distances along the camera axis, in world units.
    float             viewDepth;      // the joint itself
    float             surfaceDepth;   // nearest surface rendered at its pixel; -1 = nothing there
};

// Reduces the full rig to the canonical joints, in CanonicalJointOrder() order, with each parent
// re-pointed at its nearest exported ancestor (the intermediate bones are gone, so raw parent
// indices would be meaningless). Joints the rig does not have are simply absent.
std::vector<JointRecord> CompactToCanonical(const std::vector<JointRecord>& all);

// Everything needed to serialize one sample's JSON annotation.
struct SampleMeta {
    std::string character;
    std::string motion;
    int    frameIndex = 0;
    double timeSec    = 0.0;
    int    viewIndex  = 0;
    float  azimuthDeg = 0.0f;
    float  elevDeg    = 0.0f;
    std::string style;         // StyleName — manifest only; one annotation covers every style
    std::string background;    // "isolated" | "scene"
    // The images this annotation describes: {style, file}. The pose and its projection depend on
    // (frame, view, background) only — the styles just re-shade the same geometry — so all of them
    // share one annotation instead of each carrying an identical copy of the joint array.
    std::vector<std::pair<std::string, std::string>> styleImages;
    std::string annFile;       // annotation filename, referenced from each manifest line
    std::string bodyPart;      // BodyPartName framed by this view ("FullBody" if not a crop)
    int    imgW = 0, imgH = 0;

    // Occlusion summary for the whole sample, so a training set can be filtered without reopening
    // every joint list: how many canonical joints are in frame, and how many of those are visible.
    int    canonicalInFrame = 0;
    int    canonicalVisible = 0;
    float  visibleFraction  = 0.0f;   // canonicalVisible / canonicalInFrame (0 when none in frame)

    // Decodes the char_depth style image back to metric depth along the camera axis:
    //   depth = charDepthNear + (1 - pixel/255) * (charDepthFar - charDepthNear)
    float  charDepthNear = 0.0f;
    float  charDepthFar  = 0.0f;

    // Camera (extrinsics + intrinsics) so world<->image is fully recoverable downstream.
    DirectX::XMFLOAT3 camEye{}, camTarget{}, camUp{};
    float  fovY = 0.0f, aspect = 1.0f, zNear = 0.0f, zFar = 0.0f;
    DirectX::XMFLOAT4X4 view{};       // world -> view
    DirectX::XMFLOAT4X4 proj{};       // view  -> clip
    std::string imageFile;            // relative path to the PNG this JSON annotates
};

// Writes the annotation JSON for one sample to `jsonPath`. Returns false on IO failure.
bool WriteSampleJson(const std::wstring& jsonPath,
                     const SampleMeta& meta,
                     const std::vector<JointRecord>& joints);

// Appends one line to the dataset manifest (JSONL): one compact object per sample. Thread-unused
// (single-threaded generation), but flushes each line so a crash mid-run keeps prior samples.
bool AppendManifestLine(const std::wstring& manifestPath, const SampleMeta& meta);

} // namespace dr
