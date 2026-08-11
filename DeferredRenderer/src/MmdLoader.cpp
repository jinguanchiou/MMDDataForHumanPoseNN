#include "MmdLoader.h"
#include "Common.h"

#include <d3d12.h>
#include <DirectXMath.h>

#include <directxtk12/WICTextureLoader.h>
#include <directxtk12/ResourceUploadBatch.h>

// Saba's headers trip /W4 (C4201 nameless struct, C4245 signed/unsigned). It's vendored
// third-party code, so silence its headers here without lowering our own warning level.
#pragma warning(push, 0)
#include <Saba/Base/UnicodeUtil.h>
#include <Saba/Model/MMD/PMXModel.h>
#include <Saba/Model/MMD/PMXFile.h>
#include <Saba/Model/MMD/VMDFile.h>
#include <Saba/Model/MMD/VMDAnimation.h>
#include <Saba/Model/MMD/VMDCameraAnimation.h>
#include <Saba/Model/MMD/MMDCamera.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#pragma warning(pop)

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <io.h>
#include <share.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace DirectX;
namespace fs = std::filesystem;

namespace dr {

// Shared with the console-command dispatcher (see ConsoleIoMutex in the header): held for the
// whole lifetime of every ScopedStdSilence so the fd-redirect below can't run concurrently
// with intentional console output. Recursive because a command handler (e.g. `mmd`) can be
// holding it and then enter a ScopedStdSilence on the same thread.
std::recursive_mutex& ConsoleIoMutex() {
    static std::recursive_mutex m;
    return m;
}

namespace {

// Redirects stdout+stderr to NUL for its lifetime. Used to swallow Saba/Bullet's
// chatty diagnostics (PMX warnings, "static-static collision") so the console stays
// clean per the project's console-hygiene requirement.
struct ScopedStdSilence {
    std::lock_guard<std::recursive_mutex> ioLock{ ConsoleIoMutex() };  // first member: locked before the swap, freed after restore
    int savedOut, savedErr, devNull;
    ScopedStdSilence() {
        std::fflush(stdout); std::fflush(stderr);
        savedOut = _dup(_fileno(stdout));
        savedErr = _dup(_fileno(stderr));
        devNull  = -1;
        if (_sopen_s(&devNull, "NUL", _O_WRONLY, _SH_DENYNO, 0) != 0) devNull = -1;
        if (devNull != -1) { _dup2(devNull, _fileno(stdout)); _dup2(devNull, _fileno(stderr)); }
    }
    ~ScopedStdSilence() {
        std::fflush(stdout); std::fflush(stderr);
        if (devNull != -1) {
            _dup2(savedOut, _fileno(stdout));
            _dup2(savedErr, _fileno(stderr));
            _close(devNull);
        }
        if (savedOut != -1) _close(savedOut);
        if (savedErr != -1) _close(savedErr);
    }
};

ComPtr<ID3D12Resource> CreateDefaultBuffer(ID3D12Device* device, UINT64 size) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = size;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> res;
    ThrowIfFailed(device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res)));
    return res;
}

ComPtr<ID3D12Resource> CreateWhite1x1(ID3D12Device* device, ResourceUploadBatch& upload) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = 1;
    td.Height           = 1;
    td.DepthOrArraySize = 1;
    td.MipLevels        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;

    ComPtr<ID3D12Resource> res;
    ThrowIfFailed(device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res)));
    NameObject(res.Get(), L"MmdFallbackWhite1x1");

    static const uint8_t kWhite[4] = { 255, 255, 255, 255 };
    D3D12_SUBRESOURCE_DATA sd{};
    sd.pData      = kWhite;
    sd.RowPitch   = 4;
    sd.SlicePitch = 4;
    upload.Upload(res.Get(), 0, &sd, 1);
    upload.Transition(res.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return res;
}

} // namespace

// ============================== MmdAnimator ==============================

struct MmdAnimator::Impl {
    std::shared_ptr<saba::PMXModel>           model;
    std::unique_ptr<saba::VMDAnimation>       anim;
    std::unique_ptr<saba::VMDCameraAnimation> camAnim;   // optional VMD camera track
    std::vector<std::string>                  matNames;
    double animTime  = 0.0;
    bool   hasMotion = false;
    bool   hasCamera = false;

    // Expression morph overrides (set by the renderer on the main thread, read by the worker
    // each update under this mutex). morphWindows = frame-ranged (expr command); morphWeights =
    // always-on per-morph (GUI sliders).
    std::vector<MorphWindow> morphWindows;
    std::vector<float>       morphWeights;
    std::mutex               morphMutex;

    // Bullet physics (cloth/hair jiggle) is the dominant per-frame animation cost — in a Debug
    // build it can drop the dance to ~15fps. Toggle it off for smooth playback; the skeletal
    // animation still runs, only the secondary physics motion is skipped.
    std::atomic<bool>        physicsEnabled{ true };

    // Faithful expansion of MMDModel::UpdateAllAnimation that injects the expression-morph
    // overrides AFTER the VMD sets the weights but BEFORE the morphs deform the vertices.
    void Step(float frame, float dt) {
        auto* m = model.get();
        m->BeginAnimation();
        if (anim) anim->Evaluate(frame);
        {
            std::lock_guard<std::mutex> lk(morphMutex);
            auto* mm = m->GetMorphManager();
            const int n = static_cast<int>(mm->GetMorphCount());
            const int f = static_cast<int>(frame);
            for (const auto& w : morphWindows)
                if (f >= w.start && f <= w.end && w.morphIdx >= 0 && w.morphIdx < n)
                    mm->GetMorph(static_cast<size_t>(w.morphIdx))->SetWeight(w.weight);
            // Always-on GUI overrides (applied after windows so they win). A weight < 0 means
            // "not controlled" (leave the VMD's value); >= 0 forces it — INCLUDING 0, so an
            // un-ticked morph is actively driven back off instead of sticking at its last value.
            const int wc = static_cast<int>(morphWeights.size());
            for (int i = 0; i < wc && i < n; ++i)
                if (morphWeights[i] >= 0.0f)
                    mm->GetMorph(static_cast<size_t>(i))->SetWeight(morphWeights[i]);
        }
        m->UpdateMorphAnimation();
        m->UpdateNodeAnimation(false);
        if (physicsEnabled.load()) {          // Bullet cloth/hair — the big Debug cost; optional
            m->UpdatePhysicsAnimation(dt);
            m->UpdateNodeAnimation(true);
        }
        m->EndAnimation();
        m->Update();
    }
};

