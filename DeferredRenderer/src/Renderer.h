#pragma once
#include "Common.h"
#include "Camera.h"
#include "Scene.h"
#include "Input.h"
#include "DatasetGen.h"
#include <d3d12.h>
#include <random>
#include <dxgi1_6.h>
#include <DirectXMath.h>
#include <directxtk12/GraphicsMemory.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dr {

class MmdAnimator;
class Audio;

enum class ViewMode : int {
    Depth   = 0,
    Normal  = 1,
    Albedo  = 2,
    Color   = 3,
    Outline = 4,   // line-art: black edges over flat fill (dataset style variant)
    // Depth of the CHARACTER only, normalised to the character's own near..far span (near = white)
    // with everything else masked out. The plain Depth view divides by zFar, so a character 500
    // units away lands in the bottom 5% of the range and quantises to a couple of grey levels —
    // useless as depth supervision. Not part of the Z cycle (which stays Depth/Normal/Albedo/Color).
    CharDepth = 5,
};

const char* ViewModeName(ViewMode m);

class Renderer {
public:
    static constexpr UINT kFrameCount = 2;

    // RTV layout: back buffers, then normal, albedo, sceneHDR, bloom0, bloom1, ssao, ssaoBlur, ldr.
    static constexpr UINT kRtvHeapSize           = kFrameCount + 8;
    static constexpr UINT kGBufferNormalRtvIndex = kFrameCount + 0;
    static constexpr UINT kGBufferAlbedoRtvIndex = kFrameCount + 1;
    static constexpr UINT kSceneHdrRtvIndex      = kFrameCount + 2;
    static constexpr UINT kBloom0RtvIndex        = kFrameCount + 3;
    static constexpr UINT kBloom1RtvIndex        = kFrameCount + 4;
    static constexpr UINT kSsaoRtvIndex          = kFrameCount + 5;
    static constexpr UINT kSsaoBlurRtvIndex      = kFrameCount + 6;
    static constexpr UINT kLdrRtvIndex           = kFrameCount + 7;  // tonemap output when FXAA is on

    // Shader-visible SRV heap: depth, normal, albedo (lighting reads 0..2), sceneHDR,
    // bloom0, bloom1, shadow, ssao, ssaoBlur, ssaoNoise, pointCube, ldr.
    static constexpr UINT kGBufferSrvHeapSize    = 12;
    static constexpr UINT kLightingSrvCount      = 3;
    static constexpr UINT kSrvSceneHdr           = 3;
    static constexpr UINT kSrvBloom0             = 4;
    static constexpr UINT kSrvBloom1             = 5;
    static constexpr UINT kSrvShadow             = 6;
    static constexpr UINT kSrvSsao               = 7;
    static constexpr UINT kSrvSsaoBlur           = 8;
    static constexpr UINT kSrvNoise              = 9;
    static constexpr UINT kSrvPointCube          = 10;  // point-light distance shadow cube
    static constexpr UINT kSrvLdr                = 11;  // tonemapped LDR image (FXAA input)
    static constexpr UINT kShadowMapSize         = 2048;
    static constexpr UINT kPointShadowSize       = 256;  // per cube face (baked, not per-frame)

    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void Init(HWND hwnd, UINT width, UINT height);
    void InitImGui(HWND hwnd);       // on-screen controls (play/pause, SSAO, exposure…)
    void SetImGuiVisible(bool v) { m_imguiVisible = v; }
    void Resize(UINT width, UINT height);
    void Update(const Input& input, float dt);
    void Render();
    void Shutdown();

    bool LoadScene(const std::wstring& objPath);
    bool LoadMmdModel(const std::wstring& pmxPath);
    bool LoadMmdMotion(const std::wstring& vmdPath);
    bool LoadBgm(const std::wstring& wavPath);   // dance music, looped + synced to the motion

    // Selectable motion clips (GUI "Dance" combo + console `motion`). Each clip is a VMD plus
    // an optional BGM. Switching reloads the VMD on the same character and swaps the music so a
    // music-less clip plays silently and loops on its own clock — it never fights the default
    // dance + bgm.wav. AddMotionClip is called at load time; clip 0 is the active default.
    void        AddMotionClip(const std::string& name, const std::wstring& vmdPath,
                              const std::wstring& bgmPath);
    int         MotionClipCount() const;
    std::string MotionClipName(int i) const;
    int         CurrentMotionClip() const { return m_currentClip; }
    bool        SelectMotion(int index);   // thread-safe: defers the heavy switch to the render thread
    // Selectable characters (GUI "Character" combo + console `char load`). Switching reloads the
    // PMX, rebuilds GPU resources, and rebinds the current motion — heavy, so it's deferred to the
    // render thread like motion switching. Clip 0 is the character loaded at startup.
    void        AddCharacter(const std::string& name, const std::wstring& pmxPath);
    int          CharacterCount() const;
    std::string  CharacterName(int i) const;
    std::wstring CharacterPath(int i) const;
    bool         HasCharacter() const { return !m_mmd.submeshes.empty(); }
    int          CurrentCharacter() const { return m_currentChar; }
    bool         SelectCharacter(int index);   // thread-safe: defers reload to the render thread

