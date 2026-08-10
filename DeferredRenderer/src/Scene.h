#pragma once
#include "Common.h"
#include <d3d12.h>
#include <DirectXMath.h>
#include <array>
#include <string>
#include <vector>

namespace dr {

struct Vertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 uv;
};

// Per-model rendering method, chosen by which game folder a character lives in (different folder
// = different game = different render method). Cel = deferred cel path (LeMaline/MMD). EndfieldPBR
// = Arknights: Endfield forward NPR+PBR. WuwaPBR = Wuthering Waves (鸣潮) PBR-based NPR.
enum class RenderProfile { Cel, EndfieldPBR, WuwaPBR, ZzzNPR };

const char* RenderProfileName(RenderProfile p);

// Pick the render profile from a character's file path by matching the game name in the folder
// (e.g. "终末地"/endfield → EndfieldPBR, "鸣潮"/wuwa → WuwaPBR). Unrecognised → Cel. This is how
// "organise characters into per-game folders" maps to a rendering method.
RenderProfile ProfileForPath(const std::wstring& path);

struct Submesh {
    UINT indexStart   = 0;
    UINT indexCount   = 0;
    UINT srvHeapIndex = 0;  // PMX-assigned diffuse texture, SRV index into srvHeap
    UINT normalSrvIndex = 0;// tangent-space normal map (0 = none/white) for PBR game models (…_N.png)
    // Endfield PBR profile extra maps (SRV indices into srvHeap; 0 = none/white):
    UINT srvPacked = 0;     // …_P packed PBR (metallic/roughness/AO — channel order TBD via debug)
    UINT srvMask   = 0;     // …_M material/region mask
    UINT srvEmiss  = 0;     // …_E emissive
    // MMD material toon ramp + sphere/MatCap (used by the ZZZ ramp+mask/matcap shader).
    UINT srvToon   = 0;     // PMX per-material toon ramp (m_toonTexture)
    UINT srvSphere = 0;     // PMX sphere / MatCap (m_spTexture)
    int  sphereMode = 0;    // 0 none, 1 multiply, 2 add (Saba SphereTextureMode)
    // Endfield "full NPR" experiment maps (per-material, resolved by part-type; 0 = none/white):
    UINT srvRamp      = 0;  // _RD 1D toon diffuse ramp (body/face/hair/cloth common ramp)
    UINT srvSubsurf   = 0;  // _ST subsurface scatter tint (per-material / hairst for hair)
    UINT srvLut       = 0;  // colour-grade LUT (skincolor for skin, cloth_lut for cloth)
    UINT srvReflect   = 0;  // _RS reflection/spec env sphere (cloth/hair)
    UINT srvHairDetail= 0;  // hairline strand / _sw shine mask (hair only)
    // PMX material diffuse colour + alpha. Texture-less overlays (eye-shadow / hair-shadow) carry
    // their shadow as a dark, sub-1 alpha diffuse here → must be tinted + alpha-blended, not drawn
    // opaque-white from the fallback texture.
    DirectX::XMFLOAT3 matDiffuse{ 1.0f, 1.0f, 1.0f };
    float             matAlpha = 1.0f;
    bool isEye        = false;  // uses the EyeA texture → eligible for the expression eye-swap
    bool isSkin       = false;  // body/face skin texture → gets subsurface scattering (SSS)
    bool isHair       = false;  // hair texture → gets the Endfield anisotropic "angel ring" highlight
    bool isMetal      = false;  // metal material (name 金属/Metal) → PBR/matcap branch (ZZZ)
    bool isEffDecal   = false;  // Eff_* facial-effect decals (blush/sweat/blue-face/…) → emissive
    UINT emissiveSrvIndex = 0;  // *_Emission texture (the vivid glow colour) for Eff decals
};

class Scene {
public:
    ComPtr<ID3D12Resource>      vertexBuffer;
    ComPtr<ID3D12Resource>      indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW    vbv{};
    D3D12_INDEX_BUFFER_VIEW     ibv{};

    std::vector<Submesh>                submeshes;
    std::vector<ComPtr<ID3D12Resource>> textures;
    ComPtr<ID3D12DescriptorHeap>        srvHeap;
    UINT                                srvDescriptorSize = 0;

    // Expression eye-swap: SRV indices for the three eye textures [A, B, C]. Eye submeshes
    // (Submesh::isEye) normally use [0]=EyeA; the renderer can bind [1]/[2] for a chosen
    // frame window. eyeSwapAvailable is true only if EyeB/EyeC were found beside EyeA.
    std::array<UINT, 3> eyeSrv{ 0, 0, 0 };
    bool                eyeSwapAvailable = false;

    // One extra SRV slot reserved at the end of srvHeap for the engine's directional shadow map,
    // so the forward pass can sample it from this same heap (only one CBV_SRV_UAV heap can be bound
    // at a time). The Renderer writes the shadow SRV here after the shadow map exists.
    UINT                shadowSrvSlot = 0;   // 0 = not reserved
    // Endfield reflection matcap (nidorx-style env sphere reflected on smooth materials). SRV index
    // into srvHeap; 0 = none. Endfield-only: bound to the else-unused mask slot (t3) so ZZZ/Wuwa/Cel
    // bindings are untouched. Fixed cloth/body leather sphere, chosen by filename in the matcap/ folder.
    UINT                matcapSrv = 0;   // cloth/body leather matcap (1B1B1B_999999_575757_747474)
    // Endfield "full NPR" experiment — model-wide face maps (0 = none/white):
    UINT                faceSdfSrv = 0;  // female_face_01_SDF — smooth rotating face shadow
    UINT                faceCmSrv  = 0;  // face_01_cm_M — face colour/makeup (blush/eye/lip) mask
    UINT                faceHlSrv  = 0;  // face_hl_M — face highlight mask

    // Per-object world transform consumed by the geometry pass (b1). Sponza is
    // pre-transformed so it stays identity; the MMD character uses this to scale/place
    // (and later to animate) without re-baking the vertex buffer.
    DirectX::XMFLOAT4X4 world{ 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

    // Model-space (untransformed) bounds.
    DirectX::XMFLOAT3 boundsMin{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 boundsMax{ 0.0f, 0.0f, 0.0f };
    UINT vertexCount = 0;
    UINT indexCount  = 0;

    // Rendering method for this model (set by BuildSceneFromMmd from the texture set).
    RenderProfile profile = RenderProfile::Cel;
};

} // namespace dr