MmdAnimator::MmdAnimator() : m_impl(std::make_unique<Impl>()) {}
MmdAnimator::~MmdAnimator() = default;

bool MmdAnimator::LoadModel(const std::wstring& pmxPath) {
    const std::string pmxU8 = saba::ToUtf8String(pmxPath);
    auto model = std::make_shared<saba::PMXModel>();
    bool ok;
    {
        ScopedStdSilence silence;  // mute Saba/Bullet load + physics-init chatter
        ok = model->Load(pmxU8, "");
        if (ok) model->InitializeAnimation();
    }
    if (!ok) {
        std::fprintf(stderr, "[MmdLoader] FAILED to load PMX: %s\n", pmxU8.c_str());
        return false;
    }
    m_impl->model = std::move(model);

    // Material names (Saba's MMDModel drops them) — needed to find the "Face" material so
    // its eyebrows can be drawn over the hair.
    saba::PMXFile pmx;
    if (saba::ReadPMXFile(&pmx, pmxU8.c_str())) {
        m_impl->matNames.clear();
        for (const auto& m : pmx.m_materials) m_impl->matNames.push_back(m.m_name);
    }
    return true;
}

bool MmdAnimator::LoadMotion(const std::wstring& vmdPath) {
    if (!m_impl->model) return false;
    const std::string vmdU8 = saba::ToUtf8String(vmdPath);

    saba::VMDFile vmd;
    if (!saba::ReadVMDFile(&vmd, vmdU8.c_str())) {
        std::fprintf(stderr, "[MmdLoader] FAILED to read VMD: %s\n", vmdU8.c_str());
        return false;
    }
    auto anim = std::make_unique<saba::VMDAnimation>();
    {
        ScopedStdSilence silence;
        if (!anim->Create(m_impl->model) || !anim->Add(vmd)) {
            std::fprintf(stderr, "[MmdLoader] FAILED to bind VMD to skeleton\n");
            return false;
        }
        anim->SyncPhysics(0.0f);
    }
    m_impl->anim      = std::move(anim);
    m_impl->animTime  = 0.0;
    m_impl->hasMotion = true;
    std::printf("[MmdLoader] motion bound: %zu bone / %zu morph keyframes, duration %.2f s\n",
                vmd.m_motions.size(), vmd.m_morphs.size(), MotionDurationSeconds());
    std::fflush(stdout);
    return true;
}

bool MmdAnimator::HasMotion() const { return m_impl->hasMotion; }
void MmdAnimator::ResetTime() { m_impl->animTime = 0.0; }
void MmdAnimator::SetPhysicsEnabled(bool on) { m_impl->physicsEnabled.store(on); }
bool MmdAnimator::IsPhysicsEnabled() const { return m_impl->physicsEnabled.load(); }

bool MmdAnimator::LoadCameraMotion(const std::wstring& vmdPath) {
    const std::string vmdU8 = saba::ToUtf8String(vmdPath);
    saba::VMDFile vmd;
    if (!saba::ReadVMDFile(&vmd, vmdU8.c_str())) {
        std::fprintf(stderr, "[MmdLoader] FAILED to read camera VMD: %s\n", vmdU8.c_str());
        return false;
    }
    if (vmd.m_cameras.empty()) {
        std::fprintf(stderr, "[MmdLoader] %s has no camera keyframes.\n", vmdU8.c_str());
        return false;
    }
    auto cam = std::make_unique<saba::VMDCameraAnimation>();
    if (!cam->Create(vmd)) {
        std::fprintf(stderr, "[MmdLoader] FAILED to create camera animation\n");
        return false;
    }
    m_impl->camAnim   = std::move(cam);
    m_impl->hasCamera = true;
    std::printf("[MmdLoader] camera motion bound: %zu camera keyframes\n", vmd.m_cameras.size());
    std::fflush(stdout);
    return true;
}

bool MmdAnimator::HasCamera() const { return m_impl->hasCamera; }

size_t MmdAnimator::MorphCount() const {
    return m_impl->model ? m_impl->model->GetMorphManager()->GetMorphCount() : 0;
}

std::string MmdAnimator::MorphName(size_t i) const {
    if (!m_impl->model) return {};
    auto* mm = m_impl->model->GetMorphManager();
    return (i < mm->GetMorphCount()) ? mm->GetMorph(i)->GetName() : std::string{};
}

int MmdAnimator::FindMorph(const std::string& name) const {
    if (!m_impl->model) return -1;
    const size_t idx = m_impl->model->GetMorphManager()->FindMorphIndex(name);
    // Saba returns its NPos sentinel (size_t max) when not found.
    return (idx == static_cast<size_t>(-1)) ? -1 : static_cast<int>(idx);
}

void MmdAnimator::SetMorphWindows(const std::vector<MorphWindow>& windows) {
    std::lock_guard<std::mutex> lk(m_impl->morphMutex);
    m_impl->morphWindows = windows;
}

void MmdAnimator::SetMorphWeights(const std::vector<float>& weights) {
    std::lock_guard<std::mutex> lk(m_impl->morphMutex);
    m_impl->morphWeights = weights;
}

void MmdAnimator::EvaluateCamera(double timeSec, XMFLOAT3& eye, XMFLOAT3& center,
                                 XMFLOAT3& up, float& fovY) {
    if (!m_impl->hasCamera) return;
    m_impl->camAnim->Evaluate(static_cast<float>(timeSec) * 30.0f);   // VMD is 30 fps
    const saba::MMDCamera& cam = m_impl->camAnim->GetCamera();
    saba::MMDLookAtCamera look(cam);
    eye    = { look.m_eye.x,    look.m_eye.y,    look.m_eye.z };
    center = { look.m_center.x, look.m_center.y, look.m_center.z };
    up     = { look.m_up.x,     look.m_up.y,     look.m_up.z };
    fovY   = cam.m_fov;   // radians
}
double MmdAnimator::AnimTime() const { return m_impl->animTime; }
double MmdAnimator::MotionDurationSeconds() const {
    return m_impl->anim ? static_cast<double>(m_impl->anim->GetMaxKeyTime()) / 30.0 : 0.0;
}
saba::MMDModel* MmdAnimator::Model() const { return m_impl->model.get(); }
const std::vector<std::string>& MmdAnimator::MaterialNames() const { return m_impl->matNames; }
size_t MmdAnimator::VertexCount() const {
    return m_impl->model ? m_impl->model->GetVertexCount() : 0;
}