    bool LoadMmdCameraMotion(const std::wstring& vmdPath);  // optional cam.vmd camera track
    bool HasCameraMotion() const;
    bool& CamMotionRef() { return m_camMotionOn; }
    float& CamYOffsetRef() { return m_camYOffset; }   // VMD camera vertical shift (down = negative)

    // Selectable camera tracks (GUI "Cam track" combo + console `cam`). Each clip is a camera
    // VMD; only one is active at a time, so a new track never fights the original cam.vmd.
    // Clip 0 is the default loaded at startup; switching reloads the track on the render thread.
    void        AddCameraClip(const std::string& name, const std::wstring& vmdPath);
    int         CameraClipCount() const;
    std::string CameraClipName(int i) const;
    int         CurrentCameraClip() const { return m_currentCamera; }
    bool        SelectCamera(int index);   // thread-safe: defers the reload to the render thread

    // Expression eye-swap: between startFrame and endFrame (MMD 30 fps frames, within the
    // looping motion) the character's eyes use texture expr (0=EyeA, 1=EyeB, 2=EyeC).
    void AddEyeWindow(int startFrame, int endFrame, int expr);
    void ClearEyeWindows();
    void PrintEyeWindows() const;
    bool EyeSwapAvailable() const { return m_mmd.eyeSwapAvailable; }

    // Facial-expression morphs (TEX_LML_Eff_Facial.png icons etc.). List the morphs and
    // schedule one to show over a frame window.
    size_t      MmdMorphCount() const;
    std::string MmdMorphName(size_t i) const;
    int         FindMmdMorph(const std::string& name) const;   // -1 if not found
    // Print the loaded character's bone list with its canonical mapping (console `bones`). Bone
    // counts and names vary wildly between models, so this is how you check a rig before using it.
    void PrintBoneList(const std::string& filter) const;

    void AddExprWindow(int morphIdx, int startFrame, int endFrame, float weight);
    void ClearExprWindows();
    void PrintExprWindows() const;

    // Live placement tuning for the character (recomputes its world matrix from the
    // model-space bounds + these params). Yaw is in degrees.
    void SetCharacterPos(float x, float y, float z) { m_charPos = { x, y, z }; RecomputeCharacterWorld(); }
    void SetCharacterScale(float s)                 { m_charScale = s;         RecomputeCharacterWorld(); }
    void SetCharacterYaw(float deg)                 { m_charYawDeg = deg;       RecomputeCharacterWorld(); }
    void PrintCharacterTransform() const;

    // Jump the animation to an absolute time (seconds) and re-skin — for screenshots.
    void SampleAnimation(double seconds);

    // Animation playback control (wired to the GUI / SPACE key). These also drive the BGM
    // (pause/resume/restart) so the music stays frame-aligned with the motion.
    void SetPaused(bool p);
    void TogglePause();
    void Replay();         // restart motion + music from the top, playing
    double DanceSpeed() const { return m_animSpeed.load(); }
    void   SetDanceSpeed(double s) { m_animSpeed.store(s < 0.05 ? 0.05 : s); }
    bool IsPaused() const  { return m_animPaused; }
    bool HasAnimation() const;

    bool& VsyncRef() { return m_vsync; }   // off = uncapped present (for FPS measurement)

    // Bullet cloth/hair physics toggle — the big per-frame animation cost (a Debug dance is ~15fps
    // with it, ~115 without). Applied to the live animator and re-applied on every character load.
    void SetPhysics(bool on);
    bool PhysicsEnabled() const { return m_physicsOn; }

    // SSAO toggle + tuning (README: AO must be enable/disable-able).
    bool& SsaoEnabledRef() { return m_ssaoEnabled; }
    float& SsaoRadiusRef()    { return m_ssaoRadius; }
    float& SsaoIntensityRef() { return m_ssaoIntensity; }
    float& ExposureRef()      { return m_exposure; }
    float& BloomStrengthRef() { return m_bloomStrength; }
    float& ShadowsRef()       { return m_shadows; }
    float& HighlightsRef()    { return m_highlights; }
    float& SpecFocusRef()     { return m_efSpecFocus; }
    float& SheenRef()         { return m_efSheen; }
    float& HairRangeRef()     { return m_efHairRange; }
    bool&  FaceSdfRef()       { return m_efFaceSdf; }
    float& FaceFloorRef()     { return m_efFaceFloor; }
    float& EfFidelityRef()    { return m_efFidelity; }
    float  Ssaa() const       { return m_ssaa; }
    void   RequestSsaa(float s) { m_pendingSsaa.store(s); }   // thread-safe; applied at next frame top
    bool&  BloomEnabledRef()  { return m_bloomEnabled; }
    bool&  ForwardPlusRef()   { return m_forwardPlus; }
    bool&  FpHeatRef()        { return m_fpDebugHeat; }
    UINT   PointLightCount() const { return kNumPointLights; }
    float& CharSatRef()       { return m_charSat; }
    float& CharContrastRef()  { return m_charContrast; }
    float& OutlineDarkenRef() { return m_outlineDarken; }
    float& SssStrengthRef()   { return m_sssStrength; }
    float& SssWrapRef()       { return m_sssWrap; }
    float& SpecIntRef()       { return m_specInt; }
    float& SpecPowRef()       { return m_specPow; }
    float& SkinFresnelRef()   { return m_skinFresnel; }
    bool&  DirLightRef()      { return m_dirLightOn; }
    void   SetAllLights(bool on) { m_dirLightOn = on; m_forwardPlus = on; m_pointShadowOn = on; }
    bool&  PointShadowRef()   { return m_pointShadowOn; }   // the N coloured lights cast cube shadows
    bool&  CharXrayRef()      { return m_charXray; }        // character revealed through occluding buildings
    float& XrayRadiusRef()    { return m_xrayRadiusScale; }
    float& XrayStrengthRef()  { return m_xrayStrength; }

