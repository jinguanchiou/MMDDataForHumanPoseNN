#pragma once
#include "Scene.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct ID3D12Device;
struct ID3D12CommandQueue;
namespace saba { class MMDModel; }

namespace dr {

// Smoke-test entry point for the Saba PMX/VMD integration. Loads the model (and an
// optional motion), prints geometry / skeleton / morph statistics, and returns true on
// success. Used by the `mmd` console command.
bool ProbeMmd(const std::wstring& pmxPath, const std::wstring& vmdPath = L"");

// The MMD animation worker silences stdout/stderr around Saba/Bullet steps via a PROCESS-WIDE
// fd redirect (_dup2 to NUL), so a main-thread printf landing in that window goes to NUL.
// Lock this recursive mutex while emitting intentional console output (e.g. command echoes)
// so it can never be swallowed by a concurrent silence on the worker thread.
std::recursive_mutex& ConsoleIoMutex();

// Owns the live Saba PMXModel + VMD animation for one character and drives it forward in
// time, producing CPU-skinned vertices each frame. The static GPU resources (index
// buffer, textures, submeshes) are built once via BuildSceneFromMmd; only the vertex
// positions/normals/uvs change per frame.
class MmdAnimator {
public:
    MmdAnimator();
    ~MmdAnimator();
    MmdAnimator(const MmdAnimator&) = delete;
    MmdAnimator& operator=(const MmdAnimator&) = delete;

    bool LoadModel(const std::wstring& pmxPath);   // Load + InitializeAnimation
    bool LoadMotion(const std::wstring& vmdPath);  // VMDAnimation::Create + Add + settle
    bool HasMotion() const;
    void ResetTime();

    // Optional VMD camera track (e.g. cam.vmd). Evaluate at an absolute time to get a
    // look-at camera in the model's own space (before the character's world transform).
    bool LoadCameraMotion(const std::wstring& vmdPath);
    bool HasCamera() const;
    void EvaluateCamera(double timeSec,
                        DirectX::XMFLOAT3& eye,
                        DirectX::XMFLOAT3& center,
                        DirectX::XMFLOAT3& up,
                        float& fovY);

    // Bullet cloth/hair physics — the dominant per-frame cost (drops a Debug dance to ~15fps).
    // Skeletal animation still runs when off; only the secondary jiggle is skipped.
    void SetPhysicsEnabled(bool on);
    bool IsPhysicsEnabled() const;

    // Advance the animation by dt seconds and run skinning (morph + node + physics).
    void Update(double dtSeconds);

    // Set the pose at an absolute time (seconds) while stepping physics by the real frame dt.
    // Used to slave the motion to an external master clock (the BGM) so they never drift.
    void UpdateTo(double absSeconds, double realDt);

    // Jump to an absolute time (seconds) and re-skin — for deterministic screenshots.
    void SeekTo(double seconds);

    // Facial-expression morphs (e.g. the icons in TEX_LML_Eff_Facial.png: closed eyes, ><,
    // spiral, heart…). A window forces morph `morphIdx` to `weight` while the looped motion
    // frame is in [start, end] — injected after the VMD evaluation, before vertices deform.
    struct MorphWindow { int start; int end; int morphIdx; float weight; };
    size_t      MorphCount() const;
    std::string MorphName(size_t i) const;
    int         FindMorph(const std::string& name) const;   // -1 if not found
    void        SetMorphWindows(const std::vector<MorphWindow>& windows);  // thread-safe
    // Always-on per-morph weight overrides (GUI sliders): weights[i] (>0) forces morph i.
    void        SetMorphWeights(const std::vector<float>& weights);         // thread-safe

    // Copies the current skinned vertices (Position/Normal/UV) into dst, which must hold
    // VertexCount() entries.
    void CopySkinnedVertices(Vertex* dst) const;
    size_t VertexCount() const;

    // ---- Skeleton pose extraction (dataset generation) ----
    // One bone/joint of the character at the current (already-stepped) animation frame. All
    // positions/rotations are in the model's own space — the SAME space the skinned vertices
    // come out in — so transforming globalPos by the character's world matrix lands exactly on
    // the rendered mesh. Local values are relative to the parent joint (derived from the true
    // final global transforms, so they include IK + physics, matching what is drawn).
    struct BonePose {
        std::string       name;       // MMD bone name (usually Japanese)
        int               parent;     // parent joint index into the same array, -1 if root
        DirectX::XMFLOAT3 globalPos;  // model-space world position of the joint
        DirectX::XMFLOAT4 globalRot;  // model-space orientation quaternion (x,y,z,w)
        DirectX::XMFLOAT3 localPos;   // translation relative to parent
        DirectX::XMFLOAT4 localRot;   // rotation relative to parent (x,y,z,w); axis-angle recoverable
    };
    size_t BoneCount() const;
    // Reads every bone's final transform for the current frame. Call after Update/SeekTo/UpdateTo
    // (i.e. after a Step) so the transforms reflect the frame being captured.
    void   ExtractPose(std::vector<BonePose>& out) const;
    // Model-space head-bone basis for the Endfield face SDF: outFwd = the bone's Z axis, outRight =
    // its X axis (raw, un-negated — the renderer applies the MMD -front/-right convention in world
    // space). Returns false if there's no head bone. Cheap: finds the 頭/head node, reads one matrix.
    bool   HeadBasis(DirectX::XMFLOAT3& outFwd, DirectX::XMFLOAT3& outRight) const;

    saba::MMDModel* Model() const;   // for one-time GPU resource building
    const std::vector<std::string>& MaterialNames() const;
    double AnimTime() const;
    double MotionDurationSeconds() const;   // VMD length (last keyframe / 30 fps); 0 if none

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Builds the static GPU resources for an already-loaded MMD model into a Scene (index
// buffer, per-material PNG textures, SRV heap, submeshes, model-space bounds, and an
// initial bind-pose vertex buffer). Placement lives in Scene.world, set by the renderer.
bool BuildSceneFromMmd(
    ID3D12Device*                    device,
    ID3D12CommandQueue*              queue,
    saba::MMDModel&                  model,
    const std::vector<std::string>&  materialNames,
    Scene&                           outScene);

} // namespace dr