void MmdAnimator::Update(double dtSeconds) {
    if (!m_impl->hasMotion || !m_impl->model) return;
    // Clamp dt so a hitch / first frame doesn't blow up the physics step.
    const float dt = static_cast<float>(std::min(std::max(dtSeconds, 0.0), 1.0 / 30.0));
    m_impl->animTime += dt;
    const float frame = static_cast<float>(m_impl->animTime) * 30.0f; // MMD is 30 fps

    ScopedStdSilence silence; // bullet may warn each physics step; keep the console clean
    m_impl->Step(frame, dt);
}

void MmdAnimator::UpdateTo(double absSeconds, double realDt) {
    if (!m_impl->hasMotion || !m_impl->model) return;
    m_impl->animTime = std::max(0.0, absSeconds);
    const float frame = static_cast<float>(m_impl->animTime) * 30.0f;   // pose from the master clock
    const float dt    = static_cast<float>(std::min(std::max(realDt, 0.0), 1.0 / 30.0)); // real-time physics
    ScopedStdSilence silence;
    m_impl->Step(frame, dt);
}

void MmdAnimator::SeekTo(double seconds) {
    // Require only a model, not a motion: Step() guards `if (anim)`, so with no VMD this still
    // runs morphs + physics + skinning to produce valid BIND-POSE vertices (needed by the dataset
    // capture, which streams these verts — otherwise GetUpdatePositions stays zero and the
    // character collapses to the origin).
    if (!m_impl->model) return;
    m_impl->animTime = std::max(0.0, seconds);
    const float frame = static_cast<float>(m_impl->animTime) * 30.0f;
    ScopedStdSilence silence;
    m_impl->Step(frame, 1.0f / 30.0f);
}

void MmdAnimator::CopySkinnedVertices(Vertex* dst) const {
    if (!m_impl->model) return;
    auto* model = m_impl->model.get();
    const size_t n        = model->GetVertexCount();
    const glm::vec3* pos  = model->GetUpdatePositions();
    const glm::vec3* nrm  = model->GetUpdateNormals();
    const glm::vec2* uv   = model->GetUpdateUVs();
    for (size_t i = 0; i < n; ++i) {
        dst[i].position = { pos[i].x, pos[i].y, pos[i].z };
        dst[i].normal   = { nrm[i].x, nrm[i].y, nrm[i].z };
        dst[i].uv       = { uv[i].x,  1.0f - uv[i].y };  // undo Saba's OpenGL V-flip (see BuildSceneFromMmd)
    }
}

size_t MmdAnimator::BoneCount() const {
    if (!m_impl->model) return 0;
    return m_impl->model->GetNodeManager()->GetNodeCount();
}

void MmdAnimator::ExtractPose(std::vector<BonePose>& out) const {
    out.clear();
    if (!m_impl->model) return;
    auto* mgr = m_impl->model->GetNodeManager();
    const size_t n = mgr->GetNodeCount();
    out.reserve(n);

    // Map node pointer -> manager index so parent links reference the same array we emit
    // (MMDNode::GetIndex is the original model index, which can differ from the manager order).
    std::unordered_map<const void*, int> nodeIdx;
    nodeIdx.reserve(n);
    for (size_t i = 0; i < n; ++i) nodeIdx[mgr->GetMMDNode(i)] = static_cast<int>(i);

    auto quatOf = [](const glm::mat4& m) -> XMFLOAT4 {
        const glm::quat q = glm::normalize(glm::quat_cast(m));
        return { q.x, q.y, q.z, q.w };
    };

    for (size_t i = 0; i < n; ++i) {
        auto* node   = mgr->GetMMDNode(i);
        auto* parent = node->GetParent();
        const glm::mat4& g = node->GetGlobalTransform();   // model-space, final (VMD+IK+physics)

        BonePose bp;
        bp.name      = node->GetName();
        bp.parent    = parent ? nodeIdx[parent] : -1;
        bp.globalPos = { g[3].x, g[3].y, g[3].z };
        bp.globalRot = quatOf(g);

        // Local = relative to parent, from the true globals (matches the drawn mesh exactly).
        const glm::mat4 local = parent ? (glm::inverse(parent->GetGlobalTransform()) * g) : g;
        bp.localPos = { local[3].x, local[3].y, local[3].z };
        bp.localRot = quatOf(local);
        out.push_back(std::move(bp));
    }
}

bool MmdAnimator::HeadBasis(DirectX::XMFLOAT3& outFwd, DirectX::XMFLOAT3& outRight) const {
    if (!m_impl->model) return false;
    auto* mgr = m_impl->model->GetNodeManager();
    const size_t n = mgr->GetNodeCount();
    for (size_t i = 0; i < n; ++i) {
        auto* node = mgr->GetMMDNode(i);
        const std::string& nm = node->GetName();
        // MMD head bone is 頭; accept romanized variants too.
        if (nm == "\xE9\xA0\xAD" || nm == "head" || nm == "Head" || nm == "\xE9\xA0\xAD\xE5\x85\x88") {
            const glm::mat4& g = node->GetGlobalTransform();  // model-space, final (VMD+IK+physics)
            outFwd   = { g[2].x, g[2].y, g[2].z };   // bone Z axis (forward, pre-negate)
            outRight = { g[0].x, g[0].y, g[0].z };   // bone X axis (right, pre-negate)
            return true;
        }
    }
    return false;
}

// ============================== ProbeMmd ==============================