    // Directional-light direction-to-light (need not be normalized); drives shading + shadow.
    void SetLightDir(float x, float y, float z) { m_lightDir = { x, y, z }; }
    DirectX::XMFLOAT3 GetLightDir() const { return m_lightDir; }

    // Saves the most recently presented back buffer to a PNG (for the §7 self-grade
    // screenshots). Waits for the GPU first; safe to call from the main loop.
    bool Screenshot(const std::wstring& path, bool verbose = true, bool wantAlpha = false);

    // ---- Dataset generation (2D-anime-image -> 3D-pose training data) ----
    // The GUI/console edit this config, then request a run. The heavy sweep (stops the anim
    // worker, drives the animator + camera deterministically, renders every frame/view/style/
    // background, and writes images + JSON) runs on the render thread, picked up in Update().
    DatasetConfig& DatasetCfgRef() { return m_datasetCfg; }
    void  RequestDataset() { m_datasetRequested.store(true); }
    bool  DatasetBusy() const { return m_datasetBusy.load(); }
    int   DatasetProgress() const { return m_datasetDone.load(); }
    int   DatasetTotal() const { return m_datasetTotal.load(); }
    // Runs the full sweep synchronously (blocks the caller). Normally invoked via RequestDataset.
    void  GenerateDataset(const DatasetConfig& cfg);

    // Live style preview: apply one dataset render style to the on-screen view so a style can be
    // eyeballed before a batch export. styleId < 0 turns preview off and restores the user's
    // pre-preview look/knobs. Safe to call every time the selection changes.
    void  PreviewStyle(int styleId);
    int   StyleCount() const;                       // number of dataset styles
    int   CurrentStylePreview() const { return m_stylePreview; }
    bool& CaptureIsolatedRef() { return m_captureIsolated; }   // "去背" cut-out preview toggle

    // Endfield render-profile debug channel (0 = BaseColor .. 8 = emissive). Only affects
    // characters on the EndfieldPBR profile.
    int&  EndfieldDebugRef() { return m_endfieldDebug; }
    float& ZzzSatRef()       { return m_zzzSat; }
    float& WuwaExposureRef() { return m_wuwaExposure; }
    float& WuwaShadowTintRef() { return m_wuwaShadowTint; }
    float& WuwaFidelityRef() { return m_wuwaFidelity; }
    float& ZzzFidelityRef()  { return m_zzzTexFidelity; }
    float& ZzzDeepenRef()    { return m_zzzDeepen; }
    float& ZzzWarmthRef()    { return m_zzzWarmth; }
    float& ZzzEyeLiftRef()   { return m_zzzEyeLift; }
    // Render profile of the loaded character (set by its game folder; overridable at runtime).
    RenderProfile CharProfile() const { return m_mmd.profile; }
    void          SetCharProfile(RenderProfile p) { m_mmd.profile = p; ApplyProfileChannelDefaults(); }
    bool          CharIsForwardPBR() const { return m_mmd.profile != RenderProfile::Cel; }
    // Set the _P metal/rough channel defaults for the current profile (packings differ per game rip):
    // Arknights Endfield packs R=metal, A=rough (B=AO, G=hair-spec) — confirmed by channel analysis.
    void          ApplyProfileChannelDefaults();

    void CycleView();
    void SetView(ViewMode v) { m_view = v; }
    ViewMode GetView() const noexcept { return m_view; }

    Camera& GetCamera() noexcept { return m_camera; }
    const Scene& GetScene() const noexcept { return m_scene; }

private:
    void CreateDeviceResources();
    void CreateSwapChain(HWND hwnd, UINT width, UINT height);
    void CreateFrameResources();
    void CreateDepthBuffer(UINT width, UINT height);
    void CreateGBufferSrvHeap();
    void CreateGBuffer(UINT width, UINT height);
    void UpdateRenderResolution();   // recompute m_rw/m_rh from m_width/m_height * m_ssaa
    void RecreateGBufferSrvs();
    void CreateGeometryPipeline();
    void CreatePostPipeline();
    void CreateEndfieldPipeline();  // dedicated forward NPR+PBR pass for Endfield-profile characters
    void BindShadowIntoMmdHeap();   // write the directional shadow SRV into m_mmd.srvHeap's reserved slot
    void CreateShadowResources();   // shadow map + DSV/SRV + pipeline
    void CreateSsaoResources();     // ssao RT + blur RT + noise + pipelines
    void BuildImGuiUI();            // builds the control panel each frame
    void RecomputeCharacterWorld();
    void StreamSkinnedVertices();   // upload the worker's latest skinned verts into a dynamic VB
    void CreateLightingPipeline();