bool ProbeMmd(const std::wstring& pmxPath, const std::wstring& vmdPath) {
    const std::string pmxU8 = saba::ToUtf8String(pmxPath);

    auto model = std::make_shared<saba::PMXModel>();
    if (!model->Load(pmxU8, "")) {
        std::printf("[mmd] FAILED to load PMX: %s\n", pmxU8.c_str());
        return false;
    }

    std::printf("[mmd] Loaded PMX: %s\n", pmxU8.c_str());
    std::printf("[mmd]   vertices : %zu\n", model->GetVertexCount());
    std::printf("[mmd]   indices  : %zu  (%zu triangles)\n",
                model->GetIndexCount(), model->GetIndexCount() / 3);
    std::printf("[mmd]   materials: %zu\n", model->GetMaterialCount());

    auto* nodeMan = model->GetNodeManager();
    const size_t boneCount = nodeMan->GetNodeCount();
    std::printf("[mmd]   bones    : %zu\n", boneCount);
    for (size_t i = 0; i < boneCount && i < 4; ++i)
        std::printf("[mmd]       bone[%zu] = %s\n", i, nodeMan->GetMMDNode(i)->GetName().c_str());

    std::printf("[mmd]   morphs   : %zu\n", model->GetMorphManager()->GetMorphCount());

    if (!vmdPath.empty()) {
        const std::string vmdU8 = saba::ToUtf8String(vmdPath);
        saba::VMDFile vmd;
        if (!saba::ReadVMDFile(&vmd, vmdU8.c_str())) {
            std::printf("[mmd] FAILED to read VMD: %s\n", vmdU8.c_str());
            return false;
        }
        std::printf("[mmd] Loaded VMD: %s\n", vmdU8.c_str());
        std::printf("[mmd]   bone keyframes : %zu\n", vmd.m_motions.size());
        std::printf("[mmd]   morph keyframes: %zu\n", vmd.m_morphs.size());
        std::printf("[mmd]   camera keyframes: %zu\n", vmd.m_cameras.size());

        model->InitializeAnimation();
        auto anim = std::make_unique<saba::VMDAnimation>();
        if (!anim->Create(model) || !anim->Add(vmd)) {
            std::printf("[mmd] FAILED to bind VMD animation to model\n");
            return false;
        }
        anim->SyncPhysics(0.0f);
        anim->Evaluate(0.0f);
        std::printf("[mmd] VMD animation bound to skeleton OK (evaluated frame 0)\n");
    }

    std::printf("[mmd] Probe OK.\n");
    return true;
}

// ============================== BuildSceneFromMmd ==============================