    // Animation worker: the expensive MMD update (bone + morph + Bullet physics + CPU
    // skinning) runs on this thread so the render frame rate stays high regardless of how
    // long an update takes. It produces CPU-skinned vertices into a double buffer that the
    // render thread uploads each frame.
    void AnimThreadProc();
    void StartAnimThread();
    void StopAnimThread();
    void ApplyMotionSwitch(int index);   // heavy clip switch (VMD reload + BGM swap + worker bounce); render thread only
    void ApplyCameraSwitch(int index);   // reload the active camera track; render thread only
    void ApplyCharacterSwitch(int index);// reload PMX + rebuild GPU + rebind motion; render thread only

    void ReleaseBackBuffers();
    // Copies the scene depth buffer (post-Render, so it holds scene + character) into system
    // memory as R32 floats at the internal render resolution. Used by the dataset generator to
    // decide, per joint, whether it is actually visible or hidden behind geometry.
    bool ReadDepthBuffer(std::vector<float>& out, UINT& width, UINT& height);
    // Fills JointRecord::visible for the frame just rendered, and reports how many canonical
    // joints were in frame / actually visible (the sample's occlusion summary).
    void MarkJointVisibility(std::vector<JointRecord>& recs, int imgSize, float bodyHeight,
                             int& canonicalInFrame, int& canonicalVisible);
    void WaitForGpu();
    void MoveToNextFrame();
    void DrainInfoQueue();          // surface D3D12 debug-layer WARNING/ERROR to the console

    HWND m_hwnd   = nullptr;
    UINT m_width  = 0;
    UINT m_height = 0;

    ComPtr<IDXGIFactory4>        m_factory;
    ComPtr<ID3D12Device>         m_device;
    ComPtr<ID3D12CommandQueue>   m_queue;
    ComPtr<IDXGISwapChain3>      m_swap;
#if defined(_DEBUG)
    ComPtr<ID3D12InfoQueue>      m_infoQueue;   // debug-layer message sink (drained each frame)
#endif

    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;        // kRtvHeapSize slots
    UINT                         m_rtvSize = 0;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;        // 1 slot
    UINT                         m_dsvSize = 0;
    ComPtr<ID3D12DescriptorHeap> m_gbufferSrvHeap; // kGBufferSrvHeapSize slots, shader-visible
    UINT                         m_srvSize = 0;

    std::array<ComPtr<ID3D12Resource>, kFrameCount>         m_backBuffers;
    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount> m_allocators;
    ComPtr<ID3D12GraphicsCommandList>                       m_cmd;

    ComPtr<ID3D12Resource>       m_depth;     // R32_TYPELESS, DSV=D32_FLOAT, SRV=R32_FLOAT
    ComPtr<ID3D12Resource>       m_depthReadback;        // READBACK heap, grown on demand
    UINT64                       m_depthReadbackBytes = 0;
    // Character depth span of the frame just rendered (view-space units) — the scale of the
    // CharDepth view, recorded into each dataset annotation so the image decodes to metric depth.
    float                        m_charDepthNear = 0.0f;
    float                        m_charDepthFar  = 0.0f;
    ComPtr<ID3D12Resource>       m_normalRT;  // R16G16B16A16_FLOAT
    ComPtr<ID3D12Resource>       m_albedoRT;  // R8G8B8A8_UNORM
    ComPtr<ID3D12Resource>       m_sceneHDR;  // R16G16B16A16_FLOAT — lighting output (HDR)
    ComPtr<ID3D12Resource>       m_bloom0;    // half-res RGBA16F (bright/blur ping)
    ComPtr<ID3D12Resource>       m_bloom1;    // half-res RGBA16F (blur pong)

    // Endfield forward NPR+PBR pass (per-model profile). Characters whose Scene.profile is
    // EndfieldPBR are drawn here into sceneHDR instead of the deferred G-buffer.
    ComPtr<ID3D12RootSignature>  m_endfieldRS;
    ComPtr<ID3D12PipelineState>  m_endfieldPSO;
    ComPtr<ID3D12PipelineState>  m_endfieldOutlinePSO;  // back-face-expansion outline (CULL_FRONT)
    ComPtr<ID3D12PipelineState>  m_endfieldBlendPSO;    // alpha-blended overlays (eye/hair shadow, depth no-write)
    // Wuthering Waves (鸣潮) PSOs — same forward RS/CBs, own PBR-NPR shader (Wuwa.hlsl).
    ComPtr<ID3D12PipelineState>  m_wuwaPSO;
    ComPtr<ID3D12PipelineState>  m_wuwaOutlinePSO;
    ComPtr<ID3D12PipelineState>  m_wuwaBlendPSO;
    // Zenless Zone Zero (绝区零) PSOs — same forward RS/CBs, own ramp+matcap shader (Zzz.hlsl).
    ComPtr<ID3D12PipelineState>  m_zzzPSO;
    ComPtr<ID3D12PipelineState>  m_zzzOutlinePSO;
    ComPtr<ID3D12PipelineState>  m_zzzBlendPSO;
    int                          m_endfieldDebug = 0;   // 0 toon .. 8 emissive (see Endfield.hlsl)
    float                        m_endfieldOutline   = 1.6f;  // outline width in px (0 = off)
    float                        m_endfieldToonThresh = 0.5f; // binary-diffuse threshold
    float                        m_endfieldToonFeather = 0.06f;
    // Endfield material/look (M6/M7). Metal/rough source channels of _P are GUI-selectable since
    // the ripped pack's channel order is unconfirmed.
    int                          m_efMetalChan = 0, m_efRoughChan = 1, m_efInvertRough = 0;
    float                        m_efSpec = 0.25f, m_efRoughBias = 0.0f;
    float                        m_efRim = 0.15f, m_efRimPow = 4.0f, m_efEmiss = 1.0f;
    float                        m_efHair = 0.4f;   // hair angel-ring highlight strength
    float                        m_efHairRange = 40.0f;  // hair KK band width (higher = narrower/sharper, less "oily")
    bool                         m_efFaceSdf = false;    // face SDF shadow — OFF by default (ref default = flat painted face)
    float                        m_efFaceFloor = 0.6f;   // face flatness: 1 = flat/bright, lower = shaded like the body
    bool                         m_efNormalMap = true;
    bool                         m_efFlipNormalY = false;  // flip _N green channel (DX vs GL)
    float                        m_efShadowStr = 0.65f;  // received cast-shadow strength ("Shadow recv"; user-set)
    float                        m_efShadowDepth = 0.6f; // dark-side brightness (lower = darker)
    float                        m_outlineRefFrac = 0.5f;  // outline is full width when the character fills this fraction of screen height; scales down proportionally when smaller
    float                        m_outlineDepthBias = 18.0f;  // world-units the outline is pushed back so the body hides it at small-gap self-overlaps
    float                        m_wuwaExposure = 0.8f;  // Wuwa-only character brightness (<1 = dimmer; user wanted lower)
    float                        m_wuwaShadowTint = 0.4f;// Wuwa cold shadow-tint amount (1 = full blue-violet, 0 = neutral grey)
    float                        m_wuwaFidelity = 0.7f;  // Wuwa texture-colour fidelity (undo exposure/ACES → stop the white wash)
    float                        m_zzzMatcap = 2.2f;     // ZZZ metal MatCap strength
    float                        m_zzzSat    = 1.0f;     // ZZZ extra saturation (1.0 = faithful to texture)
    float                        m_zzzTexFidelity = 0.9f;// ZZZ texture-colour fidelity: pre-invert the global exposure/ACES/vibrance so the composited character keeps its painted albedo (0 = stylised, 1 = exact texture)
    float                        m_zzzDeepen  = 0.28f;   // ZZZ overall colour deepen/darken (0 = none)
    float                        m_zzzWarmth  = 0.38f;   // ZZZ warm/orange hue push (yellow hair → orange; 0 = neutral)
    float                        m_zzzEyeLift = 0.60f;   // ZZZ eye/hair shadow-overlay + eyeball shading lift (0 = original dark, 1 = no darkening)
    DirectX::XMFLOAT3            m_efRimColor{ 0.80f, 0.85f, 1.0f };

    ComPtr<ID3D12RootSignature>  m_geometryRS;
    ComPtr<ID3D12PipelineState>  m_geometryPSO;
    ComPtr<ID3D12PipelineState>  m_geometryPSONoCull;    // MMD character: double-sided / arbitrary winding
    ComPtr<ID3D12PipelineState>  m_charXrayDepthPSO;     // depth-only, DepthFunc=ALWAYS: resets scene depth at the character silhouette so it draws over walls
    ComPtr<ID3D12PipelineState>  m_decalPSO;             // Eff facial decals: forward alpha-blend (diffuse base)
    ComPtr<ID3D12PipelineState>  m_decalEmissivePSO;     // Eff facial decals: additive emission (glow colour)
    ComPtr<ID3D12RootSignature>  m_lightingRS;
    ComPtr<ID3D12PipelineState>  m_lightingPSO;
    ComPtr<ID3D12RootSignature>  m_postRS;            // bright / blur / tonemap share this
    ComPtr<ID3D12PipelineState>  m_brightPSO;
    ComPtr<ID3D12PipelineState>  m_blurPSO;
    ComPtr<ID3D12PipelineState>  m_tonemapPSO;
    ComPtr<ID3D12PipelineState>  m_fxaaPSO;      // post-process edge anti-aliasing
    ComPtr<ID3D12Resource>       m_ldrRT;        // full-res LDR tonemap output → FXAA → back buffer
    bool                         m_fxaa = true;  // FXAA on by default (softens the aliased edges)
    float                        m_fxaaStrength = 1.0f;  // FXAA sub-pixel aliasing strength 0..1 (GUI "AA")
    // Supersampling anti-aliasing: render the whole internal pipeline (G-buffer, depth, sceneHDR,
    // bloom, SSAO) at m_ssaa× the window resolution, then the tonemap box-downsamples to the
    // window-size LDR/back buffer. This is the real fix for the NPR outline + toon-break shimmer
    // that FXAA (a post filter) can't stabilise. 1.0 = off; 2.0 = 4× the pixels.
    float                        m_ssaa = 2.0f;
    UINT                         m_rw = 0, m_rh = 0;   // supersampled render resolution
    std::atomic<float>           m_pendingSsaa{ -1.0f };  // GUI/console request; applied at frame top (rebuilds RTs)