bool BuildSceneFromMmd(
    ID3D12Device*                   device,
    ID3D12CommandQueue*             queue,
    saba::MMDModel&                 model,
    const std::vector<std::string>& materialNames,
    Scene&                          outScene)
{
    const size_t vtxCount = model.GetVertexCount();
    const size_t idxCount = model.GetIndexCount();
    const glm::vec3* positions = model.GetPositions();
    const glm::vec3* normals   = model.GetNormals();
    const glm::vec2* uvs       = model.GetUVs();
    if (vtxCount == 0 || idxCount == 0 || !positions || !normals || !uvs) {
        std::fprintf(stderr, "[MmdLoader] PMX has no usable geometry\n");
        return false;
    }

    // Bind-pose vertices in model space (PMX is left-handed Y-up like our world).
    std::vector<Vertex> vertices(vtxCount);
    XMFLOAT3 bmin{ FLT_MAX, FLT_MAX, FLT_MAX };
    XMFLOAT3 bmax{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (size_t i = 0; i < vtxCount; ++i) {
        vertices[i].position = { positions[i].x, positions[i].y, positions[i].z };
        vertices[i].normal   = { normals[i].x,   normals[i].y,   normals[i].z };
        // Saba flips V on load (1-v) for OpenGL's bottom-left texture origin; undo it so the
        // UVs are PMX/DirectX convention (top-left) for our D3D sampler. Without this the eyes,
        // eyebrows, eyelashes and catchlights all sample the wrong texture rows.
        vertices[i].uv       = { uvs[i].x, 1.0f - uvs[i].y };
        const glm::vec3& p = positions[i];
        bmin.x = std::min(bmin.x, p.x); bmin.y = std::min(bmin.y, p.y); bmin.z = std::min(bmin.z, p.z);
        bmax.x = std::max(bmax.x, p.x); bmax.y = std::max(bmax.y, p.y); bmax.z = std::max(bmax.z, p.z);
    }

    // Indices: PMX may store 1/2/4-byte indices; normalise to UINT32.
    std::vector<UINT> indices(idxCount);
    const size_t idxElem = model.GetIndexElementSize();
    const void*  idxData = model.GetIndices();
    if (idxElem == 1) {
        const uint8_t* p = static_cast<const uint8_t*>(idxData);
        for (size_t i = 0; i < idxCount; ++i) indices[i] = p[i];
    } else if (idxElem == 2) {
        const uint16_t* p = static_cast<const uint16_t*>(idxData);
        for (size_t i = 0; i < idxCount; ++i) indices[i] = p[i];
    } else {
        const uint32_t* p = static_cast<const uint32_t*>(idxData);
        for (size_t i = 0; i < idxCount; ++i) indices[i] = p[i];
    }

    ResourceUploadBatch upload(device);
    upload.Begin();

    const UINT64 vbSize = static_cast<UINT64>(vertices.size()) * sizeof(Vertex);
    outScene.vertexBuffer = CreateDefaultBuffer(device, vbSize);
    NameObject(outScene.vertexBuffer.Get(), L"MMD VB (bind pose)");
    {
        D3D12_SUBRESOURCE_DATA sd{};
        sd.pData = vertices.data(); sd.RowPitch = (LONG_PTR)vbSize; sd.SlicePitch = sd.RowPitch;
        upload.Upload(outScene.vertexBuffer.Get(), 0, &sd, 1);
        upload.Transition(outScene.vertexBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    }
    outScene.vbv.BufferLocation = outScene.vertexBuffer->GetGPUVirtualAddress();
    outScene.vbv.SizeInBytes    = static_cast<UINT>(vbSize);
    outScene.vbv.StrideInBytes  = sizeof(Vertex);

    const UINT64 ibSize = static_cast<UINT64>(indices.size()) * sizeof(UINT);
    outScene.indexBuffer = CreateDefaultBuffer(device, ibSize);
    NameObject(outScene.indexBuffer.Get(), L"MMD IB");
    {
        D3D12_SUBRESOURCE_DATA sd{};
        sd.pData = indices.data(); sd.RowPitch = (LONG_PTR)ibSize; sd.SlicePitch = sd.RowPitch;
        upload.Upload(outScene.indexBuffer.Get(), 0, &sd, 1);
        upload.Transition(outScene.indexBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    }
    outScene.ibv.BufferLocation = outScene.indexBuffer->GetGPUVirtualAddress();
    outScene.ibv.SizeInBytes    = static_cast<UINT>(ibSize);
    outScene.ibv.Format         = DXGI_FORMAT_R32_UINT;

    // Per-material diffuse textures, applied EXACTLY as the PMX assigns them. Each
    // material's m_texture is the path Saba resolved from the PMX texture table; we load
    // it straight — no atlas substitution (TEX_LML_Face.png is used as-is, NOT a
    // hole-punched "Combined" variant) and no fabricated sphere/mask/normal textures (the
    // PMX references none — every material is sphereMode=None, toon05). Transparency in
    // EyeA / Eff_Facial is handled by the alpha test in the geometry shader, exactly as
    // MMD does it. White 1x1 fallback at slot 0 for any material with no texture.
    const size_t matCount = model.GetMaterialCount();
    const saba::MMDMaterial* mats = model.GetMaterials();

    std::vector<ComPtr<ID3D12Resource>> textures;
    const UINT whiteIndex = 0;
    textures.push_back(CreateWhite1x1(device, upload));

    std::vector<UINT> matSrvIdx(matCount, whiteIndex);
    std::unordered_map<std::string, UINT> texCache;

    auto loadTexture = [&](const std::string& path) -> int {
        if (path.empty()) return -1;
        if (auto it = texCache.find(path); it != texCache.end()) return (int)it->second;
        ComPtr<ID3D12Resource> res;
        HRESULT hr = CreateWICTextureFromFile(device, upload, Widen(path).c_str(), &res, true);
        if (FAILED(hr)) {
            // A missing optional texture (e.g. an unused emission map the PMX references but the
            // model doesn't ship) just falls back to the white 1x1 — keep the console clean and
            // don't warn for that. Warn only for textures that exist but failed to decode.
            if (hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
                std::fprintf(stderr, "[MmdLoader] texture decode failed (HR=0x%08lX): %s\n",
                             static_cast<unsigned long>(hr), path.c_str());
            return -1;
        }
        const UINT idx = static_cast<UINT>(textures.size());
        textures.push_back(res);
        texCache.emplace(path, idx);
        return (int)idx;
    };

    for (size_t mi = 0; mi < matCount; ++mi) {
        const int di = loadTexture(mats[mi].m_texture);
        if (di >= 0) matSrvIdx[mi] = (UINT)di;
    }

    // MMD toon ramp + sphere/MatCap per material (ZZZ ramp+matcap framework). These are PMX
    // material fields, not _D siblings; Saba resolves them to file paths.
    std::vector<UINT> matToonIdx(matCount, whiteIndex), matSphereIdx(matCount, whiteIndex);
    std::vector<int>  matSphereMode(matCount, 0);
    for (size_t mi = 0; mi < matCount; ++mi) {
        const int ti = loadTexture(mats[mi].m_toonTexture);
        if (ti >= 0) matToonIdx[mi] = (UINT)ti;
        const int si = loadTexture(mats[mi].m_spTexture);
        if (si >= 0) matSphereIdx[mi] = (UINT)si;
        matSphereMode[mi] = static_cast<int>(mats[mi].m_spTextureMode);  // None/Mul/Add = 0/1/2
    }

    // PBR sibling maps for game-ripped models (Arknights: Endfield / HSR / etc.): the PMX material
    // references only the "..._D.ext" BaseColor, but the full PBR set (_N normal, _P packed, _M
    // mask, _E emissive) ships alongside — often NOT beside the diffuse but in a separate folder
    // (e.g. "other tex/") the PMX never references. Derive each sibling's filename from the _D
    // diffuse and search: same dir → common PBR-map folders → recursively under the model root.
    // Presence of any _D diffuse marks the model as the Endfield PBR render profile. Traditional
    // MMD toon models (no _D suffix) get none of this and stay on the cel profile.
    auto loadSibling = [&](const std::string& diffuseU8, const std::wstring& suffix) -> UINT {
        if (diffuseU8.rfind("_D.") == std::string::npos) return whiteIndex;
        const fs::path dp(Widen(diffuseU8));
        std::wstring fname = dp.filename().wstring();
        const size_t wp = fname.rfind(L"_D.");
        if (wp == std::wstring::npos) return whiteIndex;
        fname.replace(wp, 2, suffix);                       // "_D" → suffix, keep ".<ext>"
        const fs::path dir  = dp.parent_path();
        const fs::path base = dir.parent_path();
        std::error_code ec;
        fs::path found;
        const fs::path cands[] = {
            dir / fname,
            base / L"other tex" / fname, base / L"othertex" / fname,
            base / L"tex" / fname, base / L"textures" / fname, base / fname,
        };
        for (const auto& c : cands) if (fs::exists(c, ec)) { found = c; break; }
        if (found.empty() && !base.empty()) {
            for (auto it = fs::recursive_directory_iterator(base, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (it->is_regular_file(ec) && it->path().filename().wstring() == fname) {
                    found = it->path(); break;
                }
            }
        }
        if (found.empty()) return whiteIndex;
        const int idx = loadTexture(Narrow(found.wstring()));
        return (idx >= 0) ? (UINT)idx : whiteIndex;
    };

    std::vector<UINT> matNormalIdx(matCount, whiteIndex), matPackedIdx(matCount, whiteIndex),
                      matMaskIdx(matCount, whiteIndex),  matEmissIdx(matCount, whiteIndex);
    bool anyPbr = false;
    for (size_t mi = 0; mi < matCount; ++mi) {
        const std::string& d = mats[mi].m_texture;
        if (d.rfind("_D.") == std::string::npos) continue;
        anyPbr = true;
        UINT n = loadSibling(d, L"_N");
        if (n == whiteIndex) n = loadSibling(d, L"_HN");    // hair ships _HN instead of _N
        matNormalIdx[mi] = n;
        matPackedIdx[mi] = loadSibling(d, L"_P");
        matMaskIdx[mi]   = loadSibling(d, L"_M");
        matEmissIdx[mi]  = loadSibling(d, L"_E");
    }
    outScene.profile = anyPbr ? RenderProfile::EndfieldPBR : RenderProfile::Cel;
    // Ground-truth per-material PBR-sibling table (Endfield): which of _N/_P/_M/_E actually resolved
    // for each material. Y = found, - = missing (falls back to white). This is how we spot a material
    // that "didn't get its _P" (e.g. its diffuse isn't a _D-suffixed per-material texture).
    if (anyPbr) {
        std::printf("[Endfield] material PBR-sibling table (%zu materials):\n", matCount);
        for (size_t mi = 0; mi < matCount; ++mi) {
            const std::string& d = mats[mi].m_texture;
            std::string diffBase = d.empty() ? "(none)"
                : Narrow(fs::path(Widen(d)).filename().wstring());
            const std::string mn = (mi < materialNames.size()) ? materialNames[mi] : std::string();
            const bool hasD = d.rfind("_D.") != std::string::npos;
            std::printf("  [%2zu] %-14s D=%c N=%c P=%c M=%c E=%c  <- %s\n", mi, mn.c_str(),
                        hasD ? 'Y' : '-',
                        matNormalIdx[mi] != whiteIndex ? 'Y' : '-',
                        matPackedIdx[mi] != whiteIndex ? 'Y' : '-',
                        matMaskIdx[mi]   != whiteIndex ? 'Y' : '-',
                        matEmissIdx[mi]  != whiteIndex ? 'Y' : '-', diffBase.c_str());
        }
        std::fflush(stdout);
    }

    // ===== Endfield "full NPR" experiment: load the maps the game uses that we previously ignored
    // (toon ramp _RD, subsurface _ST, colour LUT, reflection _RS, face SDF/cm/hl, hair detail). Most
    // are shared "T_actor_common_*" files the PMX never references → search the model tree by filename
    // substring and load once (cached). Assigned per-material by part-type (from the diffuse name). =====
    std::vector<UINT> matRampIdx(matCount, whiteIndex),  matSubsurfIdx(matCount, whiteIndex),
                      matLutIdx(matCount, whiteIndex),   matReflectIdx(matCount, whiteIndex),
                      matHairDetailIdx(matCount, whiteIndex);
    if (anyPbr) {
        // model root (parent of the textures/ folder) from the first _D material
        fs::path base;
        for (size_t mi = 0; mi < matCount; ++mi) {
            const std::string& d = mats[mi].m_texture;
            if (d.rfind("_D.") != std::string::npos) { base = fs::path(Widen(d)).parent_path().parent_path(); break; }
        }
        std::unordered_map<std::string, UINT> cache;   // substr -> SRV, so a shared file loads once
        auto findLoad = [&](const char* substr) -> UINT {
            std::string key = substr;
            auto it0 = cache.find(key); if (it0 != cache.end()) return it0->second;
            UINT result = whiteIndex;
            std::error_code ec;
            if (!base.empty() && fs::exists(base, ec)) {
                for (auto it = fs::recursive_directory_iterator(base, ec);
                     it != fs::recursive_directory_iterator(); it.increment(ec)) {
                    if (ec) break;
                    if (!it->is_regular_file(ec)) continue;
                    std::string fn = Narrow(it->path().filename().wstring());
                    for (char& c : fn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (fn.find(substr) != std::string::npos && fn.size() > 4 &&
                        fn.compare(fn.size() - 4, 4, ".png") == 0) {
                        const int idx = loadTexture(Narrow(it->path().wstring()));
                        if (idx >= 0) result = static_cast<UINT>(idx);
                        break;
                    }
                }
            }
            cache[key] = result; return result;
        };
        // model-wide face maps
        outScene.faceSdfSrv = findLoad("face_01_sdf");
        outScene.faceCmSrv  = findLoad("face_01_cm_m");
        outScene.faceHlSrv  = findLoad("hl_m");
        // per-material assignment by part-type (diffuse name)
        for (size_t mi = 0; mi < matCount; ++mi) {
            std::string d = mats[mi].m_texture;
            if (d.rfind("_D.") == std::string::npos) continue;
            for (char& c : d) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const bool isFace  = d.find("face") != std::string::npos || d.find("iris") != std::string::npos;
            const bool isBody  = d.find("body") != std::string::npos;
            const bool isHairP = d.find("hair") != std::string::npos;
            const bool isCloth = d.find("cloth") != std::string::npos;
            const bool isSkinP = isFace || isBody;
            // _RD toon ramp (part-specific common ramp)
            matRampIdx[mi] = isFace  ? findLoad("face_01_rd")
                           : isBody  ? findLoad("body_01_rd")
                           : isHairP ? findLoad("hair_01_rd")
                           : isCloth ? findLoad("cloth_04_rd") : whiteIndex;
            // colour LUT: skin vs cloth
            matLutIdx[mi]  = isSkinP ? findLoad("skincolor")
                           : isCloth ? findLoad("cloth_lut") : whiteIndex;
            // _RS reflection sphere: cloth / hair
            matReflectIdx[mi] = isCloth ? findLoad("cloth_02_rs")
                              : isHairP ? findLoad("hair_03_rs") : whiteIndex;
            // hair strand / hairline detail (hair only)
            matHairDetailIdx[mi] = isHairP ? findLoad("hairline") : whiteIndex;
            // _ST subsurface: the material's own sibling first, else the common face _ST
            UINT st = loadSibling(mats[mi].m_texture, L"_ST");
            if (st == whiteIndex && isFace) st = findLoad("face_01_st");
            if (st == whiteIndex && isHairP) st = findLoad("hairst");
            matSubsurfIdx[mi] = st;
        }
        std::printf("[Endfield] NPR maps: SDF=%u cm=%u hl=%u (per-mat ramp/lut/refl/ss/hair assigned)\n",
                    outScene.faceSdfSrv, outScene.faceCmSrv, outScene.faceHlSrv);
        std::fflush(stdout);
    }

    // Endfield reflection matcap: a FIXED cloth/body leather sphere from the model's matcap/ folder,
    // matched by exact filename. The Endfield forward pass binds it to the else-unused mask slot (t3).
    if (anyPbr) {
        const std::string kClothStem = "1B1B1B_999999_575757_747474";   // cloth/body leather
        for (size_t mi = 0; mi < matCount && outScene.matcapSrv == whiteIndex; ++mi) {
            const std::string& d = mats[mi].m_texture;
            if (d.rfind("_D.") == std::string::npos) continue;
            std::error_code ec;
            const fs::path base = fs::path(Widen(d)).parent_path().parent_path();
            if (base.empty() || !fs::exists(base, ec)) continue;
            for (auto it = fs::recursive_directory_iterator(base, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                if (Narrow(it->path().stem().wstring()) == kClothStem) {
                    const int idx = loadTexture(Narrow(it->path().wstring()));
                    if (idx >= 0) { outScene.matcapSrv = static_cast<UINT>(idx); break; }
                }
            }
        }
        std::printf("[MmdLoader] Endfield matcap: cloth SRV %u\n", outScene.matcapSrv);
        std::fflush(stdout);
    }

    // Emission textures for the Eff facial decals (TEX_*_Eff_*_Emission.png) — the vivid glow
    // colours that make blush/sweat/blue-face pop (the plain diffuse is pale). Loaded as the
    // sibling of each Eff material's diffuse; falls back to the diffuse if there's no emission.
    std::vector<UINT> matEmissiveIdx(matCount, whiteIndex);
    for (size_t mi = 0; mi < matCount; ++mi) {
        const std::string& dtex = mats[mi].m_texture;
        if (dtex.find("Eff") == std::string::npos) continue;
        std::string em = dtex;
        size_t dot = em.rfind(".png");
        if (dot == std::string::npos) dot = em.rfind(".PNG");
        int ei = -1;
        if (dot != std::string::npos) { em.insert(dot, "_Emission"); ei = loadTexture(em); }
        matEmissiveIdx[mi] = (ei >= 0) ? (UINT)ei : matSrvIdx[mi];
    }

    // Alternate expression eyes (EyeB / EyeC): not referenced by any material, so load them
    // explicitly as siblings of the EyeA texture for the time-based eye-swap feature.
    outScene.eyeSrv = { whiteIndex, whiteIndex, whiteIndex };
    {
        std::string eyeAPath;
        for (size_t mi = 0; mi < matCount; ++mi)
            if (mats[mi].m_texture.find("EyeA") != std::string::npos) { eyeAPath = mats[mi].m_texture; break; }
        if (!eyeAPath.empty()) {
            auto sibling = [&](const char* to) {
                std::string p = eyeAPath;
                const size_t pos = p.find("EyeA");
                if (pos != std::string::npos) p.replace(pos, 4, to);
                return p;
            };
            const int a = loadTexture(eyeAPath);                 // already cached
            const int b = loadTexture(sibling("EyeB"));
            const int c = loadTexture(sibling("EyeC"));
            const UINT fa = (a >= 0) ? (UINT)a : whiteIndex;
            outScene.eyeSrv[0] = fa;
            outScene.eyeSrv[1] = (b >= 0) ? (UINT)b : fa;
            outScene.eyeSrv[2] = (c >= 0) ? (UINT)c : fa;
            outScene.eyeSwapAvailable = (b >= 0 || c >= 0);
        }
    }

    auto finish = upload.End(queue);
    finish.wait();

    // SRV heap. One extra slot at the end holds the engine directional shadow map so the Endfield
    // forward pass can sample it from this heap (written later by the Renderer).
    outScene.shadowSrvSlot = static_cast<UINT>(textures.size());
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = static_cast<UINT>(textures.size()) + 1;
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&outScene.srvHeap)));
    NameObject(outScene.srvHeap.Get(), L"MMD SRV Heap");

    const UINT srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    outScene.srvDescriptorSize = srvSize;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = outScene.srvHeap->GetCPUDescriptorHandleForHeapStart();
    for (auto& tex : textures) {
        const D3D12_RESOURCE_DESC rd = tex->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format                        = rd.Format;
        sd.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels           = rd.MipLevels;
        sd.Texture2D.ResourceMinLODClamp = 0.0f;
        device->CreateShaderResourceView(tex.Get(), &sd, handle);
        handle.ptr += srvSize;
    }

    // Submeshes: one draw per material range, kept in PMX material order so the natural
    // MMD draw order is preserved (Eye → Face → ... → Hair → Eff_* facial features last,
    // layering the eyebrows/eyelashes over the bangs). The facial features live in the
    // "Eff_*" materials (Eff_Facial / Eff_Bickle / Eff_Tear textures); the expression
    // variants (Pink/Aozame/Angry/Bickle/Sweat) sit on transparent UV regions in the bind
    // pose and the alpha test drops them until a VMD morph slides their UVs onto a drawn
    // icon — so the neutral pose shows only the natural brows/lashes (Eff_Default).
    const size_t subCount = model.GetSubMeshCount();
    const saba::MMDSubMesh* subs = model.GetSubMeshes();
    std::vector<Submesh> submeshes;
    submeshes.reserve(subCount);
    for (size_t i = 0; i < subCount; ++i) {
        const int matId = subs[i].m_materialID;
        Submesh sm;
        sm.indexStart   = static_cast<UINT>(subs[i].m_beginIndex);
        sm.indexCount   = static_cast<UINT>(subs[i].m_vertexCount);
        sm.srvHeapIndex = (matId >= 0 && static_cast<size_t>(matId) < matSrvIdx.size())
                              ? matSrvIdx[matId] : whiteIndex;
        const bool matOk = (matId >= 0 && static_cast<size_t>(matId) < matNormalIdx.size());
        sm.normalSrvIndex = matOk ? matNormalIdx[matId] : whiteIndex;
        sm.srvPacked      = matOk ? matPackedIdx[matId] : whiteIndex;
        sm.srvMask        = matOk ? matMaskIdx[matId]   : whiteIndex;
        sm.srvEmiss       = matOk ? matEmissIdx[matId]  : whiteIndex;
        sm.srvToon        = matOk ? matToonIdx[matId]   : whiteIndex;
        sm.srvSphere      = matOk ? matSphereIdx[matId] : whiteIndex;
        sm.sphereMode     = matOk ? matSphereMode[matId] : 0;
        sm.srvRamp        = matOk ? matRampIdx[matId]       : whiteIndex;
        sm.srvSubsurf     = matOk ? matSubsurfIdx[matId]    : whiteIndex;
        sm.srvLut         = matOk ? matLutIdx[matId]        : whiteIndex;
        sm.srvReflect     = matOk ? matReflectIdx[matId]    : whiteIndex;
        sm.srvHairDetail  = matOk ? matHairDetailIdx[matId] : whiteIndex;
        if (matOk) {
            sm.matDiffuse = { mats[matId].m_diffuse.x, mats[matId].m_diffuse.y, mats[matId].m_diffuse.z };
            sm.matAlpha   = mats[matId].m_alpha;
        }
        // Tag the eye submeshes (they sample the EyeA texture) so the renderer can swap them
        // to the EyeB/EyeC expression textures over a chosen frame window.
        if (matId >= 0 && static_cast<size_t>(matId) < matCount) {
            const std::string& mtex = mats[matId].m_texture;
            sm.isEye  = mtex.find("EyeA") != std::string::npos;
            // Many rips (e.g. ZZZ 诺姆) draw all eye parts from one shared face texture (脸.png) and
            // distinguish them only by MATERIAL NAME. Detect the clean eyeball parts (眼/目/瞳/eye)
            // by name so the shader renders them bright/unlit — but EXCLUDE the lash/lid-line (睫/线/
            // 線), the eye-shadow overlay (影) and the brow (眉), which must stay as dark inked parts.
            {
                const std::string& mn = (static_cast<size_t>(matId) < materialNames.size())
                                            ? materialNames[matId] : std::string();
                auto has = [](const std::string& s, const char* sub) {
                    return s.find(sub) != std::string::npos;
                };
                const bool nameEye  = has(mn, "\xE7\x9C\xBC")   // 眼
                                   || has(mn, "\xE7\x9B\xAE")   // 目
                                   || has(mn, "\xE7\x9E\xB3")   // 瞳
                                   || has(mn, "eye") || has(mn, "Eye");
                const bool nameDark = has(mn, "\xE7\x9D\xAB")   // 睫 (lash)
                                   || has(mn, "\xE7\xBA\xBF")   // 线 (line, simplified)
                                   || has(mn, "\xE7\xB7\x9A")   // 線 (line, traditional)
                                   || has(mn, "\xE5\xBD\xB1")   // 影 (shadow overlay)
                                   || has(mn, "\xE7\x9C\x89")   // 眉 (brow)
                                   || has(mn, "lash") || has(mn, "brow");
                if (nameEye && !nameDark) sm.isEye = true;
            }
            // Body/face skin (TEX_LML_Skin / SkinSotai / Face) → eligible for SSS, and (Endfield) the
            // skin path (flat albedo×AO). Game rips name skin LOWERCASE (T_actor_*_face_/_body_), which
            // the capitalised-only match missed. A "body_" material is bare skin ONLY when it has no _P
            // (arms/neck) — a body_ WITH _P is a bodysuit (别礼) and stays cloth. So the face + the arms/
            // hands share the same flat skin tone, fixing the face-too-white-vs-fingers mismatch.
            const bool bodyNoPacked = mtex.find("body") != std::string::npos &&
                (matId < 0 || static_cast<size_t>(matId) >= matPackedIdx.size() ||
                 matPackedIdx[matId] == whiteIndex);
            sm.isSkin = mtex.find("Skin") != std::string::npos ||
                        mtex.find("Face") != std::string::npos ||
                        mtex.find("face") != std::string::npos ||
                        bodyNoPacked;
            // Hair material (Endfield angel-ring highlight). Match common naming, incl. game rips.
            sm.isHair = mtex.find("hair") != std::string::npos ||
                        mtex.find("Hair") != std::string::npos ||
                        mtex.find("\xE5\x8F\x91") != std::string::npos ||   // 发 (simplified hair)
                        mtex.find("\xE9\xAB\xAE") != std::string::npos;     // 髮 (traditional hair)
            // Metal material (ZZZ PBR/MatCap branch): 金属 / Metal.
            sm.isMetal = mtex.find("Metal") != std::string::npos ||
                         mtex.find("metal") != std::string::npos ||
                         mtex.find("\xE9\x87\x91\xE5\xB1\x9E") != std::string::npos; // 金属
            // Eff_* facial decals (blush/sweat/blue-face/anger-vein/tears/icon eyes) — these are
            // soft/emissive in MMD; render them in the forward decal pass (diffuse blend + the
            // emission added) and skip the saturation boost so they don't turn garish.
            sm.isEffDecal = mtex.find("Eff") != std::string::npos;
            sm.emissiveSrvIndex = matEmissiveIdx[matId];
        }
        if (sm.indexCount == 0) continue;
        submeshes.push_back(sm);
        (void)materialNames;
    }

    outScene.submeshes   = std::move(submeshes);
    outScene.textures    = std::move(textures);
    outScene.boundsMin   = bmin;
    outScene.boundsMax   = bmax;
    outScene.vertexCount = static_cast<UINT>(vertices.size());
    outScene.indexCount  = static_cast<UINT>(indices.size());

    std::printf(
        "[MmdLoader] %u verts, %u indices, %zu submeshes, %zu textures, "
        "model bounds (%.1f,%.1f,%.1f)..(%.1f,%.1f,%.1f)\n",
        outScene.vertexCount, outScene.indexCount,
        outScene.submeshes.size(), outScene.textures.size(),
        bmin.x, bmin.y, bmin.z, bmax.x, bmax.y, bmax.z);
    return true;
}

} // namespace dr