    ComPtr<ID3D12Resource>       m_shadowMap;         // D32 depth from the light's POV
    ComPtr<ID3D12RootSignature>  m_shadowRS;
    ComPtr<ID3D12PipelineState>  m_shadowPSO;
    DirectX::XMFLOAT3            m_lightDir{ -0.577f, -0.577f, -0.577f };
    bool                        m_dirLightOn = true;   // directional "sun" master toggle

    float m_exposure       = 1.2f;
    float m_shadows        = 0.0f;   // CHARACTER-only tone: + lifts shadow detail on the char texture
    float m_highlights     = 0.0f;   // CHARACTER-only tone: - recovers highlight detail on the char texture
    float m_efSpecFocus    = 0.0f;   // concentrate the character's specular highlight (0 = wide, 1 = tight spot)
    float m_efSheen        = 0.0f;   // leather/latex reflection — OFF by default (opt-in per char; it reflects on
                                     // ALL dark Endfield materials, so it wrongly lit 李织烟's coat/legs "from below").
    float m_efFidelity     = 0.5f;   // Endfield texture-colour fidelity — OPTIONAL accuracy (undo exposure/ACES); not the leather look
    float m_bloomStrength  = 0.6f;
    float m_bloomThreshold = 1.0f;
    bool  m_bloomEnabled   = true;

    // Character / look tunables (GUI-controlled).
    // Character "x-ray reveal": inside a screen circle around the character, occluding buildings
    // are seen through to reveal it (dithered soft edge / translucency), so the VMD camera never
    // loses sight of it. Self-occlusion stays correct; outside the circle, normal occlusion.
    bool  m_charXray           = true;
    float m_xrayRadiusScale    = 1.6f;  // window radius = projected character half-height x this
    float m_xrayStrength       = 0.6f;  // reveal opacity (1 = solid; <1 = occluder dithers through)

    bool  m_charNormalMap     = true;   // apply "_N" normal maps on PBR game models (…_D/_N)
    float m_charSat            = 5.0f;  // character albedo saturation boost (1 = faithful)
    float m_charContrast       = 1.0f;  // character albedo contrast (1 = unchanged)
    float m_outlineDarken      = 0.70f; // cel outline = interior fill x this (lower = darker)

    // Character surface look (cel shader, GUI sliders). SSS = cheap wrap-diffuse scatter.
    float             m_sssStrength = 0.35f;
    DirectX::XMFLOAT3 m_sssColor{ 1.0f, 0.0f, 0.0f };   // fixed red subsurface tint
    float             m_sssWrap     = 0.50f;
    float             m_specInt     = 0.30f;            // normal-based specular highlight
    float             m_specPow     = 32.0f;
    float             m_skinFresnel = 0.0f;             // skin view/normal sheen (camera-based)
    float m_vibrance           = 1.25f; // whole-scene saturation in tonemap

    std::atomic<bool> m_animPaused{ false };   // read by the anim worker, written by control
    bool              m_physicsOn = true;      // Bullet cloth/hair physics (see SetPhysics)

    ComPtr<ID3D12DescriptorHeap> m_imguiSrvHeap;
    bool  m_imguiReady     = false;
    bool  m_imguiVisible   = true;

    // SSAO
    ComPtr<ID3D12Resource>       m_ssaoRT;     // R8_UNORM occlusion (full res)
    ComPtr<ID3D12Resource>       m_ssaoBlurRT; // blurred occlusion
    ComPtr<ID3D12Resource>       m_ssaoNoise;  // 4x4 RG random rotations
    ComPtr<ID3D12RootSignature>  m_ssaoRS;
    ComPtr<ID3D12PipelineState>  m_ssaoPSO;
    ComPtr<ID3D12PipelineState>  m_ssaoBlurPSO;
    bool  m_ssaoEnabled   = true;
    float m_ssaoRadius    = 40.0f;
    float m_ssaoIntensity = 1.3f;
    float m_ssaoBias      = 1.5f;

    // Forward+ tiled point lights: LightCulling.hlsl culls all lights per 16x16 tile into
    // per-tile index lists (compute), then the lighting pass only sums each pixel's tile.
    static constexpr UINT kTileSize         = 16;
    static constexpr UINT kMaxLightsPerTile = 64;
    static constexpr UINT kNumPointLights   = 128;  // exceeds the 96-light requirement
    struct PointLightGPU {
        DirectX::XMFLOAT3 posWS; float radius;
        DirectX::XMFLOAT3 color; float intensity;
    };
    std::vector<PointLightGPU>     m_pointLights;
    std::vector<DirectX::XMFLOAT3> m_lightHomePos;   // rest positions (lights orbit these)
    ComPtr<ID3D12Resource>         m_tileLightBuffer; // RWStructuredBuffer<uint> per-tile lists
    ComPtr<ID3D12RootSignature>    m_lightCullRS;
    ComPtr<ID3D12PipelineState>    m_lightCullPSO;
    DirectX::XMUINT2               m_tileCount{ 0, 0 };
    bool  m_forwardPlus   = false;  // the 128 coloured point lights — off by default (demo with `fplus on`)
    bool  m_fpDebugHeat   = false;
    float m_lightAnimTime = 0.0f;

    void GeneratePointLights();
    void SelectShadowedLights();   // move the N lights nearest the character to indices 0..N-1
    void CreateLightCullPipeline();
    void CreateTileLightBuffer(UINT width, UINT height);

    // Cube distance-shadows for the first kNumShadowedLights of the 128 Forward+ point lights
    // (the rest are unshadowed but still lit + bloom-marked). Each gets one cube in a cube
    // ARRAY; the lighting pass samples cube `lightIndex` for those lights.
    static constexpr UINT kNumShadowedLights = 4;
    bool m_pointShadowOn = false;    // off by default — demo with `plight on` (needs fplus on)
    // The shadowed lights are kept STATIC (don't orbit) and their cubes are baked only when
    // dirty (enable / config / character placement change), not every frame — re-rendering the
    // whole scene 6×N every frame is far too slow. Character animation is frozen in these cubes.
    bool m_pointShadowDirty = true;
    bool m_prevForwardPlus  = false; // detect fplus/plight toggles to re-bake
    bool m_prevPointShadow  = false;
    ComPtr<ID3D12Resource>       m_pointCube;       // R32_FLOAT TextureCubeArray (6*N slices)
    ComPtr<ID3D12Resource>       m_pointCubeDepth;  // D32 depth, reused per face
    ComPtr<ID3D12DescriptorHeap> m_pointRtvHeap;    // 6*N face RTVs
    ComPtr<ID3D12RootSignature>  m_pointShadowRS;
    ComPtr<ID3D12PipelineState>  m_pointShadowPSO;
    // Bloom sphere marker
    ComPtr<ID3D12Resource>       m_sphereVB;
    ComPtr<ID3D12Resource>       m_sphereIB;
    D3D12_VERTEX_BUFFER_VIEW     m_sphereVBV{};
    D3D12_INDEX_BUFFER_VIEW      m_sphereIBV{};
    UINT                         m_sphereIndexCount = 0;
    ComPtr<ID3D12RootSignature>  m_sphereRS;
    ComPtr<ID3D12PipelineState>  m_spherePSO;

    void CreatePointShadowResources();
    void CreateBloomSphere();

    std::unique_ptr<DirectX::GraphicsMemory> m_graphicsMemory;

    ComPtr<ID3D12Fence>             m_fence;
    std::array<UINT64, kFrameCount> m_fenceValues{};
    HANDLE                          m_fenceEvent = nullptr;
    UINT                            m_frameIndex = 0;
    UINT                            m_lastPresentedIndex = 0;

    Scene    m_scene;
    Scene    m_mmd;     // PMX character placed inside Sponza
    Camera   m_camera;

    // Skeletal animation: drives the character each frame and re-skins into a dynamic
    // vertex buffer allocated from GraphicsMemory.
    std::unique_ptr<MmdAnimator> m_animator;
    DirectX::GraphicsResource    m_mmdDynamicVB;

    // Animation worker thread state. m_skinFront holds the most recent completed skinned
    // verts (render thread reads under m_skinMutex); the worker fills m_skinBack then swaps.
    std::thread             m_animThread;
    std::mutex              m_skinMutex;
    std::vector<Vertex>     m_skinFront;
    std::vector<Vertex>     m_skinBack;
    size_t                  m_skinVertexCount = 0;
    std::atomic<bool>       m_animThreadRun{ false };
    std::atomic<bool>       m_animHasFrame{ false };   // worker produced at least one frame
    std::atomic<bool>       m_animRefresh{ false };    // re-skin once while paused (expr changed)
    std::atomic<double>     m_seekRequest{ -1.0 };     // >=0 → worker seeks the motion there

    // Dance BGM. Loops forever; paused/resumed/restarted in lockstep with the motion. The
    // motion is wrapped at m_bgmLength so frame time and music time stay aligned across loops.
    std::unique_ptr<Audio> m_audio;
    float                  m_bgmLength = 0.0f;   // loop period in seconds (0 = no BGM)

    // Dance time-scale relative to the audio master clock: pose time = audioPos * m_animSpeed.
    // Set per clip to motionLen/musicLen so the dance fills the song (a short dance plays
    // slower, fixing "dance faster than music"); the GUI/console expose it for fine-tuning.
    std::atomic<double>    m_animSpeed{ 1.0 };

    // Selectable dance clips. The switch itself is heavy (reload VMD, swap BGM, bounce the
    // anim worker) and touches m_animator, which the worker owns and the render thread also
    // queries, so a request from the GUI/console is parked in m_pendingMotionSwitch and applied
    // on the render thread in Update() via ApplyMotionSwitch.
    struct MotionClip { std::string name; std::wstring vmd; std::wstring bgm; };
    std::vector<MotionClip> m_motionClips;
    int                     m_currentClip = 0;
    std::atomic<int>        m_pendingMotionSwitch{ -1 };

    // Present pacing: vsync on (1) caps to the monitor refresh; off (0, with tearing support)
    // uncaps the frame rate so the true CPU/GPU throughput can be measured.
    bool m_vsync           = true;
    BOOL m_tearingSupported = FALSE;

    // Character placement (world matrix is rebuilt from these + model-space bounds).
    DirectX::XMFLOAT3 m_charPos{ 0.0f, 0.0f, 250.0f };
    float             m_charScale  = 1.0f;
    float             m_charYawDeg = 180.0f; // face -Z toward the start camera

    // VMD camera track (cam.vmd). When m_camMotionOn, the render camera follows the evaluated
    // look-at (transformed into world space by the character's world matrix) instead of the
    // free-fly camera. Matrices are recomputed each frame in Update().
    bool              m_camMotionOn = false;
    // Vertical world-space offset applied to the VMD camera (eye + target). The camera.vmd was
    // authored for a taller model, so our shorter character sits low in frame; nudge the whole
    // shot down to recentre on it. Negative = down. GUI slider "Cam height" + console `camy`.
    float             m_camYOffset  = -60.0f;
    // Selectable camera tracks (cam.vmd, the seele camera.vmd, …). The reload touches the
    // animator's camAnim, which the render thread also evaluates, so a request is parked here
    // and applied on the render thread in Update() via ApplyCameraSwitch.
    struct CameraClip { std::string name; std::wstring vmd; };
    std::vector<CameraClip> m_cameraClips;
    int                     m_currentCamera = 0;
    std::atomic<int>        m_pendingCameraSwitch{ -1 };
    DirectX::XMFLOAT4X4 m_camView{};
    DirectX::XMFLOAT4X4 m_camProj{};
    DirectX::XMFLOAT3   m_camPos{};

    // Expression eye-swap windows (frame ranges → eye texture index). Evaluated each frame
    // against the current looped motion time to pick which eye texture the eyes sample.
    struct EyeWindow { int start; int end; int expr; };
    std::vector<EyeWindow> m_eyeWindows;
    int CurrentEyeExpr() const;   // 0/1/2 for the current motion frame (default 0 = EyeA)

    // Facial-expression morph windows (mirrors the animator's MorphWindow; kept here so the
    // list can be accumulated, then pushed wholesale to the animator).
    struct ExprWindow { int start; int end; int morphIdx; float weight; };
    std::vector<ExprWindow> m_exprWindows;
    void PushExprWindows();   // forward m_exprWindows to the animator
    std::vector<float> m_morphWeights;   // GUI "all morphs" slider values (per morph)
    int m_guiExpr = 0;        // GUI "Expression" combo selection (0 = none)
    int m_guiIris = 0;        // GUI "Iris" combo selection (0 = EyeA)
    ViewMode m_view = ViewMode::Color;

    // -------------------- Dataset generation --------------------
    // Capture-time background isolation: when set, the Sponza scene is not drawn and background
    // pixels get m_captureBg (solid) with alpha 0 so the PNG is a clean cut-out of the character.
    bool              m_captureIsolated = false;
    DirectX::XMFLOAT3 m_captureBg{ 0.0f, 0.7f, 0.25f };   // linear bg colour under isolation
    // Directional-light colour tint (fed to the lighting pass as lightIntensity). Default white;
    // the RandomLight dataset style varies it per sample.
    DirectX::XMFLOAT3 m_lightTint{ 1.0f, 1.0f, 1.0f };

    // Selectable characters (PMX hot-swap). Reload is heavy and touches m_animator/m_mmd, so a
    // GUI/console request is parked here and applied on the render thread in Update().
    struct CharacterAsset { std::string name; std::wstring pmx; };
    std::vector<CharacterAsset> m_characters;
    int                     m_currentChar = 0;
    std::atomic<int>        m_pendingCharSwitch{ -1 };

    DatasetConfig       m_datasetCfg;
    std::atomic<bool>   m_datasetRequested{ false };
    std::atomic<bool>   m_datasetBusy{ false };
    std::atomic<int>    m_datasetDone{ 0 };
    std::atomic<int>    m_datasetTotal{ 0 };
    // Applies one render style (pipeline/knob combo) before a capture. Some styles randomize the
    // light direction/tint using rng so repeated runs with the same seed are reproducible.
    void ApplyDatasetStyle(int styleId, std::mt19937& rng);

    // Live style-preview state: -1 = off. When a preview is first activated we snapshot the user's
    // look so turning it off restores exactly what they had.
    int          m_stylePreview       = -1;
    bool         m_stylePreviewActive = false;
    std::mt19937 m_previewRng{ 12345u };
    struct StyleSnapshot {
        ViewMode view; bool bloom, ssao, dir; float exp, vib, sat, con, spec;
        DirectX::XMFLOAT3 lightDir, tint;
    } m_styleSnap{};
};

} // namespace dr
