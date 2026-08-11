#include "Renderer.h"
#include "AssetLoader.h"
#include "MmdLoader.h"
#include "Audio.h"

#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <directxtk12/GraphicsMemory.h>
#include <directxtk12/ScreenGrab.h>
#include <directxtk12/ResourceUploadBatch.h>
#include <wincodec.h>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>
#include <vector>
#include <random>
#include <cmath>
#include <cstring>

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <set>
#include <string>
#include <unordered_set>
#include <chrono>
#include <thread>
#include <algorithm>

using namespace DirectX;
namespace fs = std::filesystem;

namespace dr {

const char* ViewModeName(ViewMode m) {
    switch (m) {
    case ViewMode::Depth:  return "Depth";
    case ViewMode::Normal: return "Normal";
    case ViewMode::Albedo: return "Albedo";
    case ViewMode::Color:  return "Color";
    case ViewMode::Outline:return "Outline";
    case ViewMode::CharDepth: return "CharDepth";
    }
    return "?";
}

namespace {

// Simple bump/free-list allocator over a shader-visible heap for the ImGui DX12 backend
// (1.92's InitInfo wants alloc/free callbacks). One window → a single file-scope instance.
struct ImGuiHeapAllocator {
    ID3D12DescriptorHeap* heap = nullptr;
    UINT inc = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart{};
    std::vector<UINT> freeList;
    void Create(ID3D12Device* dev, ID3D12DescriptorHeap* h, UINT cap) {
        heap = h;
        inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        cpuStart = h->GetCPUDescriptorHandleForHeapStart();
        gpuStart = h->GetGPUDescriptorHandleForHeapStart();
        freeList.clear();
        for (UINT i = cap; i > 0; --i) freeList.push_back(i - 1);
    }
    void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g) {
        UINT idx = freeList.back(); freeList.pop_back();
        c->ptr = cpuStart.ptr + static_cast<SIZE_T>(idx) * inc;
        g->ptr = gpuStart.ptr + static_cast<UINT64>(idx) * inc;
    }
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE c, D3D12_GPU_DESCRIPTOR_HANDLE) {
        UINT idx = static_cast<UINT>((c.ptr - cpuStart.ptr) / inc);
        freeList.push_back(idx);
    }
};
ImGuiHeapAllocator g_imguiAlloc;
void ImGuiAllocFn(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g) { g_imguiAlloc.Alloc(c, g); }
void ImGuiFreeFn (ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE c, D3D12_GPU_DESCRIPTOR_HANDLE g)  { g_imguiAlloc.Free(c, g); }

struct PerFrameCB {
    XMFLOAT4X4 viewProj;
    XMFLOAT4X4 invViewProj;
    XMFLOAT3   cameraPos;
    UINT       viewMode;
    XMFLOAT3   lightDirToLight;
    float      zNear;
    XMFLOAT3   lightIntensity;
    float      zFar;
    // Appended for shadow mapping — Geometry.hlsl declares only the prefix above, so its
    // cbuffer layout is unaffected; Lighting.hlsl / Shadow.hlsl see these too.
    XMFLOAT4X4 lightViewProj;
    float      shadowBias;
    float      shadowTexel;   // 1 / shadow map size (for PCF)
    float      outlineDarken; // cel outline: edge colour = interior fill x this
    float      _pad1;
    // Dataset isolation: xyz = solid background colour, w = 1 when isolating the character
    // (Sponza not drawn; background pixels get this colour + alpha 0 for a cut-out PNG).
    XMFLOAT4   captureBg;
    // CharDepth view: x = nearest, y = farthest view-space depth of the character this frame, so
    // its own depth span fills the full 0..1 output range. zw unused.
    XMFLOAT4   charDepthRange;
    // Endfield face SDF: head-bone basis in WORLD space (MMD -front/-right convention applied).
    // headValid = 1 when a head bone was found this frame (else the face falls back to geometric NoL).
    XMFLOAT3   headFront;  float headValid;
    XMFLOAT3   headRight;  float _hpad0;
    XMFLOAT3   headUp;     float _hpad1;
};
static_assert(sizeof(PerFrameCB) == 336, "PerFrameCB layout drift");

struct PerObjectCB {
    XMFLOAT4X4 world;
    UINT       materialId;   // 0 = scene (Blinn-Phong), 1 = MMD character (cel)
    UINT       useFaceMask;  // 1 = multiply diffuse by the baked face AO/shadow mask
    UINT       useNormalMap; // 1 = perturb normal with the tangent-space normal map
    float      satBoost;     // albedo saturation multiplier (1 = unchanged)
    XMFLOAT4X4 view;            // camera view matrix (sphere-map view-space normal)
    UINT       useSphere;       // 1 = apply the MMD sphere maps
    float      faceMaskStrength;// 0..1 face shadow/AO darkening
    float      sphereStrength;  // 0..1 sphere-map blend
    float      contrast;        // albedo contrast (1 = unchanged)
    // X-ray reveal window (used only by the character depth-reset pre-pass / PSXrayReveal):
    // a screen circle around the character inside which an occluding object is "seen through".
    XMFLOAT4   xrayReveal;      // (centerPx.x, centerPx.y, radiusPx, featherPx)
    float      xrayStrength;    // reveal opacity inside the window (1 = solid; <1 = occluder dithers through)
    float      _xpad0, _xpad1, _xpad2;
};

struct CullCB {                 // matches cbuffer CullCB in LightCulling.hlsl
    XMFLOAT4X4 invViewProj;
    XMFLOAT3   cameraPos;  UINT numLights;
    XMUINT2    screenSize; XMUINT2 tileCount;
};

struct ForwardPlusCB {          // matches cbuffer ForwardPlusCB in Lighting.hlsl
    XMUINT2  tileCount;
    UINT     numLights;
    UINT     enabled;
    UINT     debugHeat;
    UINT     pointEnabled;   // (unused) kept for layout
    UINT     dirEnabled;
    UINT     numShadowed;    // first N point lights cast cube shadows
    XMFLOAT3 sssColor;    float sssStrength;   // character subsurface scatter
    float    specInt;     float specPow;  float sssWrap;  float skinFresnel;
};

struct PointShadowCB {          // matches cbuffer PointShadowCB in PointShadow.hlsl
    XMFLOAT4X4 faceViewProj;
    XMFLOAT3   lightPos;  float lightRange;
};

struct SphereCB {               // matches cbuffer SphereCB in BloomSphere.hlsl
    XMFLOAT4X4 viewProj;
    float      radius;          // marker sphere size
    float      emissiveScale;   // light.color * this = HDR emissive
    float      pad0, pad1;
};

struct EndfieldObjectCB {       // matches cbuffer EndfieldObject in Endfield.hlsl
    XMFLOAT4X4 world;
    int        debugMode;
    float      outlineWidth;
    float      toonThreshold;
    float      toonFeather;
    XMFLOAT2   screenSize;
    int        hasPacked, hasEmissive;
    int        hasNormal, isHair, transparentMode, matClass;  // matClass: 0 cloth,1 skin,2 hair,3 eye,4 metal
    XMFLOAT3   matDiffuse; float matAlpha;
    int        sphereMode;        // MMD sphere mode (ZZZ): 0 none,1 mul,2 add
    float      outlineScale;      // 0..1 from the character's on-screen height (0 = no outline)
    float      matcapStrength;    // ZZZ metal MatCap strength
    float      satBoost;          // extra saturation (ZZZ)
    float      outlineDepthBias;  // push outline back this many world units so the body hides it where a self-overlap's depth gap is smaller
    // Trailing pad reused by ZZZ only (Endfield/Wuwa ignore — same 4-byte slots, layout identical):
    float      postExposure;      // global tonemap exposure  (ZZZ texture-fidelity pre-inversion)
    float      postVibrance;      // global tonemap vibrance  (ZZZ texture-fidelity pre-inversion)
    float      texFidelity;       // 0..1 strength of that pre-inversion (0 for Endfield/Wuwa)
    // Endfield "full NPR" experiment: bitmask of which extra maps are present for this submesh.
    // bit0 ramp(_RD) 1 subsurf(_ST) 2 lut 3 reflect(_RS) 4 hairdetail 5 sdf 6 cm 7 hl. 0 for non-Endfield.
    int        nprMask;
};

struct EndfieldMaterialCB {     // matches cbuffer EndfieldMaterial in Endfield.hlsl
    int      metalChan, roughChan, invertRough;  float specStrength;
    float    roughBias, rimStrength, rimPower, emissStrength;
    float    useNormalMap, hairStrength, normalYSign, shadowStrength;
    XMFLOAT3 rimColor;     float shadowDepth;
    // Appended for ZZZ colour grade (Endfield/Wuwa read only the prefix above → their layout is
    // unaffected). deepen = overall darken; warmth = yellow→orange hue push; eyeLift = lighten the
    // eye/hair shadow overlay.
    float    deepen, warmth, eyeLift, wuwaExposure;   // wuwaExposure: Wuwa-only brightness (1 = unchanged)
    // Appended for the forward-PBR characters (Endfield/ZZZ): per-character tone + spec focus.
    // charShadows/charHighlights = luminance-masked detail lift/recover ON THE CHARACTER only
    // (not the whole scene); specFocus = concentrate the specular highlight (tighter spot).
    float    charShadows, charHighlights, specFocus, sheenStrength;
    float    hairRange, _hr1, _hr2, _hr3;   // Endfield hair KK angel-ring band width (higher = narrower)
};

struct SsaoCB {                 // matches cbuffer SsaoCB in Ssao.hlsl
    float radius;
    float bias;
    float intensity;
    UINT  enabled;
    float screenX;
    float screenY;
    float _p0;
    float _p1;
};

struct PostCB {                 // matches cbuffer Post in PostProcess.hlsl
    float exposure;
    float bloomStrength;
    float threshold;
    UINT  vertical;
    float invTexelX;
    float invTexelY;
    UINT  viewMode;
    float vibrance;
    UINT  isolated;   // 1 = write the HDR coverage alpha through (transparent cut-out PNG)
    float fxaaSubpix; // FXAA sub-pixel aliasing strength 0..1
    float _pp1, _pp2;
};

constexpr float kCameraZNear = 1.0f;
constexpr float kCameraZFar  = 10000.0f;

void PickHardwareAdapter(IDXGIFactory4* factory, IDXGIAdapter1** out) {
    *out = nullptr;
    ComPtr<IDXGIFactory6> f6;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f6)))) {
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> ad;
            if (DXGI_ERROR_NOT_FOUND == f6->EnumAdapterByGpuPreference(
                    i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad))) break;
            DXGI_ADAPTER_DESC1 d{}; ad->GetDesc1(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (SUCCEEDED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0,
                          __uuidof(ID3D12Device), nullptr))) {
                *out = ad.Detach();
                return;
            }
        }
    }
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> ad;
        if (DXGI_ERROR_NOT_FOUND == factory->EnumAdapters1(i, &ad)) break;
        DXGI_ADAPTER_DESC1 d{}; ad->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0,
                      __uuidof(ID3D12Device), nullptr))) {
            *out = ad.Detach();
            return;
        }
    }
    throw std::runtime_error("No D3D12-capable hardware adapter found");
}

fs::path FindShaderFile(const wchar_t* name) {
    const fs::path candidates[] = {
        fs::path(L"shaders") / name,
        fs::path(L"DeferredRenderer") / L"shaders" / name,
        fs::path(L"..") / L".." / L"DeferredRenderer" / L"shaders" / name,
        fs::path(L"..") / L".." / L".." / L"DeferredRenderer" / L"shaders" / name,
    };
    std::error_code ec;
    for (const auto& p : candidates) {
        if (fs::exists(p, ec)) return fs::canonical(p, ec);
    }
    return {};
}

ComPtr<ID3DBlob> CompileShader(const fs::path& path, const char* entry, const char* target) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry, target, flags, 0, &code, &errors);
    if (FAILED(hr)) {
        if (errors) {
            std::fprintf(stderr, "[Shader] %s\n",
                static_cast<const char*>(errors->GetBufferPointer()));
        }
        ThrowIfFailed(hr);
    }
    return code;
}

} // namespace

Renderer::Renderer()  = default;
Renderer::~Renderer() { Shutdown(); }

void Renderer::Init(HWND hwnd, UINT width, UINT height) {
    m_hwnd   = hwnd;
    m_width  = (width  == 0) ? 1 : width;
    m_height = (height == 0) ? 1 : height;
    UpdateRenderResolution();   // m_rw/m_rh = window * SSAA — all internal RTs render at this size

    CreateDeviceResources();
    CreateSwapChain(hwnd, m_width, m_height);
    CreateFrameResources();
    CreateGBufferSrvHeap();
    CreateDepthBuffer(m_rw, m_rh);
    CreateGBuffer(m_rw, m_rh);
    RecreateGBufferSrvs();
    CreateGeometryPipeline();
    CreateEndfieldPipeline();
    CreateLightingPipeline();
    CreatePostPipeline();
    CreateShadowResources();
    CreateSsaoResources();
    CreateLightCullPipeline();
    GeneratePointLights();
    CreateTileLightBuffer(m_rw, m_rh);
    CreatePointShadowResources();
    CreateBloomSphere();

    m_graphicsMemory = std::make_unique<DirectX::GraphicsMemory>(m_device.Get());

    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) throw std::runtime_error("CreateEvent failed");

    m_camera.SetPerspective(XMConvertToRadians(60.0f),
        static_cast<float>(m_width) / static_cast<float>(m_height),
        kCameraZNear, kCameraZFar);
}

void Renderer::CreateDeviceResources() {
    UINT dxgiFlags = 0;
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    ThrowIfFailed(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&m_factory)));

    ComPtr<IDXGIAdapter1> adapter;
    PickHardwareAdapter(m_factory.Get(), &adapter);

    DXGI_ADAPTER_DESC1 ad{}; adapter->GetDesc1(&ad);
    char descBuf[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1,
        descBuf, sizeof(descBuf), nullptr, nullptr);
    std::printf("[D3D12] Adapter: %s (VRAM %.0f MB)\n",
        descBuf,
        static_cast<double>(ad.DedicatedVideoMemory) / (1024.0 * 1024.0));

    ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));
    NameObject(m_device.Get(), L"Device");

#if defined(_DEBUG)
    if (SUCCEEDED(m_device.As(&m_infoQueue))) {
        // Break into the debugger on truly fatal classes; ERROR is drained and printed
        // to the console (see DrainInfoQueue) so it is observable without a debugger.
        m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        D3D12_MESSAGE_SEVERITY hide[] = { D3D12_MESSAGE_SEVERITY_INFO };
        D3D12_INFO_QUEUE_FILTER f{};
        f.DenyList.NumSeverities = _countof(hide);
        f.DenyList.pSeverityList = hide;
        m_infoQueue->PushStorageFilter(&f);
    }
#endif

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)));
    NameObject(m_queue.Get(), L"DirectQueue");
}

void Renderer::CreateSwapChain(HWND hwnd, UINT width, UINT height) {
    // Tearing support lets Present(0, ALLOW_TEARING) genuinely uncap the frame rate in a
    // windowed flip-model swap chain (otherwise DWM composition pins it to the refresh).
    {
        ComPtr<IDXGIFactory5> f5;
        if (SUCCEEDED(m_factory.As(&f5))) {
            BOOL allow = FALSE;
            if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                  &allow, sizeof(allow))))
                m_tearingSupported = allow;
        }
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.BufferCount      = kFrameCount;
    sd.Width            = width;
    sd.Height           = height;
    sd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;
    sd.Flags            = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

    ComPtr<IDXGISwapChain1> sc1;
    ThrowIfFailed(m_factory->CreateSwapChainForHwnd(
        m_queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1));
    ThrowIfFailed(m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));
    ThrowIfFailed(sc1.As(&m_swap));
    m_frameIndex = m_swap->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rd{};
    rd.NumDescriptors = kRtvHeapSize;
    rd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&m_rtvHeap)));
    NameObject(m_rtvHeap.Get(), L"RtvHeap");
    m_rtvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
}

void Renderer::CreateFrameResources() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        ThrowIfFailed(m_swap->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])));
        m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtv);
        rtv.ptr += m_rtvSize;

        wchar_t name[32];
        swprintf_s(name, L"BackBuffer%u", i);
        NameObject(m_backBuffers[i].Get(), name);

        ThrowIfFailed(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocators[i])));
        swprintf_s(name, L"CmdAlloc%u", i);
        NameObject(m_allocators[i].Get(), name);
    }

    ThrowIfFailed(m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_allocators[m_frameIndex].Get(), nullptr,
        IID_PPV_ARGS(&m_cmd)));
    NameObject(m_cmd.Get(), L"DirectCmdList");
    ThrowIfFailed(m_cmd->Close());

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    NameObject(m_fence.Get(), L"FrameFence");
    m_fenceValues[m_frameIndex] = 1;
}

void Renderer::CreateGBufferSrvHeap() {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = kGBufferSrvHeapSize;
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_gbufferSrvHeap)));
    NameObject(m_gbufferSrvHeap.Get(), L"GBufferSrvHeap");
    m_srvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void Renderer::UpdateRenderResolution() {
    float s = m_ssaa;
    if (s < 1.0f) s = 1.0f;
    if (s > 2.0f) s = 2.0f;   // cap the memory/fill cost at 4x pixels
    long rw = std::lroundf(static_cast<float>(m_width)  * s);
    long rh = std::lroundf(static_cast<float>(m_height) * s);
    m_rw = static_cast<UINT>(rw < 1 ? 1 : rw);
    m_rh = static_cast<UINT>(rh < 1 ? 1 : rh);
}

void Renderer::CreateDepthBuffer(UINT width, UINT height) {
    if (!m_dsvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 3;   // [0] scene depth, [1] shadow map, [2] point-shadow cube depth
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_dsvHeap)));
        NameObject(m_dsvHeap.Get(), L"DsvHeap");
        m_dsvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }

    m_depth.Reset();

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC td{};
    td.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width              = width;
    td.Height             = height;
    td.DepthOrArraySize   = 1;
    td.MipLevels          = 1;
    td.Format             = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count   = 1;
    td.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE cv{};
    cv.Format               = DXGI_FORMAT_D32_FLOAT;
    cv.DepthStencil.Depth   = 1.0f;
    cv.DepthStencil.Stencil = 0;

    ThrowIfFailed(m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
        IID_PPV_ARGS(&m_depth)));
    NameObject(m_depth.Get(), L"DepthBuffer");

    D3D12_DEPTH_STENCIL_VIEW_DESC dvd{};
    dvd.Format        = DXGI_FORMAT_D32_FLOAT;
    dvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_device->CreateDepthStencilView(m_depth.Get(), &dvd,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void Renderer::CreateGBuffer(UINT width, UINT height) {
    m_normalRT.Reset();
    m_albedoRT.Reset();
    m_sceneHDR.Reset();
    m_bloom0.Reset();
    m_bloom1.Reset();
    m_ssaoRT.Reset();
    m_ssaoBlurRT.Reset();
    m_ldrRT.Reset();

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto createColorRT = [&](UINT w, UINT h, DXGI_FORMAT format, const float clear[4],
                             const wchar_t* name) -> ComPtr<ID3D12Resource>
    {
        D3D12_RESOURCE_DESC td{};
        td.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width              = w;
        td.Height             = h;
        td.DepthOrArraySize   = 1;
        td.MipLevels          = 1;
        td.Format             = format;
        td.SampleDesc.Count   = 1;
        td.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.Flags              = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv{};
        cv.Format = format;
        cv.Color[0] = clear[0]; cv.Color[1] = clear[1];
        cv.Color[2] = clear[2]; cv.Color[3] = clear[3];

        ComPtr<ID3D12Resource> rt;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
            IID_PPV_ARGS(&rt)));
        NameObject(rt.Get(), name);
        return rt;
    };

    constexpr float kZero[4]  = { 0.0f, 0.0f, 0.0f, 0.0f };
    constexpr float kBlack[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const UINT hw = (width  + 1) / 2;
    const UINT hh = (height + 1) / 2;
    m_normalRT = createColorRT(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, kZero,  L"GBuffer_Normal");
    m_albedoRT = createColorRT(width, height, DXGI_FORMAT_R8G8B8A8_UNORM,     kZero,  L"GBuffer_Albedo");
    m_sceneHDR   = createColorRT(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, kZero, L"SceneHDR");
    m_bloom0     = createColorRT(hw,    hh,     DXGI_FORMAT_R16G16B16A16_FLOAT, kZero, L"Bloom0");
    m_bloom1     = createColorRT(hw,    hh,     DXGI_FORMAT_R16G16B16A16_FLOAT, kZero, L"Bloom1");
    constexpr float kWhite[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    m_ssaoRT     = createColorRT(width, height, DXGI_FORMAT_R8_UNORM, kWhite, L"SSAO");
    m_ssaoBlurRT = createColorRT(width, height, DXGI_FORMAT_R8_UNORM, kWhite, L"SSAO_Blur");
    // LDR is the tonemap output that the SS scene is box-downsampled INTO, so it (and the FXAA that
    // reads it) stay at the WINDOW resolution regardless of the supersample factor.
    m_ldrRT      = createColorRT(m_width, m_height, DXGI_FORMAT_R8G8B8A8_UNORM, kZero, L"LDR");

    auto rtvAt = [&](UINT index) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(index) * m_rtvSize;
        return h;
    };
    m_device->CreateRenderTargetView(m_normalRT.Get(), nullptr, rtvAt(kGBufferNormalRtvIndex));
    m_device->CreateRenderTargetView(m_albedoRT.Get(), nullptr, rtvAt(kGBufferAlbedoRtvIndex));
    m_device->CreateRenderTargetView(m_sceneHDR.Get(),   nullptr, rtvAt(kSceneHdrRtvIndex));
    m_device->CreateRenderTargetView(m_bloom0.Get(),     nullptr, rtvAt(kBloom0RtvIndex));
    m_device->CreateRenderTargetView(m_bloom1.Get(),     nullptr, rtvAt(kBloom1RtvIndex));
    m_device->CreateRenderTargetView(m_ssaoRT.Get(),     nullptr, rtvAt(kSsaoRtvIndex));
    m_device->CreateRenderTargetView(m_ssaoBlurRT.Get(), nullptr, rtvAt(kSsaoBlurRtvIndex));
    m_device->CreateRenderTargetView(m_ldrRT.Get(),      nullptr, rtvAt(kLdrRtvIndex));
}

void Renderer::RecreateGBufferSrvs() {
    if (!m_gbufferSrvHeap) return;

    D3D12_CPU_DESCRIPTOR_HANDLE h = m_gbufferSrvHeap->GetCPUDescriptorHandleForHeapStart();

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format                    = DXGI_FORMAT_R32_FLOAT;
        sd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip = 0;
        sd.Texture2D.MipLevels       = 1;
        m_device->CreateShaderResourceView(m_depth.Get(), &sd, h);
        h.ptr += m_srvSize;
    }
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format                    = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip = 0;
        sd.Texture2D.MipLevels       = 1;
        m_device->CreateShaderResourceView(m_normalRT.Get(), &sd, h);
        h.ptr += m_srvSize;
    }
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format                    = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip = 0;
        sd.Texture2D.MipLevels       = 1;
        m_device->CreateShaderResourceView(m_albedoRT.Get(), &sd, h);
        h.ptr += m_srvSize;
    }
    // Post-process SRVs: sceneHDR, bloom0, bloom1 (all RGBA16F).
    for (ID3D12Resource* rt : { m_sceneHDR.Get(), m_bloom0.Get(), m_bloom1.Get() }) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format                    = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip = 0;
        sd.Texture2D.MipLevels       = 1;
        m_device->CreateShaderResourceView(rt, &sd, h);
        h.ptr += m_srvSize;
    }
    // Shadow SRV occupies slot kSrvShadow (created in CreateShadowResources); skip it here.
    h.ptr += m_srvSize;
    // SSAO + blurred SSAO (R8_UNORM) at slots kSrvSsao / kSrvSsaoBlur.
    for (ID3D12Resource* rt : { m_ssaoRT.Get(), m_ssaoBlurRT.Get() }) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format                    = DXGI_FORMAT_R8_UNORM;
        sd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels       = 1;
        m_device->CreateShaderResourceView(rt, &sd, h);
        h.ptr += m_srvSize;
    }
    // Noise SRV (slot kSrvNoise) is created once in CreateSsaoResources.

    // LDR tonemap output (FXAA input) at slot kSrvLdr — recreated with the RT on every resize.
    {
        D3D12_CPU_DESCRIPTOR_HANDLE lh = m_gbufferSrvHeap->GetCPUDescriptorHandleForHeapStart();
        lh.ptr += static_cast<SIZE_T>(kSrvLdr) * m_srvSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_ldrRT.Get(), &sd, lh);
    }
}

void Renderer::CreateGeometryPipeline() {
    D3D12_DESCRIPTOR_RANGE1 srvRange{};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = 1;
    srvRange.BaseShaderRegister                = 0; // t0 diffuse
    srvRange.RegisterSpace                     = 0;
    srvRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE1 maskRange = srvRange;
    maskRange.BaseShaderRegister               = 1; // t1 face mask
    D3D12_DESCRIPTOR_RANGE1 normalRange = srvRange;
    normalRange.BaseShaderRegister             = 2; // t2 normal map
    D3D12_DESCRIPTOR_RANGE1 sphAddRange = srvRange;
    sphAddRange.BaseShaderRegister             = 3; // t3 sphere add
    D3D12_DESCRIPTOR_RANGE1 sphMulRange = srvRange;
    sphMulRange.BaseShaderRegister             = 4; // t4 sphere mul

    D3D12_ROOT_PARAMETER1 params[7]{};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace  = 0;
    params[0].Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV; // PerObject (b1)
    params[2].Descriptor.ShaderRegister = 1;
    params[2].Descriptor.RegisterSpace  = 0;
    params[2].Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges   = &maskRange;   // t1 face mask
    params[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    params[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges   = &normalRange; // t2 normal map
    params[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    params[5].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[5].DescriptorTable.NumDescriptorRanges = 1;
    params[5].DescriptorTable.pDescriptorRanges   = &sphAddRange; // t3 sphere add
    params[5].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    params[6].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[6].DescriptorTable.NumDescriptorRanges = 1;
    params[6].DescriptorTable.pDescriptorRanges   = &sphMulRange; // t4 sphere mul
    params[6].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxAnisotropy    = 1;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD           = 0.0f;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;
    sampler.RegisterSpace    = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version                    = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters     = _countof(params);
    desc.Desc_1_1.pParameters       = params;
    desc.Desc_1_1.NumStaticSamplers = 1;
    desc.Desc_1_1.pStaticSamplers   = &sampler;
    desc.Desc_1_1.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &sig, &err);
    if (FAILED(hr)) {
        if (err) std::fprintf(stderr, "[GeomRS] %s\n",
            static_cast<const char*>(err->GetBufferPointer()));
        ThrowIfFailed(hr);
    }
    ThrowIfFailed(m_device->CreateRootSignature(
        0, sig->GetBufferPointer(), sig->GetBufferSize(),
        IID_PPV_ARGS(&m_geometryRS)));
    NameObject(m_geometryRS.Get(), L"GeometryRS");

    fs::path shaderPath = FindShaderFile(L"Geometry.hlsl");
    if (shaderPath.empty()) throw std::runtime_error("Geometry.hlsl not found");
    ComPtr<ID3DBlob> vs = CompileShader(shaderPath, "VSMain", "vs_5_1");
    ComPtr<ID3DBlob> ps = CompileShader(shaderPath, "PSMain", "ps_5_1");

    static const D3D12_INPUT_ELEMENT_DESC kInputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_geometryRS.Get();
    pd.VS             = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS             = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.InputLayout    = { kInputLayout, _countof(kInputLayout) };

    pd.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode              = D3D12_CULL_MODE_BACK;
    pd.RasterizerState.FrontCounterClockwise = FALSE;
    pd.RasterizerState.DepthClipEnable       = TRUE;
    pd.RasterizerState.MultisampleEnable     = FALSE;
    pd.RasterizerState.AntialiasedLineEnable = FALSE;
    pd.RasterizerState.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    pd.BlendState.AlphaToCoverageEnable  = FALSE;
    pd.BlendState.IndependentBlendEnable = FALSE;
    {
        D3D12_RENDER_TARGET_BLEND_DESC rb{};
        rb.SrcBlend              = D3D12_BLEND_ONE;
        rb.DestBlend             = D3D12_BLEND_ZERO;
        rb.BlendOp               = D3D12_BLEND_OP_ADD;
        rb.SrcBlendAlpha         = D3D12_BLEND_ONE;
        rb.DestBlendAlpha        = D3D12_BLEND_ZERO;
        rb.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        rb.LogicOp               = D3D12_LOGIC_OP_NOOP;
        rb.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            pd.BlendState.RenderTarget[i] = rb;
    }

    pd.DepthStencilState.DepthEnable    = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    pd.DepthStencilState.StencilEnable  = FALSE;

    pd.SampleMask            = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets      = 2;
    pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT; // Normal
    pd.RTVFormats[1]         = DXGI_FORMAT_R8G8B8A8_UNORM;     // Albedo
    pd.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count      = 1;

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_geometryPSO)));
    NameObject(m_geometryPSO.Get(), L"GeometryPSO");

    // Variant for MMD characters: every PMX material is flagged bothFace=1 (double-sided),
    // so we honor that with CULL_NONE — the eyelash / eye-decal / cloth planes are meant to
    // render from both sides. LESS_EQUAL lets MMD's later coplanar features win the depth tie.
    pd.RasterizerState.CullMode    = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.FrontCounterClockwise = FALSE;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; // MMD layering: later coplanar features win
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_geometryPSONoCull)));
    NameObject(m_geometryPSONoCull.Get(), L"GeometryPSO_NoCull");

    // Character x-ray depth-reset PSO: depth-only (writes SV_Depth, no colour targets), CULL_NONE,
    // DepthFunc = ALWAYS, depth write on. PSXrayReveal resets depth to far ONLY inside a screen
    // circle around the character (dithered edge), so the normal character draw (LESS_EQUAL) wins
    // over an occluding wall there — revealing the character through it within that region — while
    // still self-occluding and writing its true depth (deferred lighting/shadows stay correct).
    // Outside the window the fragment is discarded, so normal occlusion is preserved. Cheap: one
    // extra depth-only pass over the character, no extra render targets or buffers.
    ComPtr<ID3DBlob> xrayPs        = CompileShader(shaderPath, "PSXrayReveal", "ps_5_1");
    pd.PS                          = { xrayPs->GetBufferPointer(), xrayPs->GetBufferSize() };
    pd.NumRenderTargets            = 0;
    pd.RTVFormats[0]               = DXGI_FORMAT_UNKNOWN;
    pd.RTVFormats[1]               = DXGI_FORMAT_UNKNOWN;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    // (CULL_NONE and DepthWriteMask=ALL are already set from the no-cull variant above.)
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_charXrayDepthPSO)));
    NameObject(m_charXrayDepthPSO.Get(), L"CharXrayDepthPSO");

    // Eff facial-decal forward pass: alpha-blended + emissive over sceneHDR (single RGBA16F RT),
    // depth-tested against the scene (no write) so the decals are semi-transparent (eyes show
    // through) instead of opaquely overwriting the G-buffer.
    // Diffuse sub-pass — alpha-weighted MULTIPLY (DEST_COLOR x src). The decal's dark/coloured
    // texels DARKEN & tint the face by their true colour (青面 dark-green, brows), while the
    // white atlas background leaves the face unchanged — so the effect reads as its real colour
    // and the eyes stay visible (multiply darkens, it doesn't replace).
    ComPtr<ID3DBlob> decalMulPs = CompileShader(shaderPath, "DecalMultiplyPS", "ps_5_1");
    ComPtr<ID3DBlob> decalPs    = CompileShader(shaderPath, "DecalPS", "ps_5_1");
    pd.PS                                = { decalMulPs->GetBufferPointer(), decalMulPs->GetBufferSize() };
    pd.NumRenderTargets                  = 1;
    pd.RTVFormats[0]                     = DXGI_FORMAT_R16G16B16A16_FLOAT; // sceneHDR
    pd.RTVFormats[1]                     = DXGI_FORMAT_UNKNOWN;
    pd.DepthStencilState.DepthWriteMask  = D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.BlendState.RenderTarget[0].BlendEnable    = TRUE;
    pd.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_DEST_COLOR;
    pd.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_decalPSO)));
    NameObject(m_decalPSO.Get(), L"CharDecalPSO");

    // Second sub-pass: the Eff emission texture ADDED on top (ONE/ONE) so the vivid glow colours
    // (blush red, sweat, blue-face, anger vein…) pop over the lit face; the emission's black
    // background adds nothing so the eyes stay visible.
    pd.PS                                        = { decalPs->GetBufferPointer(), decalPs->GetBufferSize() };
    pd.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_decalEmissivePSO)));
    NameObject(m_decalEmissivePSO.Get(), L"CharDecalEmissivePSO");
}

void Renderer::CreateEndfieldPipeline() {
    // Root sig: b0 PerFrame (viewProj/camera/light), b1 EndfieldObject (world + debugMode), single-
    // descriptor tables t0..t4 (BaseColor / Normal / Packed / Mask / Emissive), b2 material, t5 shadow,
    // then t6..t13 for the Endfield "full NPR" maps (ramp/subsurf/lut/reflect/hairdetail/SDF/cm/hl).
    // The RS is SHARED with Wuwa/ZZZ — the new slots are ALWAYS bound (white default) in the draw loop
    // so their draws leave no descriptor table unset; only Endfield.hlsl reads t6..t13.
    constexpr UINT kNumSrv = 14;   // t0..t13
    D3D12_DESCRIPTOR_RANGE1 ranges[kNumSrv]{};
    for (UINT t = 0; t < kNumSrv; ++t) {
        ranges[t].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[t].NumDescriptors                    = 1;
        ranges[t].BaseShaderRegister                = t;   // t0..t13
        ranges[t].RegisterSpace                     = 0;
        ranges[t].Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
        ranges[t].OffsetInDescriptorsFromTableStart = 0;
    }

    D3D12_ROOT_PARAMETER1 params[17]{};   // 3 CBV + t0..t13 (14 tables)
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;   // b0 per-frame
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;   // b1 per-object
    params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    for (UINT t = 0; t < 5; ++t) {
        params[2 + t].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2 + t].DescriptorTable.NumDescriptorRanges = 1;
        params[2 + t].DescriptorTable.pDescriptorRanges   = &ranges[t];
        params[2 + t].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    params[7].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;   // b2 material/look
    params[7].Descriptor.ShaderRegister = 2;
    params[7].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    params[8].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;  // t5 shadow
    params[8].DescriptorTable.NumDescriptorRanges = 1;
    params[8].DescriptorTable.pDescriptorRanges   = &ranges[5];
    params[8].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    for (UINT t = 6; t < kNumSrv; ++t) {   // t6..t13 → params[9..16]
        params[3 + t].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3 + t].DescriptorTable.NumDescriptorRanges = 1;
        params[3 + t].DescriptorTable.pDescriptorRanges   = &ranges[t];
        params[3 + t].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_STATIC_SAMPLER_DESC samplers[3]{};
    samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].MaxAnisotropy    = 1;
    samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    samplers[0].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    samplers[0].MinLOD           = 0.0f;
    samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister   = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // s2: clamp-linear for 1D ramps / LUT / matcap / SDF lookups (no wrap artefact at 0/1 edges).
    samplers[2]                  = samplers[0];
    samplers[2].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].ShaderRegister   = 2;
    // s1: PCF shadow comparison sampler (outside the frustum → white/lit border).
    samplers[1].Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].MaxAnisotropy    = 1;
    samplers[1].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].MinLOD           = 0.0f;
    samplers[1].MaxLOD           = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister   = 1;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version                    = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters     = _countof(params);
    desc.Desc_1_1.pParameters       = params;
    desc.Desc_1_1.NumStaticSamplers = _countof(samplers);
    desc.Desc_1_1.pStaticSamplers   = samplers;
    desc.Desc_1_1.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &sig, &err);
    if (FAILED(hr)) {
        if (err) std::fprintf(stderr, "[EndfieldRS] %s\n", static_cast<const char*>(err->GetBufferPointer()));
        ThrowIfFailed(hr);
    }
    ThrowIfFailed(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                IID_PPV_ARGS(&m_endfieldRS)));
    NameObject(m_endfieldRS.Get(), L"EndfieldRS");

    fs::path sp = FindShaderFile(L"Endfield.hlsl");
    if (sp.empty()) throw std::runtime_error("Endfield.hlsl not found");
    ComPtr<ID3DBlob> vs = CompileShader(sp, "VSMain", "vs_5_1");
    ComPtr<ID3DBlob> ps = CompileShader(sp, "PSMain", "ps_5_1");

    static const D3D12_INPUT_ELEMENT_DESC kLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_endfieldRS.Get();
    pd.VS             = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS             = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.InputLayout    = { kLayout, _countof(kLayout) };
    pd.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;   // MMD meshes are double-sided
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable    = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.DepthStencilState.StencilEnable  = FALSE;
    pd.SampleMask            = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets      = 1;
    pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;   // sceneHDR
    pd.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count      = 1;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_endfieldPSO)));
    NameObject(m_endfieldPSO.Get(), L"EndfieldPSO");

    // Outline variant: back-face expansion — cull FRONT so only the expanded back hull shows as a
    // rim around the silhouette; the main pass (LESS_EQUAL) then draws the model over the interior.
    ComPtr<ID3DBlob> ovs = CompileShader(sp, "VSOutline", "vs_5_1");
    ComPtr<ID3DBlob> ops = CompileShader(sp, "PSOutline", "ps_5_1");
    pd.VS = { ovs->GetBufferPointer(), ovs->GetBufferSize() };
    pd.PS = { ops->GetBufferPointer(), ops->GetBufferSize() };
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_endfieldOutlinePSO)));
    NameObject(m_endfieldOutlinePSO.Get(), L"EndfieldOutlinePSO");

    // Blended-overlay variant: the semi-transparent eye-/hair-shadow meshes (dark diffuse, sub-1
    // alpha, no texture). Alpha-blend over the lit face, depth-test but DON'T write (coplanar with
    // the face), CULL_NONE. Uses the same VS/PS as the opaque pass (transparentMode selects the path).
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    pd.DepthStencilState.DepthFunc     = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.BlendState.RenderTarget[0].BlendEnable    = TRUE;
    pd.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_endfieldBlendPSO)));
    NameObject(m_endfieldBlendPSO.Get(), L"EndfieldBlendPSO");

    // ---- Wuthering Waves PSOs: identical pipeline states + root signature, Wuwa.hlsl shaders.
    // Build the 3 variants (opaque main / outline / blended overlay) from the same `pd` we just
    // configured for the blend PSO, resetting the per-variant state as we go.
    fs::path wsp = FindShaderFile(L"Wuwa.hlsl");
    if (!wsp.empty()) {
        ComPtr<ID3DBlob> wvs  = CompileShader(wsp, "VSMain",    "vs_5_1");
        ComPtr<ID3DBlob> wps  = CompileShader(wsp, "PSMain",    "ps_5_1");
        ComPtr<ID3DBlob> wovs = CompileShader(wsp, "VSOutline", "vs_5_1");
        ComPtr<ID3DBlob> wops = CompileShader(wsp, "PSOutline", "ps_5_1");
        // Opaque main: cull none, depth LESS_EQUAL + write, blend off.
        pd.VS = { wvs->GetBufferPointer(), wvs->GetBufferSize() };
        pd.PS = { wps->GetBufferPointer(), wps->GetBufferSize() };
        pd.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        pd.DepthStencilState.DepthFunc     = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.BlendState.RenderTarget[0].BlendEnable = FALSE;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_wuwaPSO)));
        NameObject(m_wuwaPSO.Get(), L"WuwaPSO");
        // Outline: cull front, depth LESS.
        pd.VS = { wovs->GetBufferPointer(), wovs->GetBufferSize() };
        pd.PS = { wops->GetBufferPointer(), wops->GetBufferSize() };
        pd.RasterizerState.CullMode    = D3D12_CULL_MODE_FRONT;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_wuwaOutlinePSO)));
        NameObject(m_wuwaOutlinePSO.Get(), L"WuwaOutlinePSO");
        // Blended overlays: cull none, depth no-write, alpha blend.
        pd.VS = { wvs->GetBufferPointer(), wvs->GetBufferSize() };
        pd.PS = { wps->GetBufferPointer(), wps->GetBufferSize() };
        pd.RasterizerState.CullMode         = D3D12_CULL_MODE_NONE;
        pd.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.BlendState.RenderTarget[0].BlendEnable    = TRUE;
        pd.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
        pd.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
        pd.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
        pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_wuwaBlendPSO)));
        NameObject(m_wuwaBlendPSO.Get(), L"WuwaBlendPSO");
    }

    // ---- Zenless Zone Zero PSOs (ramp + matcap NPR, Zzz.hlsl) — same 3 variants.
    fs::path zsp = FindShaderFile(L"Zzz.hlsl");
    if (!zsp.empty()) {
        ComPtr<ID3DBlob> zvs  = CompileShader(zsp, "VSMain",    "vs_5_1");
        ComPtr<ID3DBlob> zps  = CompileShader(zsp, "PSMain",    "ps_5_1");
        ComPtr<ID3DBlob> zovs = CompileShader(zsp, "VSOutline", "vs_5_1");
        ComPtr<ID3DBlob> zops = CompileShader(zsp, "PSOutline", "ps_5_1");
        pd.VS = { zvs->GetBufferPointer(), zvs->GetBufferSize() };
        pd.PS = { zps->GetBufferPointer(), zps->GetBufferSize() };
        pd.RasterizerState.CullMode         = D3D12_CULL_MODE_NONE;
        pd.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.BlendState.RenderTarget[0].BlendEnable = FALSE;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_zzzPSO)));
        NameObject(m_zzzPSO.Get(), L"ZzzPSO");
        pd.VS = { zovs->GetBufferPointer(), zovs->GetBufferSize() };
        pd.PS = { zops->GetBufferPointer(), zops->GetBufferSize() };
        pd.RasterizerState.CullMode    = D3D12_CULL_MODE_FRONT;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_zzzOutlinePSO)));
        NameObject(m_zzzOutlinePSO.Get(), L"ZzzOutlinePSO");
        pd.VS = { zvs->GetBufferPointer(), zvs->GetBufferSize() };
        pd.PS = { zps->GetBufferPointer(), zps->GetBufferSize() };
        pd.RasterizerState.CullMode         = D3D12_CULL_MODE_NONE;
        pd.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.BlendState.RenderTarget[0].BlendEnable    = TRUE;
        pd.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
        pd.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
        pd.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
        pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_zzzBlendPSO)));
        NameObject(m_zzzBlendPSO.Get(), L"ZzzBlendPSO");
    }
}

void Renderer::CreateLightingPipeline() {
    D3D12_DESCRIPTOR_RANGE1 srvRange{};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = kLightingSrvCount;
    srvRange.BaseShaderRegister                = 0;
    srvRange.RegisterSpace                     = 0;
    srvRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE1 shadowRange{};
    shadowRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors     = 1;
    shadowRange.BaseShaderRegister = 3; // t3 (shadow map)

    D3D12_DESCRIPTOR_RANGE1 ssaoRange{};
    ssaoRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ssaoRange.NumDescriptors     = 1;
    ssaoRange.BaseShaderRegister = 4; // t4 (blurred SSAO)

    D3D12_ROOT_PARAMETER1 params[8]{};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace  = 0;
    params[0].Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges   = &shadowRange;
    params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges   = &ssaoRange;
    params[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // Forward+: ForwardPlusCB (b1) + point-light buffer (t5) + tile-light list (t6), all as
    // root descriptors (the two StructuredBuffers are bound by GPU address, no heap slots).
    params[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[4].Descriptor.ShaderRegister = 1; // b1
    params[4].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    params[5].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[5].Descriptor.ShaderRegister = 5; // t5 lights
    params[5].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    params[6].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[6].Descriptor.ShaderRegister = 6; // t6 tile lights
    params[6].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE1 cubeRange{};
    cubeRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    cubeRange.NumDescriptors     = 1;
    cubeRange.BaseShaderRegister = 7; // t7 point-shadow cube
    params[7].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[7].DescriptorTable.NumDescriptorRanges = 1;
    params[7].DescriptorTable.pDescriptorRanges   = &cubeRange;
    params[7].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC shadowSamp{};
    shadowSamp.Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSamp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSamp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSamp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSamp.ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSamp.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE; // outside = lit
    shadowSamp.MaxLOD           = D3D12_FLOAT32_MAX;
    shadowSamp.ShaderRegister   = 0; // s0
    shadowSamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC linearSamp{};   // s1: plain linear-clamp for the point cube
    linearSamp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linearSamp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearSamp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearSamp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearSamp.MaxLOD           = D3D12_FLOAT32_MAX;
    linearSamp.ShaderRegister   = 1; // s1
    linearSamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_STATIC_SAMPLER_DESC samplers[] = { shadowSamp, linearSamp };

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version                    = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters     = _countof(params);
    desc.Desc_1_1.pParameters       = params;
    desc.Desc_1_1.NumStaticSamplers = _countof(samplers);
    desc.Desc_1_1.pStaticSamplers   = samplers;
    desc.Desc_1_1.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &sig, &err);
    if (FAILED(hr)) {
        if (err) std::fprintf(stderr, "[LightRS] %s\n",
            static_cast<const char*>(err->GetBufferPointer()));
        ThrowIfFailed(hr);
    }
    ThrowIfFailed(m_device->CreateRootSignature(
        0, sig->GetBufferPointer(), sig->GetBufferSize(),
        IID_PPV_ARGS(&m_lightingRS)));
    NameObject(m_lightingRS.Get(), L"LightingRS");

    fs::path shaderPath = FindShaderFile(L"Lighting.hlsl");
    if (shaderPath.empty()) throw std::runtime_error("Lighting.hlsl not found");
    ComPtr<ID3DBlob> vs = CompileShader(shaderPath, "VSMain", "vs_5_1");
    ComPtr<ID3DBlob> ps = CompileShader(shaderPath, "PSMain", "ps_5_1");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_lightingRS.Get();
    pd.VS             = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS             = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.InputLayout    = { nullptr, 0 };

    pd.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;

    pd.BlendState.AlphaToCoverageEnable  = FALSE;
    pd.BlendState.IndependentBlendEnable = FALSE;
    {
        D3D12_RENDER_TARGET_BLEND_DESC rb{};
        rb.SrcBlend              = D3D12_BLEND_ONE;
        rb.DestBlend             = D3D12_BLEND_ZERO;
        rb.BlendOp               = D3D12_BLEND_OP_ADD;
        rb.SrcBlendAlpha         = D3D12_BLEND_ONE;
        rb.DestBlendAlpha        = D3D12_BLEND_ZERO;
        rb.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        rb.LogicOp               = D3D12_LOGIC_OP_NOOP;
        rb.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            pd.BlendState.RenderTarget[i] = rb;
    }

    pd.DepthStencilState.DepthEnable   = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;

    pd.SampleMask            = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets      = 1;
    pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT; // lighting writes HDR
    pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pd.SampleDesc.Count      = 1;

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_lightingPSO)));
    NameObject(m_lightingPSO.Get(), L"LightingPSO");
}

void Renderer::CreatePostPipeline() {
    // Two single-SRV tables (t0 primary, t1 bloom), a CBV (b0), and a linear-clamp sampler.
    D3D12_DESCRIPTOR_RANGE1 rangeA{};
    rangeA.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rangeA.NumDescriptors     = 1;
    rangeA.BaseShaderRegister = 0; // t0
    D3D12_DESCRIPTOR_RANGE1 rangeB = rangeA;
    rangeB.BaseShaderRegister = 1; // t1

    D3D12_ROOT_PARAMETER1 params[3]{};
    params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges   = &rangeA;
    params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &rangeB;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV; // Post (b0)
    params[2].Descriptor.ShaderRegister = 0;
    params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD           = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version                    = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters     = _countof(params);
    desc.Desc_1_1.pParameters       = params;
    desc.Desc_1_1.NumStaticSamplers = 1;
    desc.Desc_1_1.pStaticSamplers   = &samp;
    desc.Desc_1_1.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &sig, &err);
    if (FAILED(hr)) {
        if (err) std::fprintf(stderr, "[PostRS] %s\n", static_cast<const char*>(err->GetBufferPointer()));
        ThrowIfFailed(hr);
    }
    ThrowIfFailed(m_device->CreateRootSignature(
        0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_postRS)));
    NameObject(m_postRS.Get(), L"PostRS");

    fs::path shaderPath = FindShaderFile(L"PostProcess.hlsl");
    if (shaderPath.empty()) throw std::runtime_error("PostProcess.hlsl not found");
    ComPtr<ID3DBlob> vs      = CompileShader(shaderPath, "VSMain",    "vs_5_1");
    ComPtr<ID3DBlob> psBright = CompileShader(shaderPath, "BrightPS",  "ps_5_1");
    ComPtr<ID3DBlob> psBlur   = CompileShader(shaderPath, "BlurPS",    "ps_5_1");
    ComPtr<ID3DBlob> psTone   = CompileShader(shaderPath, "TonemapPS", "ps_5_1");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_postRS.Get();
    pd.VS             = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.InputLayout    = { nullptr, 0 };
    pd.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    {
        D3D12_RENDER_TARGET_BLEND_DESC rb{};
        rb.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.BlendState.RenderTarget[0] = rb;
    }
    pd.DepthStencilState.DepthEnable   = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.SampleMask            = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets      = 1;
    pd.SampleDesc.Count      = 1;

    // Bright + blur write to the half-res HDR bloom targets.
    pd.PS            = { psBright->GetBufferPointer(), psBright->GetBufferSize() };
    pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_brightPSO)));
    NameObject(m_brightPSO.Get(), L"BrightPSO");

    pd.PS = { psBlur->GetBufferPointer(), psBlur->GetBufferSize() };
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_blurPSO)));
    NameObject(m_blurPSO.Get(), L"BlurPSO");

    // Tonemap writes the LDR back buffer.
    pd.PS            = { psTone->GetBufferPointer(), psTone->GetBufferSize() };
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_tonemapPSO)));
    NameObject(m_tonemapPSO.Get(), L"TonemapPSO");

    // FXAA: edge-softening post pass (LDR image → back buffer). Same RS/format as tonemap.
    ComPtr<ID3DBlob> psFxaa = CompileShader(shaderPath, "FxaaPS", "ps_5_1");
    pd.PS = { psFxaa->GetBufferPointer(), psFxaa->GetBufferSize() };
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_fxaaPSO)));
    NameObject(m_fxaaPSO.Get(), L"FxaaPSO");
}

void Renderer::CreateShadowResources() {
    // ---- Shadow map (D32) + DSV at slot 1 + SRV (R32_FLOAT) at kSrvShadow.
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC td{};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = kShadowMapSize;
    td.Height           = kShadowMapSize;
    td.DepthOrArraySize = 1;
    td.MipLevels        = 1;
    td.Format           = DXGI_FORMAT_R32_TYPELESS;       // typeless: DSV=D32, SRV=R32
    td.SampleDesc.Count = 1;
    td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE cv{};
    cv.Format               = DXGI_FORMAT_D32_FLOAT;
    cv.DepthStencil.Depth   = 1.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, IID_PPV_ARGS(&m_shadowMap)));
    NameObject(m_shadowMap.Get(), L"ShadowMap");

    D3D12_DEPTH_STENCIL_VIEW_DESC dvd{};
    dvd.Format        = DXGI_FORMAT_D32_FLOAT;
    dvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += static_cast<SIZE_T>(m_dsvSize); // slot 1
    m_device->CreateDepthStencilView(m_shadowMap.Get(), &dvd, dsv);

    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format                    = DXGI_FORMAT_R32_FLOAT;
    sd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels       = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE srv = m_gbufferSrvHeap->GetCPUDescriptorHandleForHeapStart();
    srv.ptr += static_cast<SIZE_T>(kSrvShadow) * m_srvSize;
    m_device->CreateShaderResourceView(m_shadowMap.Get(), &sd, srv);

    // ---- Shadow root signature: PerFrame (b0) + PerObject (b1), no SRVs/samplers.
    D3D12_ROOT_PARAMETER1 params[2]{};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version                = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = _countof(params);
    desc.Desc_1_1.pParameters   = params;
    desc.Desc_1_1.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &sig, &err);
    if (FAILED(hr)) {
        if (err) std::fprintf(stderr, "[ShadowRS] %s\n", static_cast<const char*>(err->GetBufferPointer()));
        ThrowIfFailed(hr);
    }
    ThrowIfFailed(m_device->CreateRootSignature(
        0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_shadowRS)));
    NameObject(m_shadowRS.Get(), L"ShadowRS");

    fs::path shaderPath = FindShaderFile(L"Shadow.hlsl");
    if (shaderPath.empty()) throw std::runtime_error("Shadow.hlsl not found");
    ComPtr<ID3DBlob> vs = CompileShader(shaderPath, "VSMain", "vs_5_1");

    static const D3D12_INPUT_ELEMENT_DESC kLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_shadowRS.Get();
    pd.VS             = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.InputLayout    = { kLayout, _countof(kLayout) };
    pd.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode        = D3D12_CULL_MODE_BACK;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.RasterizerState.DepthBias            = 5000;      // constant bias (D32 units)
    pd.RasterizerState.SlopeScaledDepthBias = 2.0f;      // slope-scaled bias vs acne
    pd.RasterizerState.DepthBiasClamp       = 0.0f;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0; // depth only
    pd.DepthStencilState.DepthEnable    = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    pd.SampleMask            = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets      = 0;
    pd.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count      = 1;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_shadowPSO)));
    NameObject(m_shadowPSO.Get(), L"ShadowPSO");
}

void Renderer::CreateSsaoResources() {
    // ---- 4x4 noise tile of random in-plane rotation vectors (RGBA8; z packed to 0.5).
    static const float kRot[16][2] = {
        { 0.97f,-0.26f},{-0.51f, 0.86f},{ 0.28f, 0.96f},{-0.83f,-0.55f},
        { 0.62f, 0.78f},{-0.99f, 0.10f},{ 0.14f,-0.99f},{ 0.71f, 0.71f},
        {-0.31f, 0.95f},{ 0.45f,-0.89f},{-0.67f,-0.74f},{ 0.88f, 0.47f},
        {-0.20f,-0.98f},{ 0.55f, 0.84f},{-0.93f, 0.37f},{ 0.05f, 0.99f},
    };
    uint8_t noisePixels[16 * 4];
    for (int i = 0; i < 16; ++i) {
        noisePixels[i * 4 + 0] = (uint8_t)((kRot[i][0] * 0.5f + 0.5f) * 255.0f);
        noisePixels[i * 4 + 1] = (uint8_t)((kRot[i][1] * 0.5f + 0.5f) * 255.0f);
        noisePixels[i * 4 + 2] = 128;
        noisePixels[i * 4 + 3] = 255;
    }

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = 4;
    td.Height           = 4;
    td.DepthOrArraySize = 1;
    td.MipLevels        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    ThrowIfFailed(m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_ssaoNoise)));
    NameObject(m_ssaoNoise.Get(), L"SSAO_Noise");

    {
        DirectX::ResourceUploadBatch up(m_device.Get());
        up.Begin();
        D3D12_SUBRESOURCE_DATA sd{};
        sd.pData = noisePixels; sd.RowPitch = 4 * 4; sd.SlicePitch = 4 * 4 * 4;
        up.Upload(m_ssaoNoise.Get(), 0, &sd, 1);
        up.Transition(m_ssaoNoise.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        up.End(m_queue.Get()).wait();
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC nsd{};
    nsd.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    nsd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nsd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    nsd.Texture2D.MipLevels     = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE nh = m_gbufferSrvHeap->GetCPUDescriptorHandleForHeapStart();
    nh.ptr += static_cast<SIZE_T>(kSrvNoise) * m_srvSize;
    m_device->CreateShaderResourceView(m_ssaoNoise.Get(), &nsd, nh);

    // ---- Root signature: PerFrame (b0), SsaoCB (b1), and three single-SRV tables (t0,
    //      t1, t2) — kept separate so a table range never overlaps the current RTV.
    D3D12_DESCRIPTOR_RANGE1 r0{}, r1{}, r2{};
    r0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r0.NumDescriptors = 1; r0.BaseShaderRegister = 0;
    r1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r1.NumDescriptors = 1; r1.BaseShaderRegister = 1;
    r2.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r2.NumDescriptors = 1; r2.BaseShaderRegister = 2;

    D3D12_ROOT_PARAMETER1 params[5]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1; params[2].DescriptorTable.pDescriptorRanges = &r0;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1; params[3].DescriptorTable.pDescriptorRanges = &r1;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1; params[4].DescriptorTable.pDescriptorRanges = &r2;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = _countof(params);
    desc.Desc_1_1.pParameters   = params;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &sig, &err);
    if (FAILED(hr)) {
        if (err) std::fprintf(stderr, "[SsaoRS] %s\n", static_cast<const char*>(err->GetBufferPointer()));
        ThrowIfFailed(hr);
    }
    ThrowIfFailed(m_device->CreateRootSignature(
        0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_ssaoRS)));
    NameObject(m_ssaoRS.Get(), L"SsaoRS");

    fs::path shaderPath = FindShaderFile(L"Ssao.hlsl");
    if (shaderPath.empty()) throw std::runtime_error("Ssao.hlsl not found");
    ComPtr<ID3DBlob> vs   = CompileShader(shaderPath, "VSMain", "vs_5_1");
    ComPtr<ID3DBlob> psAo = CompileShader(shaderPath, "SsaoPS", "ps_5_1");
    ComPtr<ID3DBlob> psBl = CompileShader(shaderPath, "BlurPS", "ps_5_1");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_ssaoRS.Get();
    pd.VS             = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.InputLayout    = { nullptr, 0 };
    pd.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable   = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.SampleMask            = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets      = 1;
    pd.RTVFormats[0]         = DXGI_FORMAT_R8_UNORM;
    pd.SampleDesc.Count      = 1;

    pd.PS = { psAo->GetBufferPointer(), psAo->GetBufferSize() };
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_ssaoPSO)));
    NameObject(m_ssaoPSO.Get(), L"SsaoPSO");
    pd.PS = { psBl->GetBufferPointer(), psBl->GetBufferSize() };
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_ssaoBlurPSO)));
    NameObject(m_ssaoBlurPSO.Get(), L"SsaoBlurPSO");
}

// ============================== Forward+ tiled point lights ==============================

void Renderer::GeneratePointLights() {
    m_pointLights.resize(kNumPointLights);
    m_lightHomePos.resize(kNumPointLights);

    std::mt19937 rng(1337);                            // fixed seed → deterministic layout
    std::uniform_real_distribution<float> ux(-1650.0f, 1650.0f);
    std::uniform_real_distribution<float> uy(   50.0f,  760.0f);
    std::uniform_real_distribution<float> uz( -480.0f,  480.0f);
    std::uniform_real_distribution<float> uh(    0.0f,    1.0f);
    std::uniform_real_distribution<float> ur(  220.0f,  430.0f);

    // Fully-saturated bright colour from a hue in [0,1].
    auto hueRGB = [](float h) -> XMFLOAT3 {
        float r = std::min(std::max(std::fabs(h * 6.0f - 3.0f) - 1.0f, 0.0f), 1.0f);
        float g = std::min(std::max(2.0f - std::fabs(h * 6.0f - 2.0f), 0.0f), 1.0f);
        float b = std::min(std::max(2.0f - std::fabs(h * 6.0f - 4.0f), 0.0f), 1.0f);
        return { r, g, b };
    };
    for (UINT i = 0; i < kNumPointLights; ++i) {
        XMFLOAT3 p{ ux(rng), uy(rng), uz(rng) };
        m_lightHomePos[i]         = p;
        m_pointLights[i].posWS    = p;
        m_pointLights[i].radius   = ur(rng);
        m_pointLights[i].color    = hueRGB(uh(rng));
        m_pointLights[i].intensity = 1.4f;
    }
    std::printf("[Forward+] %u point lights generated (tile %ux%u, max %u/tile)\n",
                kNumPointLights, kTileSize, kTileSize, kMaxLightsPerTile);
    std::fflush(stdout);
}

// Reorder so the kNumShadowedLights lights nearest the character sit at indices 0..N-1
// (those are the static, cube-shadowed ones). Called whenever the character is placed/moved.
void Renderer::SelectShadowedLights() {
    if (m_pointLights.size() <= kNumShadowedLights) return;
    // Aim at the character's mid-height, not its feet.
    const XMVECTOR c = XMVectorSet(m_charPos.x, m_charPos.y + 150.0f, m_charPos.z, 0.0f);
    for (UINT slot = 0; slot < kNumShadowedLights; ++slot) {
        UINT  best = slot; float bestD = 1e30f;
        for (UINT i = slot; i < static_cast<UINT>(m_pointLights.size()); ++i) {
            const XMVECTOR p = XMLoadFloat3(&m_lightHomePos[i]);
            const float d = XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(p, c)));
            if (d < bestD) { bestD = d; best = i; }
        }
        if (best != slot) {
            std::swap(m_pointLights[slot], m_pointLights[best]);
            std::swap(m_lightHomePos[slot], m_lightHomePos[best]);
        }
    }
    std::printf("[Forward+] shadowing the %u lights nearest the character (%.0f,%.0f,%.0f):\n",
                kNumShadowedLights, m_charPos.x, m_charPos.y, m_charPos.z);
    for (UINT i = 0; i < kNumShadowedLights; ++i)
        std::printf("[Forward+]   light %u at (%.0f, %.0f, %.0f) r=%.0f\n", i,
                    m_pointLights[i].posWS.x, m_pointLights[i].posWS.y,
                    m_pointLights[i].posWS.z, m_pointLights[i].radius);
    std::fflush(stdout);
    m_pointShadowDirty = true;   // shadow set changed → re-bake
}

void Renderer::CreateLightCullPipeline() {
    // Root signature: CBV(b0) + SRV(t0 lights) + UAV(u0 tile lists) as root descriptors
    // (buffers bound by GPU address), plus a descriptor table for the depth texture (t1,
    // read for per-tile depth bounds) which must come from a shader-visible heap.
    D3D12_DESCRIPTOR_RANGE1 depthRange{};
    depthRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    depthRange.NumDescriptors     = 1;
    depthRange.BaseShaderRegister = 1; // t1

    D3D12_ROOT_PARAMETER1 p[4]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor.ShaderRegister = 0;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[1].Descriptor.ShaderRegister = 0;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[2].Descriptor.ShaderRegister = 0;
    p[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[3].DescriptorTable.NumDescriptorRanges = 1;
    p[3].DescriptorTable.pDescriptorRanges   = &depthRange;
    for (auto& pp : p) pp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = _countof(p);
    desc.Desc_1_1.pParameters   = p;
    desc.Desc_1_1.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &sig, &err);
    if (FAILED(hr)) {
        if (err) std::fprintf(stderr, "[LightCullRS] %s\n", static_cast<const char*>(err->GetBufferPointer()));
        ThrowIfFailed(hr);
    }
    ThrowIfFailed(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        IID_PPV_ARGS(&m_lightCullRS)));
    NameObject(m_lightCullRS.Get(), L"LightCullRS");

    fs::path shaderPath = FindShaderFile(L"LightCulling.hlsl");
    if (shaderPath.empty()) throw std::runtime_error("LightCulling.hlsl not found");
    ComPtr<ID3DBlob> cs = CompileShader(shaderPath, "CSMain", "cs_5_1");

    D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
    cpd.pRootSignature = m_lightCullRS.Get();
    cpd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    ThrowIfFailed(m_device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&m_lightCullPSO)));
    NameObject(m_lightCullPSO.Get(), L"LightCullPSO");
}

void Renderer::CreateTileLightBuffer(UINT width, UINT height) {
    m_tileCount.x = (width  + kTileSize - 1) / kTileSize;
    m_tileCount.y = (height + kTileSize - 1) / kTileSize;
    const UINT64 numTiles = static_cast<UINT64>(m_tileCount.x) * m_tileCount.y;
    const UINT64 bytes    = numTiles * (kMaxLightsPerTile + 1) * sizeof(uint32_t);

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = bytes;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    m_tileLightBuffer.Reset();
    // Buffers are always created in COMMON (the requested state is ignored, with a debug
    // warning); the per-frame UAV/SRV barriers promote it from there on first use.
    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_tileLightBuffer)));
    NameObject(m_tileLightBuffer.Get(), L"TileLightBuffer");
}

// ============================== Point-light cube shadow + bloom sphere ==============================

void Renderer::CreatePointShadowResources() {
    const UINT S = kPointShadowSize;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    // ---- R32_FLOAT cube ARRAY (6 faces × N shadowed lights) storing normalised light distance.
    const UINT kSlices = 6u * kNumShadowedLights;
    D3D12_RESOURCE_DESC td{};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = S; td.Height = S;
    td.DepthOrArraySize = static_cast<UINT16>(kSlices);
    td.MipLevels        = 1;
    td.Format           = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE ccv{}; ccv.Format = DXGI_FORMAT_R32_FLOAT; ccv.Color[0] = 1.0f;
    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ccv, IID_PPV_ARGS(&m_pointCube)));
    NameObject(m_pointCube.Get(), L"PointShadowCubeArray");

    // 6*N face RTVs (one Texture2DArray slice each).
    D3D12_DESCRIPTOR_HEAP_DESC rhd{};
    rhd.NumDescriptors = kSlices; rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&m_pointRtvHeap)));
    NameObject(m_pointRtvHeap.Get(), L"PointShadowRtvHeap");
    D3D12_CPU_DESCRIPTOR_HANDLE rh = m_pointRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT s = 0; s < kSlices; ++s) {
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format                         = DXGI_FORMAT_R32_FLOAT;
        rv.ViewDimension                  = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rv.Texture2DArray.FirstArraySlice = s;
        rv.Texture2DArray.ArraySize       = 1;
        m_device->CreateRenderTargetView(m_pointCube.Get(), &rv, rh);
        rh.ptr += m_rtvSize;
    }

    // TextureCubeArray SRV at kSrvPointCube (N cubes).
    D3D12_SHADER_RESOURCE_VIEW_DESC cs{};
    cs.Format                          = DXGI_FORMAT_R32_FLOAT;
    cs.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    cs.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    cs.TextureCubeArray.MipLevels      = 1;
    cs.TextureCubeArray.NumCubes       = kNumShadowedLights;
    cs.TextureCubeArray.First2DArrayFace = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE csh = m_gbufferSrvHeap->GetCPUDescriptorHandleForHeapStart();
    csh.ptr += static_cast<SIZE_T>(kSrvPointCube) * m_srvSize;
    m_device->CreateShaderResourceView(m_pointCube.Get(), &cs, csh);

    // ---- D32 depth (reused per face) + DSV at slot 2.
    D3D12_RESOURCE_DESC dd = td;
    dd.DepthOrArraySize = 1;
    dd.Format           = DXGI_FORMAT_R32_TYPELESS;
    dd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE dcv{}; dcv.Format = DXGI_FORMAT_D32_FLOAT; dcv.DepthStencil.Depth = 1.0f;
    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv, IID_PPV_ARGS(&m_pointCubeDepth)));
    NameObject(m_pointCubeDepth.Get(), L"PointShadowDepth");
    D3D12_DEPTH_STENCIL_VIEW_DESC dvd{};
    dvd.Format = DXGI_FORMAT_D32_FLOAT; dvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE pdsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    pdsv.ptr += static_cast<SIZE_T>(2) * m_dsvSize; // slot 2
    m_device->CreateDepthStencilView(m_pointCubeDepth.Get(), &dvd, pdsv);

    // ---- RS: PointShadowCB (b0) + PerObject (b1).
    D3D12_ROOT_PARAMETER1 p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor.ShaderRegister = 0;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[1].Descriptor.ShaderRegister = 1;
    for (auto& pp : p) pp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = _countof(p);
    desc.Desc_1_1.pParameters   = p;
    desc.Desc_1_1.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &sig, &err);
    if (FAILED(hr)) { if (err) std::fprintf(stderr, "[PointShadowRS] %s\n", (char*)err->GetBufferPointer()); ThrowIfFailed(hr); }
    ThrowIfFailed(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        IID_PPV_ARGS(&m_pointShadowRS)));
    NameObject(m_pointShadowRS.Get(), L"PointShadowRS");

    fs::path sp = FindShaderFile(L"PointShadow.hlsl");
    if (sp.empty()) throw std::runtime_error("PointShadow.hlsl not found");
    ComPtr<ID3DBlob> vs = CompileShader(sp, "VSMain", "vs_5_1");
    ComPtr<ID3DBlob> ps = CompileShader(sp, "PSMain", "ps_5_1");
    static const D3D12_INPUT_ELEMENT_DESC kLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature  = m_pointShadowRS.Get();
    pd.VS              = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS              = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.InputLayout     = { kLayout, _countof(kLayout) };
    pd.RasterizerState.FillMode             = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode             = D3D12_CULL_MODE_BACK;
    pd.RasterizerState.DepthClipEnable      = TRUE;
    pd.RasterizerState.DepthBias            = 1000;
    pd.RasterizerState.SlopeScaledDepthBias = 2.0f;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable    = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    pd.SampleMask            = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets      = 1;
    pd.RTVFormats[0]         = DXGI_FORMAT_R32_FLOAT;
    pd.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count      = 1;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_pointShadowPSO)));
    NameObject(m_pointShadowPSO.Get(), L"PointShadowPSO");
}

void Renderer::CreateBloomSphere() {
    // Unit UV-sphere (positions only).
    const int stacks = 16, slices = 24;
    std::vector<XMFLOAT3> verts;
    std::vector<uint16_t> idx;
    for (int i = 0; i <= stacks; ++i) {
        float v = static_cast<float>(i) / stacks;
        float phi = v * 3.14159265f;
        for (int j = 0; j <= slices; ++j) {
            float u = static_cast<float>(j) / slices;
            float theta = u * 2.0f * 3.14159265f;
            verts.push_back({ std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta) });
        }
    }
    const int ring = slices + 1;
    for (int i = 0; i < stacks; ++i)
        for (int j = 0; j < slices; ++j) {
            uint16_t a = static_cast<uint16_t>(i * ring + j),     b = static_cast<uint16_t>(a + 1);
            uint16_t c = static_cast<uint16_t>((i + 1) * ring + j), d = static_cast<uint16_t>(c + 1);
            idx.insert(idx.end(), { a, c, b, b, c, d });
        }
    m_sphereIndexCount = static_cast<UINT>(idx.size());

    auto makeUpload = [&](const void* data, UINT64 bytes, ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES uh{}; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = bytes; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(m_device->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out)));
        void* p = nullptr; D3D12_RANGE rr{ 0, 0 };
        ThrowIfFailed(out->Map(0, &rr, &p));
        std::memcpy(p, data, bytes);
        out->Unmap(0, nullptr);
    };
    const UINT64 vbBytes = verts.size() * sizeof(XMFLOAT3);
    const UINT64 ibBytes = idx.size()  * sizeof(uint16_t);
    makeUpload(verts.data(), vbBytes, m_sphereVB);
    makeUpload(idx.data(),   ibBytes, m_sphereIB);
    m_sphereVBV = { m_sphereVB->GetGPUVirtualAddress(), (UINT)vbBytes, sizeof(XMFLOAT3) };
    m_sphereIBV = { m_sphereIB->GetGPUVirtualAddress(), (UINT)ibBytes, DXGI_FORMAT_R16_UINT };

    // RS: SphereCB (b0) + StructuredBuffer<PointLight> (t0, by GPU VA, for instancing).
    D3D12_ROOT_PARAMETER1 p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[0].Descriptor.ShaderRegister = 0; p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[1].Descriptor.ShaderRegister = 0; p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = 2; desc.Desc_1_1.pParameters = p;
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeVersionedRootSignature(&desc, &sig, &err));
    ThrowIfFailed(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        IID_PPV_ARGS(&m_sphereRS)));
    NameObject(m_sphereRS.Get(), L"BloomSphereRS");

    fs::path sp = FindShaderFile(L"BloomSphere.hlsl");
    if (sp.empty()) throw std::runtime_error("BloomSphere.hlsl not found");
    ComPtr<ID3DBlob> vs = CompileShader(sp, "VSMain", "vs_5_1");
    ComPtr<ID3DBlob> ps = CompileShader(sp, "PSMain", "ps_5_1");
    static const D3D12_INPUT_ELEMENT_DESC kLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature  = m_sphereRS.Get();
    pd.VS              = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS              = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.InputLayout     = { kLayout, _countof(kLayout) };
    pd.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE; // marker sphere — winding-agnostic
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable    = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // test only, no write
    pd.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.SampleMask            = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets      = 1;
    pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT; // sceneHDR
    pd.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count      = 1;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_spherePSO)));
    NameObject(m_spherePSO.Get(), L"BloomSpherePSO");
}

void Renderer::InitImGui(HWND hwnd) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 64;
    hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_imguiSrvHeap)));
    NameObject(m_imguiSrvHeap.Get(), L"ImGuiSrvHeap");
    g_imguiAlloc.Create(m_device.Get(), m_imguiSrvHeap.Get(), 64);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    // Load a Japanese system font so the MMD morph names (あ/まばたき/…) render instead of
    // tofu boxes. The first font added becomes the default; these all include Latin glyphs.
    {
        ImGuiIO& io = ImGui::GetIO();
        const char* jpFonts[] = {
            "C:\\Windows\\Fonts\\meiryo.ttc",
            "C:\\Windows\\Fonts\\YuGothR.ttc",
            "C:\\Windows\\Fonts\\msgothic.ttc",
        };
        bool haveFont = false;
        for (const char* fp : jpFonts) {
            std::error_code ec;
            if (fs::exists(fp, ec)) {
                io.Fonts->AddFontFromFileTTF(fp, 16.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
                haveFont = true;
                break;
            }
        }

        // Merge a Chinese font on top: the asset folders (and therefore the Character / Dance combo
        // entries) mix Simplified (芙芙摇) and Traditional (最喜歡, 終末地) names, which the Japanese
        // range doesn't cover — those characters used to draw as '?'.
        const char* cnFonts[] = {
            "C:\\Windows\\Fonts\\msyh.ttc",    // Microsoft YaHei    (GB18030: simplified + traditional)
            "C:\\Windows\\Fonts\\msjh.ttc",    // Microsoft JhengHei (traditional)
        };
        for (const char* fp : cnFonts) {
            std::error_code ec;
            if (!fs::exists(fp, ec)) continue;
            ImFontConfig cfg;
            cfg.MergeMode = haveFont;         // merge into the Japanese font, or stand alone
            io.Fonts->AddFontFromFileTTF(fp, 16.0f, &cfg, io.Fonts->GetGlyphRangesChineseFull());
            break;
        }
    }

    ImGui_ImplWin32_Init(hwnd);

    ImGui_ImplDX12_InitInfo info{};
    info.Device               = m_device.Get();
    info.CommandQueue         = m_queue.Get();
    info.NumFramesInFlight    = kFrameCount;
    info.RTVFormat            = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.SrvDescriptorHeap    = m_imguiSrvHeap.Get();
    info.SrvDescriptorAllocFn = ImGuiAllocFn;
    info.SrvDescriptorFreeFn  = ImGuiFreeFn;
    ImGui_ImplDX12_Init(&info);
    m_imguiReady = true;
}

void Renderer::BuildImGuiUI() {
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(310, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Controls");

    // Frame rate — the project budget is "stable 45 fps at 1280x720 with everything on",
    // so colour it green at/above 45 and red below to make the requirement easy to check.
    const float fps = ImGui::GetIO().Framerate;
    const ImVec4 fpsCol = (fps >= 45.0f) ? ImVec4(0.40f, 0.85f, 0.40f, 1.0f)
                                         : ImVec4(0.90f, 0.35f, 0.35f, 1.0f);
    ImGui::TextColored(fpsCol, "FPS: %.1f  (%.2f ms)", fps, fps > 0.0f ? 1000.0f / fps : 0.0f);
    ImGui::Separator();

    if (HasAnimation()) {
        ImGui::SeparatorText("Playback");
        if (ImGui::Button(m_animPaused ? " Play "  : " Pause ")) TogglePause();
        ImGui::SameLine();
        if (ImGui::Button(" Replay ")) Replay();   // restart motion + music from the top
        ImGui::SameLine();
        ImGui::TextDisabled("Space = pause");

        // Selectable dance clips. Switching never disturbs the default dance + bgm.wav: a clip
        // with its own BGM plays it synced, a music-less clip stops the audio and loops itself.
        if (MotionClipCount() > 1) {
            std::vector<const char*> names;
            names.reserve(m_motionClips.size());
            for (const auto& c : m_motionClips) names.push_back(c.name.c_str());
            int cur = m_currentClip;
            if (ImGui::Combo("Dance", &cur, names.data(), static_cast<int>(names.size())))
                SelectMotion(cur);
        }
        // Dance speed relative to the music (1 = the auto-fit that fills the song; lower = slower).
        float ds = static_cast<float>(m_animSpeed.load());
        if (ImGui::SliderFloat("Dance speed", &ds, 0.3f, 1.5f)) m_animSpeed.store(static_cast<double>(ds));
        // Cloth/hair physics — the big per-frame cost; turn off for a smooth (esp. Debug) dance.
        bool phys = m_physicsOn;
        if (ImGui::Checkbox("Cloth/hair physics", &phys)) SetPhysics(phys);
        ImGui::SameLine(); ImGui::TextDisabled("(off = much faster)");
    } else {
        ImGui::TextDisabled("No animation loaded");
    }

    ImGui::SeparatorText("Ambient Occlusion");
    ImGui::Checkbox("SSAO", &m_ssaoEnabled);
    ImGui::SliderFloat("Radius",    &m_ssaoRadius,    5.0f, 150.0f);
    ImGui::SliderFloat("Intensity", &m_ssaoIntensity, 0.0f, 3.0f);

    ImGui::SeparatorText("Scene lights");
    if (ImGui::Button(" All ON "))  SetAllLights(true);
    ImGui::SameLine();
    if (ImGui::Button(" All OFF ")) SetAllLights(false);
    ImGui::Checkbox("Directional (sun)", &m_dirLightOn);
    ImGui::Checkbox("Coloured point lights (128)", &m_forwardPlus);
    ImGui::Checkbox("Point-light cube shadows", &m_pointShadowOn);
    ImGui::SameLine(); ImGui::TextDisabled("(first %u lights)", kNumShadowedLights);

    ImGui::SeparatorText("Post / Light");
    ImGui::Checkbox("Bloom", &m_bloomEnabled);
    ImGui::SameLine(); ImGui::Checkbox("FXAA", &m_fxaa);
    if (m_fxaa) ImGui::SliderFloat("AA strength", &m_fxaaStrength, 0.0f, 1.0f);
    { // Supersample AA — the real fix for outline / toon-break shimmer (rebuilds RTs on change).
        float ssaaUi = m_ssaa;
        if (ImGui::SliderFloat("SSAA", &ssaaUi, 1.0f, 2.0f, "%.2fx")) RequestSsaa(ssaaUi);
        ImGui::SameLine(); ImGui::TextDisabled("(1=off, 2=4x pixels)");
    }
    ImGui::SliderFloat("Exposure", &m_exposure, 0.2f, 3.0f);
    ImGui::SliderFloat("Vibrance",  &m_vibrance, 1.0f, 3.0f);
    if (ImGui::SliderFloat3("Light dir", &m_lightDir.x, -1.0f, 1.0f)) { /* used next frame */ }

    ImGui::SeparatorText("Forward+ (coloured lights)");
    ImGui::Checkbox("Tile heatmap", &m_fpDebugHeat);
    ImGui::Text("%u lights  |  %ux%u tiles  |  max %u/tile",
                kNumPointLights, m_tileCount.x, m_tileCount.y, kMaxLightsPerTile);
    ImGui::TextDisabled("%u of them cast cube shadows", kNumShadowedLights);

    ImGui::SeparatorText("Character");
    if (CharacterCount() > 1) {
        std::vector<const char*> names;
        names.reserve(m_characters.size());
        for (const auto& c : m_characters) names.push_back(c.name.c_str());
        int cur = m_currentChar;
        if (ImGui::Combo("Character", &cur, names.data(), static_cast<int>(names.size())))
            SelectCharacter(cur);
    }
    ImGui::Checkbox("Normal maps (_N, PBR models)", &m_charNormalMap);
    {   // Render-profile override (folder decides it on load; this lets you switch live).
        int prof = static_cast<int>(m_mmd.profile);
        if (ImGui::Combo("Render profile", &prof, "Cel (deferred)\0Endfield PBR\0Wuwa PBR\0Zzz NPR\0\0")) {
            m_mmd.profile = static_cast<RenderProfile>(prof);
            ApplyProfileChannelDefaults();   // reset _P metal/rough channels for the new profile
        }
    }
    if (CharIsForwardPBR()) {
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s (forward)", RenderProfileName(m_mmd.profile));
        static const char* kDbg[] = { "Toon", "BaseColor", "Normal map", "Packed.R",
                                      "Packed.G", "Packed.B", "Packed.A", "Mask", "Emissive" };
        ImGui::Combo("Endfield debug", &m_endfieldDebug, kDbg, IM_ARRAYSIZE(kDbg));
        ImGui::SliderFloat("Outline px",  &m_endfieldOutline,     0.0f, 5.0f);
        ImGui::SliderFloat("Outline ref",  &m_outlineRefFrac,   0.15f, 1.5f);
        ImGui::SameLine(); ImGui::TextDisabled("(higher→thinner; scales w/ char size)");
        ImGui::SliderFloat("Outline depth", &m_outlineDepthBias, 0.0f, 80.0f);
        ImGui::SameLine(); ImGui::TextDisabled("(hide line at self-overlaps)");
        ImGui::SliderFloat("Toon thresh", &m_endfieldToonThresh,  0.1f, 0.9f);
        ImGui::SliderFloat("Toon feather",&m_endfieldToonFeather, 0.005f, 0.3f);
        if (m_mmd.profile == RenderProfile::ZzzNPR) {
            ImGui::SliderFloat("Metal (matcap)", &m_zzzMatcap, 0.0f, 5.0f);
            ImGui::SliderFloat("Saturation",     &m_zzzSat,    1.0f, 2.5f);
            ImGui::SameLine(); ImGui::TextDisabled("(1.0 = faithful to texture)");
            ImGui::SliderFloat("Texture fidelity", &m_zzzTexFidelity, 0.0f, 1.0f);
            ImGui::SameLine(); ImGui::TextDisabled("(1 = undo post → texture colour)");
            ImGui::SliderFloat("Deepen",   &m_zzzDeepen,  0.0f, 1.0f);
            ImGui::SliderFloat("Warmth",   &m_zzzWarmth,  0.0f, 1.0f);
            ImGui::SameLine(); ImGui::TextDisabled("(yellow→orange)");
            ImGui::SliderFloat("Eye shadow lift", &m_zzzEyeLift, 0.0f, 1.0f);
        }
        if (m_mmd.profile == RenderProfile::WuwaPBR) {
            ImGui::SliderFloat("Exposure", &m_wuwaExposure, 0.3f, 1.2f);
            ImGui::SameLine(); ImGui::TextDisabled("(<1 = dimmer character)");
            ImGui::SliderFloat("Shadow tint", &m_wuwaShadowTint, 0.0f, 1.0f);
            ImGui::SameLine(); ImGui::TextDisabled("(0 = neutral grey, 1 = cold blue)");
            ImGui::SliderFloat("Texture fidelity", &m_wuwaFidelity, 0.0f, 1.0f);
            ImGui::SameLine(); ImGui::TextDisabled("(undo post → stop white wash)");
        }
        ImGui::Checkbox("Normal detail (_N)", &m_efNormalMap);
        ImGui::SameLine(); ImGui::Checkbox("Flip N.Y", &m_efFlipNormalY);
        ImGui::SliderFloat("Spec strength", &m_efSpec,      0.0f, 1.5f);
        ImGui::SliderFloat("Rough bias",    &m_efRoughBias, -0.5f, 0.5f);
        ImGui::Combo("Metal chan", &m_efMetalChan, "R\0G\0B\0A\0\0");
        ImGui::SameLine(); ImGui::Combo("Rough chan", &m_efRoughChan, "R\0G\0B\0A\0\0");
        bool inv = m_efInvertRough != 0;
        if (ImGui::Checkbox("Invert rough (smoothness)", &inv)) m_efInvertRough = inv ? 1 : 0;
        ImGui::SliderFloat("Shadow recv",  &m_efShadowStr,   0.0f, 1.0f);
        ImGui::SliderFloat("Shadow depth", &m_efShadowDepth, 0.2f, 1.0f);
        ImGui::SameLine(); ImGui::TextDisabled("(lower=darker)");
        ImGui::SliderFloat("Rim strength", &m_efRim,    0.0f, 1.0f);
        ImGui::SliderFloat("Rim power",    &m_efRimPow, 1.0f, 8.0f);
        ImGui::SliderFloat("Hair ring",    &m_efHair,   0.0f, 2.0f);
        ImGui::SliderFloat("Hair range",   &m_efHairRange, 4.0f, 120.0f);
        ImGui::SameLine(); ImGui::TextDisabled("(higher = narrower band, less oily)");
        ImGui::SliderFloat("Emissive",     &m_efEmiss,  0.0f, 4.0f);
        // Endfield-only look tools (leather sheen / fidelity / spec focus / char tone). Kept OUT of
        // the ZZZ/Wuwa paths so they don't leak into those profiles (they render with their own look).
        if (m_mmd.profile == RenderProfile::EndfieldPBR) {
            ImGui::SliderFloat("Texture fidelity", &m_efFidelity, 0.0f, 1.0f);
            ImGui::SameLine(); ImGui::TextDisabled("(undo post → texture colour; fixes 發白)");
            ImGui::SliderFloat("Leather sheen", &m_efSheen, 0.0f, 2.0f);
            ImGui::SameLine(); ImGui::TextDisabled("(cloth/body matcap sheen → latex look)");
            ImGui::SliderFloat("Spec focus",   &m_efSpecFocus, 0.0f, 1.0f);
            ImGui::SameLine(); ImGui::TextDisabled("(concentrate highlight)");
            ImGui::SliderFloat("Highlights",   &m_highlights, -1.0f, 1.0f);
            ImGui::SameLine(); ImGui::TextDisabled("(- recover, char only)");
            ImGui::SliderFloat("Shadows",      &m_shadows,    -1.0f, 1.0f);
            ImGui::SameLine(); ImGui::TextDisabled("(+ lift, char only)");
        }
    }
    ImGui::Checkbox("X-ray reveal through walls", &m_charXray);
    if (m_charXray) {
        ImGui::SliderFloat("X-ray window",   &m_xrayRadiusScale, 1.0f, 4.0f);
        ImGui::SliderFloat("X-ray opacity",  &m_xrayStrength,    0.2f, 1.0f);
    }
    // Cel/deferred-path look controls — they only drive the deferred cel shader (Geometry/Lighting
    // ShadeCel), so hide them for forward-PBR characters (Endfield/Wuwa/ZZZ) where they do nothing.
    if (!CharIsForwardPBR()) {
        ImGui::SliderFloat("Saturation",   &m_charSat,            1.0f, 6.0f);
        ImGui::SliderFloat("Contrast",     &m_charContrast,       0.5f, 2.5f);
        ImGui::SliderFloat("Outline dark", &m_outlineDarken,      0.1f, 1.0f);
        ImGui::SameLine(); ImGui::TextDisabled("(lower = darker; 1 = none)");
        ImGui::SliderFloat("SSS strength", &m_sssStrength, 0.0f, 2.0f);
        ImGui::SameLine(); ImGui::TextDisabled("(red, skin only)");
        ImGui::SliderFloat("SSS wrap",     &m_sssWrap,     0.0f, 1.0f);
        ImGui::SliderFloat("Specular",     &m_specInt,     0.0f, 4.0f);
        ImGui::SliderFloat("Spec power",   &m_specPow,     1.0f, 128.0f);
        ImGui::SliderFloat("Skin highlight", &m_skinFresnel, 0.0f, 2.0f);
        ImGui::SameLine(); ImGui::TextDisabled("(view/normal sheen)");
    }

    // Quick whole-loop expression + iris switches (console `expr`/`eye` do frame windows).
    if (HasAnimation() && MmdMorphCount() > 0) {
        // Friendly names → the model's morph names (resolved by name so it's model-agnostic).
        static const char* kExprLabel[] = { "None", "Blink", "Smile", "Half-lid", "Wink", "Surprise" };
        static const char* kExprMorph[] = { "",     u8"まばたき", u8"笑い",
                                             u8"じと目", u8"ウィンク",
                                             u8"びっくり" };
        if (ImGui::Combo("Expression", &m_guiExpr, kExprLabel, IM_ARRAYSIZE(kExprLabel))) {
            ClearExprWindows();
            if (m_guiExpr > 0) {
                const int idx = FindMmdMorph(kExprMorph[m_guiExpr]);
                if (idx >= 0) AddExprWindow(idx, 0, 1 << 30, 1.0f);
            }
        }
    }
    if (m_mmd.eyeSwapAvailable) {
        if (ImGui::Combo("Iris", &m_guiIris, "EyeA\0EyeB\0EyeC\0\0")) {
            ClearEyeWindows();
            if (m_guiIris > 0) AddEyeWindow(0, 1 << 30, m_guiIris);
        }
    }

    // Full per-morph control: a slider for every one of the model's facial morphs. These are
    // always-on overrides on top of the VMD animation (0 = let the animation drive it).
    if (HasAnimation() && MmdMorphCount() > 0) {
        const size_t n = MmdMorphCount();
        // Per-morph override state: -1 = not controlled (let the VMD drive it), 0 = forced off,
        // 1 = forced on. A checkbox toggles between forced-on and forced-off (so unchecking
        // actually drives the weight back to 0 instead of leaving it stuck at its last value).
        if (m_morphWeights.size() != n) m_morphWeights.assign(n, -1.0f);
        if (ImGui::CollapsingHeader("Face morphs (all)")) {
            bool changed = false;
            if (ImGui::SmallButton("Reset all")) {   // release everything back to the animation
                std::fill(m_morphWeights.begin(), m_morphWeights.end(), -1.0f);
                changed = true;
            }
            ImGui::BeginChild("morphlist", ImVec2(0, 260), true);
            for (size_t i = 0; i < n; ++i) {
                ImGui::PushID(static_cast<int>(i));
                const std::string nm = MmdMorphName(i);
                bool on = m_morphWeights[i] > 0.5f;
                if (ImGui::Checkbox(nm.c_str(), &on)) { m_morphWeights[i] = on ? 1.0f : 0.0f; changed = true; }
                ImGui::PopID();
            }
            ImGui::EndChild();
            if (changed && m_animator) { m_animator->SetMorphWeights(m_morphWeights); m_animRefresh.store(true); }
        }
    }

    if (HasCameraMotion()) {
        ImGui::SeparatorText("Camera");
        ImGui::Checkbox("VMD camera motion", &m_camMotionOn);
        if (m_camMotionOn) {
            ImGui::SliderFloat("Cam height", &m_camYOffset, -300.0f, 150.0f);
            ImGui::SameLine(); ImGui::TextDisabled("(down = lower)");
        }
        if (CameraClipCount() > 1) {
            std::vector<const char*> names;
            names.reserve(m_cameraClips.size());
            for (const auto& c : m_cameraClips) names.push_back(c.name.c_str());
            int cur = m_currentCamera;
            if (ImGui::Combo("Cam track", &cur, names.data(), static_cast<int>(names.size())))
                SelectCamera(cur);
        } else {
            ImGui::SameLine(); ImGui::TextDisabled("(cam.vmd)");
        }
    }

    ImGui::SeparatorText("View");
    ImGui::Text("%s  (Z to cycle)", ViewModeName(m_view));

    // -------------------- Dataset generation --------------------
    if (ImGui::CollapsingHeader("Dataset generation")) {
        DatasetConfig& c = m_datasetCfg;
        // Default the label to the current character/motion if unset.
        if (c.character == "char" && m_currentChar < (int)m_characters.size())
            c.character = m_characters[m_currentChar].name;
        if (m_currentClip >= 0 && m_currentClip < (int)m_motionClips.size())
            c.motion = m_motionClips[m_currentClip].name;

        ImGui::Text("char: %s", c.character.c_str());
        ImGui::Text("motion: %s", c.motion.c_str());

        // Live style preview:套用單一風格到渲染視窗，先確認再批次產。 "Off" restores your knobs.
        {
            std::vector<const char*> items;
            items.push_back("Off (live)");
            for (int s = 0; s < StyleCount(); ++s) items.push_back(StyleName((StyleId)s));
            int sel = m_stylePreview + 1;
            if (ImGui::Combo("Style preview", &sel, items.data(), (int)items.size()))
                PreviewStyle(sel - 1);
        }
        ImGui::Checkbox("Preview cut-out bg (去背)", &m_captureIsolated);
        ImGui::SameLine(); ImGui::TextDisabled("(isolated)");
        ImGui::Separator();

        ImGui::SliderInt("Frames",   &c.frameCount,   1, 240);
        ImGui::SliderInt("Azimuths", &c.azimuthCount, 1, 36);
        ImGui::SliderInt("Elev rings", &c.elevCount,  1, 12);
        ImGui::SliderFloat("Elev min", &c.elevMinDeg, -89.0f, 89.0f);
        ImGui::SameLine(); ImGui::TextDisabled("(<0 = 仰角)");
        ImGui::SliderFloat("Elev max", &c.elevMaxDeg, -89.0f, 89.0f);
        ImGui::SliderFloat("Fit margin", &c.fitMargin, 1.0f, 2.5f);
        ImGui::SliderInt("Image size", &c.imgSize, 128, 1024);

        ImGui::Checkbox("Crops (truncated views)", &c.enableCrops);
        if (c.enableCrops) ImGui::SliderFloat("Crop fraction", &c.cropProb, 0.0f, 1.0f);

        ImGui::Checkbox("BG: isolated (cut-out)", &c.bgIsolated);
        ImGui::SameLine();
        ImGui::Checkbox("BG: scene", &c.bgScene);
        ImGui::ColorEdit3("Cut-out bg", &m_captureBg.x);

        // Style multi-select (dataset spans these looks). Empty vector => all styles.
        if (ImGui::TreeNode("Styles")) {
            for (int s = 0; s < (int)StyleId::Count; ++s) {
                bool on = c.styles.empty() ||
                          std::find(c.styles.begin(), c.styles.end(), s) != c.styles.end();
                if (ImGui::Checkbox(StyleName((StyleId)s), &on)) {
                    // Materialize the full list on first edit, then add/remove this style.
                    if (c.styles.empty())
                        for (int k = 0; k < (int)StyleId::Count; ++k) c.styles.push_back(k);
                    auto it = std::find(c.styles.begin(), c.styles.end(), s);
                    if (on && it == c.styles.end()) c.styles.push_back(s);
                    if (!on && it != c.styles.end()) c.styles.erase(it);
                }
            }
            ImGui::TreePop();
        }

        const double dur = HasAnimation() ? m_animator->MotionDurationSeconds() : 0.0;
        ImGui::Text("~ %lld samples", EstimateSampleCount(c, dur));

        const bool busy = m_datasetBusy.load();
        ImGui::BeginDisabled(busy || m_mmd.submeshes.empty());
        if (ImGui::Button(" Generate dataset "))
            RequestDataset();   // runs on the render thread next Update()
        ImGui::EndDisabled();
        if (busy) {
            const int d = m_datasetDone.load(), tot = std::max(1, m_datasetTotal.load());
            ImGui::SameLine();
            ImGui::Text("%d / %d (%.0f%%)", d, m_datasetTotal.load(), 100.0f * d / tot);
        }
    }

    ImGui::End();
}

bool Renderer::LoadScene(const std::wstring& objPath) {
    return LoadSceneFromObj(m_device.Get(), m_queue.Get(), objPath, m_scene);
}

void Renderer::SetPhysics(bool on) {
    m_physicsOn = on;
    if (m_animator) m_animator->SetPhysicsEnabled(on);
}

void Renderer::BindShadowIntoMmdHeap() {
    if (!m_device || !m_mmd.srvHeap || !m_shadowMap || m_mmd.shadowSrvSlot == 0) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format                  = DXGI_FORMAT_R32_FLOAT;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels     = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_mmd.srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(m_mmd.shadowSrvSlot) * m_mmd.srvDescriptorSize;
    m_device->CreateShaderResourceView(m_shadowMap.Get(), &sd, h);
}

void Renderer::ApplyProfileChannelDefaults() {
    // The packed PBR map (_P) channel order differs per game rip. Set the metal/rough channel that
    // the forward shader samples so the model reads correctly out of the box (still GUI-overridable).
    switch (m_mmd.profile) {
    case RenderProfile::EndfieldPBR:
        // Arknights: Endfield — confirmed by per-channel analysis: R=Metallic, G=hair-spec mask,
        // B=AO, A=Roughness (NOT the standard ORM). AO (B) and the hair mask (G) are read directly
        // by Endfield.hlsl; only metal/rough are channel-selectable here.
        m_efMetalChan = 0;   // R
        m_efRoughChan = 3;   // A
        m_efInvertRough = 0;
        break;
    case RenderProfile::WuwaPBR:
        m_efMetalChan = 0;   // R
        m_efRoughChan = 1;   // G (prior default; Wuwa packing unconfirmed — left unchanged)
        break;
    default: break;          // Cel / ZZZ don't read _P rough
    }
}

bool Renderer::LoadMmdModel(const std::wstring& pmxPath) {
    m_animator = std::make_unique<MmdAnimator>();
    if (!m_animator->LoadModel(pmxPath)) { m_animator.reset(); return false; }
    if (!BuildSceneFromMmd(m_device.Get(), m_queue.Get(), *m_animator->Model(),
                           m_animator->MaterialNames(), m_mmd)) {
        m_animator.reset();
        return false;
    }
    BindShadowIntoMmdHeap();   // let the forward PBR pass receive the directional shadow
    m_animator->SetPhysicsEnabled(m_physicsOn);   // honour the physics toggle across reloads

    // Render method follows the game folder the character lives in. A recognised game folder wins;
    // otherwise keep BuildSceneFromMmd's texture-based guess (PBR maps → EndfieldPBR, else Cel).
    const RenderProfile folderProfile = ProfileForPath(pmxPath);
    if (folderProfile != RenderProfile::Cel) m_mmd.profile = folderProfile;
    ApplyProfileChannelDefaults();   // pick the _P metal/rough channels for this rip's packing
    std::printf("[char] render profile: %s\n", RenderProfileName(m_mmd.profile));

    // Rig report. Bone count is whatever this PMX ships (it is NOT a constant across models), and
    // the canonical mapping keys off standard MMD bone names — a model with renamed or missing
    // bones exports fewer usable joints, which is exactly what a pose dataset cares about. Print it
    // at load time so an unusable rig is obvious before generating thousands of samples.
    if (m_animator) {
        std::vector<MmdAnimator::BonePose> bones;
        m_animator->ExtractPose(bones);
        std::set<std::string> mapped;
        for (const auto& b : bones) {
            const std::string c = CanonicalJointName(b.name);
            if (!c.empty()) mapped.insert(c);
        }
        const auto& order = CanonicalJointOrder();
        int core = 0, coreTotal = 0;
        std::string missingCore;
        for (const auto& w : order) {
            if (!IsCoreCanonicalJoint(w)) continue;
            ++coreTotal;
            if (mapped.count(w)) ++core;
            else { if (!missingCore.empty()) missingCore += ", "; missingCore += w; }
        }
        // Break the optional joints out by group: whether a rig carries finger bones (and whether it
        // carries the 先 tips) decides what a hand-aware dataset can ask for.
        int fingers = 0, fingerTips = 0, eyes = 0;
        for (const auto& w : order) {
            const bool tip = w.size() > 4 && w.compare(w.size() - 4, 4, "_tip") == 0;
            const bool fin = !IsCoreCanonicalJoint(w) &&
                             (w.find("thumb") != std::string::npos || w.find("index") != std::string::npos ||
                              w.find("middle") != std::string::npos || w.find("ring") != std::string::npos ||
                              w.find("pinky") != std::string::npos);
            if (!mapped.count(w)) continue;
            if (fin) { if (tip) ++fingerTips; else ++fingers; }
            else if (w == "L_eye" || w == "R_eye" || w == "eyes_center") ++eyes;
        }
        std::printf("[char] rig: %zu bones, %zu/%zu canonical (core %d/%d, fingers %d/30, tips %d/10, "
                    "eyes %d/3)%s%s\n",
                    bones.size(), mapped.size(), order.size(), core, coreTotal,
                    fingers, fingerTips, eyes,
                    missingCore.empty() ? "" : " — MISSING CORE: ", missingCore.c_str());
    }
    std::fflush(stdout);

    // Default placement: scale to ~350 units tall (about a fifth of Sponza's atrium),
    // feet on the floor, in front of the start camera, facing it.
    const float modelHeight = std::max(1e-3f, m_mmd.boundsMax.y - m_mmd.boundsMin.y);
    m_charScale  = 350.0f / modelHeight;
    m_charYawDeg = 270.0f;                      // face the walkway camera; white cape to the back
    // Nudged forward along the facing direction (-X) from the atrium centre; tune with `char pos`.
    m_charPos    = { -160.0f, 0.0f, 38.0f };
    RecomputeCharacterWorld();
    PrintCharacterTransform();
    return true;
}

bool Renderer::LoadMmdMotion(const std::wstring& vmdPath) {
    if (!m_animator) {
        std::fprintf(stderr, "[Renderer] no character loaded; cannot load motion.\n");
        return false;
    }
    if (!m_animator->LoadMotion(vmdPath)) return false;

    // Size the skinning double buffer. The worker thread itself is started lazily on the
    // first Update() — by then all loading (model / motion / BGM) is finished, so the worker
    // never races with LoadBgm setting up m_audio / m_bgmLength.
    m_skinVertexCount = m_animator->VertexCount();
    m_skinFront.assign(m_skinVertexCount, Vertex{});
    m_skinBack.assign(m_skinVertexCount, Vertex{});
    m_animHasFrame.store(false);
    return true;
}

void Renderer::StartAnimThread() {
    if (m_animThread.joinable()) return;        // already running
    m_animThreadRun.store(true);
    m_animThread = std::thread([this] { AnimThreadProc(); });
}

void Renderer::StopAnimThread() {
    m_animThreadRun.store(false);
    if (m_animThread.joinable()) m_animThread.join();
}

// Runs on the worker thread. Owns m_animator exclusively (the render thread never touches
// Saba). Drives the pose from the BGM clock (so audio stays the master), runs physics +
// skinning, and publishes the result into m_skinFront under m_skinMutex.
void Renderer::AnimThreadProc() {
    using clock = std::chrono::steady_clock;
    auto prev = clock::now();
    while (m_animThreadRun.load()) {
        const auto iterStart = clock::now();
        double dtw = std::chrono::duration<double>(iterStart - prev).count();
        prev = iterStart;
        if (dtw > 0.1) dtw = 0.1;

        bool produced = false;

        // Pending seek (replay / anim command), applied on this thread only.
        const double sk = m_seekRequest.exchange(-1.0);
        if (sk >= 0.0 && m_animator) { m_animator->SeekTo(sk); produced = true; }

        if (!m_animPaused.load() && m_animator && m_animator->HasMotion()) {
            double pos;
            if (m_audio && m_audio->Playing() && m_bgmLength > 0.1f) {
                pos = m_audio->PositionSeconds() * m_animSpeed.load();  // BGM master clock, dance time-scaled to fill it
            } else {
                pos = m_animator->AnimTime() + dtw;        // free-run when no BGM
                const double dur = m_animator->MotionDurationSeconds();
                if (dur > 0.1) pos = std::fmod(pos, dur);  // loop a music-less clip on its own length
            }
            m_animator->UpdateTo(pos, dtw);
            produced = true;
        } else if (m_animRefresh.exchange(false) && m_animator && m_animator->HasMotion()) {
            // Paused, but the expression morphs changed — re-skin once at the frozen frame so
            // the new expression is visible without resuming playback.
            m_animator->UpdateTo(m_animator->AnimTime(), 0.0);
            produced = true;
        }

        if (produced && m_skinVertexCount > 0) {
            m_animator->CopySkinnedVertices(m_skinBack.data());
            {
                std::lock_guard<std::mutex> lk(m_skinMutex);
                m_skinFront.swap(m_skinBack);
            }
            m_animHasFrame.store(true);
        }

        // Pace to ~120 Hz so we don't peg a core; if an update is slower than that it just
        // runs at its natural rate (the render thread is unaffected either way).
        const double elapsed = std::chrono::duration<double>(clock::now() - iterStart).count();
        const double target  = 1.0 / 120.0;
        if (elapsed < target)
            std::this_thread::sleep_for(std::chrono::duration<double>(target - elapsed));
    }
}

void Renderer::RecomputeCharacterWorld() {
    const XMFLOAT3& mn = m_mmd.boundsMin;
    const XMFLOAT3& mx = m_mmd.boundsMax;
    const float cx = 0.5f * (mn.x + mx.x);
    const float cz = 0.5f * (mn.z + mx.z);
    const XMMATRIX w =
          XMMatrixTranslation(-cx, -mn.y, -cz)   // feet to origin, centered in XZ
        * XMMatrixScaling(m_charScale, m_charScale, m_charScale)
        * XMMatrixRotationY(XMConvertToRadians(m_charYawDeg))
        * XMMatrixTranslation(m_charPos.x, m_charPos.y, m_charPos.z);
    XMStoreFloat4x4(&m_mmd.world, w);
    SelectShadowedLights();      // shadow the lights nearest the (re)placed character + re-bake
}

void Renderer::PrintCharacterTransform() const {
    std::printf("[char] pos=(%.1f, %.1f, %.1f) scale=%.3f yaw=%.1f\n",
                m_charPos.x, m_charPos.y, m_charPos.z, m_charScale, m_charYawDeg);
    std::fflush(stdout);
}

void Renderer::Update(const Input& input, float dt) {
    m_camera.Update(input, dt);

    // Forward+ point lights orbit their home positions so the tiled culling is visibly
    // dynamic (per the "lights move in real time" expectation).
    if (m_forwardPlus && !m_pointLights.empty()) {
        m_lightAnimTime += dt;
        // The first kNumShadowedLights stay at their home positions (their cube shadows are
        // baked, not re-rendered each frame); the rest orbit to show dynamic tiled culling.
        for (size_t i = kNumShadowedLights; i < m_pointLights.size(); ++i) {
            const float ph = m_lightAnimTime * 0.6f + static_cast<float>(i) * 0.7f;
            const float r  = 120.0f;
            m_pointLights[i].posWS.x = m_lightHomePos[i].x + r * std::cos(ph);
            m_pointLights[i].posWS.z = m_lightHomePos[i].z + r * std::sin(ph);
            m_pointLights[i].posWS.y = m_lightHomePos[i].y + 40.0f * std::sin(ph * 1.7f);
        }
    }

    // Skeletal animation: advance + skin on the CPU, then stream the deformed vertices
    // into a per-frame dynamic vertex buffer the geometry pass reads. Placement stays in
    // m_mmd.world (PerObject CB); these positions are in model space.
    // The MMD animation update runs on the worker thread; here we only upload its most recent
    // skinned frame into this frame's dynamic vertex buffer. The worker is started lazily on
    // the first update (all loading, incl. BGM, is done by now → no startup race).
    // Apply a requested motion-clip switch (GUI / console) here on the render thread, where the
    // heavy VMD reload + worker bounce is safe relative to the camera/morph queries above.
    const int pendClip = m_pendingMotionSwitch.exchange(-1);
    if (pendClip >= 0) ApplyMotionSwitch(pendClip);

    // Same for a requested camera-track switch (the camera track is evaluated below on this
    // thread, so reloading it here keeps it single-threaded).
    const int pendCam = m_pendingCameraSwitch.exchange(-1);
    if (pendCam >= 0) ApplyCameraSwitch(pendCam);

    const int pendChar = m_pendingCharSwitch.exchange(-1);
    if (pendChar >= 0) ApplyCharacterSwitch(pendChar);

    // Dataset generation is requested from the GUI button / console, but the sweep must run on
    // the render thread (it drives Render() + the animator directly). Pick it up here.
    if (m_datasetRequested.exchange(false)) GenerateDataset(m_datasetCfg);

    if (m_animator && m_animator->HasMotion()) {
        if (!m_animThread.joinable()) StartAnimThread();
        StreamSkinnedVertices();
    }

    // VMD camera track: evaluate at the same clock the body uses (BGM position if playing,
    // else the animator's time) and build the world-space view/proj. The camera anim object
    // is only touched here on the main thread (the worker never uses it).
    if (m_camMotionOn && m_animator && m_animator->HasCamera()) {
        const double t = (m_audio && m_audio->Playing() && m_bgmLength > 0.1f)
                       ? m_audio->PositionSeconds() * m_animSpeed.load()
                       : m_animator->AnimTime();
        XMFLOAT3 eyeL, ctrL, upL; float fovY = XM_PIDIV4;
        m_animator->EvaluateCamera(t, eyeL, ctrL, upL, fovY);

        // The camera keyframes are in the model's own space; put them into world space with
        // the same matrix the character verts use, so the shot frames the placed character.
        const XMMATRIX world = XMLoadFloat4x4(&m_mmd.world);
        XMVECTOR eye = XMVector3TransformCoord (XMLoadFloat3(&eyeL), world);
        XMVECTOR ctr = XMVector3TransformCoord (XMLoadFloat3(&ctrL), world);
        XMVECTOR up  = XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&upL), world));

        // Shift the whole shot vertically (the camera.vmd frames a taller model; drop it onto
        // our shorter character). Same offset on eye + target keeps the look direction.
        const XMVECTOR yOff = XMVectorSet(0.0f, m_camYOffset, 0.0f, 0.0f);
        eye = XMVectorAdd(eye, yOff);
        ctr = XMVectorAdd(ctr, yOff);

        const float aspect = (m_height != 0) ? static_cast<float>(m_width) / m_height : 1.0f;
        XMStoreFloat4x4(&m_camView, XMMatrixLookAtLH(eye, ctr, up));
        XMStoreFloat4x4(&m_camProj, XMMatrixPerspectiveFovLH(fovY, aspect, kCameraZNear, kCameraZFar));
        XMStoreFloat3(&m_camPos, eye);
    }
}

bool Renderer::HasAnimation() const { return m_animator && m_animator->HasMotion(); }

void Renderer::SetPaused(bool p) {
    m_animPaused = p;
    if (m_audio && m_audio->Loaded()) {
        if (p) m_audio->Pause();
        else   m_audio->Play();
    }
}

void Renderer::TogglePause() { SetPaused(!m_animPaused); }

void Renderer::Replay() {
    if (m_audio && m_audio->Loaded()) m_audio->Restart();   // music restarts (resets the master clock)
    m_seekRequest.store(0.0);                               // worker seeks the motion to the top
    m_animPaused.store(false);
}

void Renderer::AddMotionClip(const std::string& name, const std::wstring& vmdPath,
                             const std::wstring& bgmPath) {
    m_motionClips.push_back({ name, vmdPath, bgmPath });
}

int Renderer::MotionClipCount() const { return static_cast<int>(m_motionClips.size()); }

std::string Renderer::MotionClipName(int i) const {
    return (i >= 0 && i < static_cast<int>(m_motionClips.size())) ? m_motionClips[i].name : std::string{};
}

bool Renderer::SelectMotion(int index) {
    if (index < 0 || index >= static_cast<int>(m_motionClips.size())) return false;
    // The switch reloads the VMD (which the anim worker owns) and is also touched by the render
    // thread's camera/morph queries, so it must run on the render thread. Whoever calls this
    // (GUI on the render thread, or the console thread) just parks the request; Update() applies
    // it next frame via ApplyMotionSwitch.
    m_pendingMotionSwitch.store(index);
    return true;
}

void Renderer::ApplyMotionSwitch(int index) {
    if (index < 0 || index >= static_cast<int>(m_motionClips.size())) return;
    const MotionClip clip = m_motionClips[index];   // copy: m_motionClips isn't mutated, but be safe

    StopAnimThread();   // the worker owns m_animator; stop it before re-binding the VMD

    if (!m_animator || !m_animator->LoadMotion(clip.vmd)) {
        std::fprintf(stderr, "[motion] failed to load clip [%d] %s\n", index, clip.name.c_str());
        StartAnimThread();   // keep the previous motion playing
        return;
    }
    m_currentClip = index;
    // The skin double buffer normally needs no resize — every clip drives the same model (same
    // vertex count) and the old buffers stay valid until the worker overwrites them. But if the
    // startup motion was absent (e.g. VMDs reorganised into motion/), the initial LoadMmdMotion
    // that sizes these buffers never ran, so size them here before the worker writes m_skinBack.
    if (m_skinVertexCount == 0 || m_skinBack.size() != m_animator->VertexCount()) {
        m_skinVertexCount = m_animator->VertexCount();
        m_skinFront.assign(m_skinVertexCount, Vertex{});
        m_skinBack.assign(m_skinVertexCount, Vertex{});
        m_animHasFrame.store(false);
    }

    // Swap the music so it never fights the dance: a clip with a BGM plays it synced from the
    // top; a music-less clip stops the audio and the motion free-runs + loops on its own clock.
    if (!clip.bgm.empty()) {
        if (!m_audio) m_audio = std::make_unique<Audio>();
        const bool audioOk = m_audio->Load(clip.bgm);
        if (audioOk) {
            // Equal length WITHOUT cutting the song: keep the music at its full length and
            // time-scale the dance to fill it. The dance is slaved to the audio position, so
            // pose time = audioPos * (motionLen / musicLen) → the whole dance plays across the
            // whole song (a dance shorter than the song just plays slower, fixing "dance is
            // faster than the music"). m_danceSpeed is a live multiplier on top for fine-tuning.
            const double motionLen = m_animator->MotionDurationSeconds();
            const double musicLen  = m_audio->LengthSeconds();
            m_bgmLength = static_cast<float>(musicLen);
            double fit = (musicLen > 0.1 && motionLen > 0.1) ? (motionLen / musicLen) : 1.0;
            // When the dance is stretched to a longer song, the exact fill (≈0.909 for seele) read
            // a hair fast; the user tuned the sweet spot to 0.920. Only override when stretched, so
            // an already-matched clip like dz stays at 1.0.
            if (musicLen > motionLen + 1.0) fit = 0.920;
            m_animSpeed.store(fit);
            if (!m_animPaused) m_audio->Restart();
        } else {
            std::fprintf(stderr, "[motion] BGM decode failed for clip [%d]; dancing silently\n", index);
            m_bgmLength = 0.0f;   // decode failed → treat as music-less so the dance still loops
        }
    } else {
        if (m_audio) m_audio->Stop();
        m_bgmLength = 0.0f;
    }

    m_seekRequest.store(0.0);   // worker seeks the new motion to the top

    // Log BEFORE restarting the worker: while it's stopped there's no stdout-silence window to
    // swallow this line (the worker's ScopedStdSilence does a process-wide fd redirect).
    const double motionLen = m_animator->MotionDurationSeconds();
    if (clip.bgm.empty()) {
        std::printf("[motion] now playing [%d] %s (no BGM, looping on its own %.1f s)\n",
                    index, clip.name.c_str(), motionLen);
    } else {
        std::printf("[motion] now playing [%d] %s  (motion %.1f s @ x%.3f -> fills BGM %.1f s)\n",
                    index, clip.name.c_str(), motionLen, m_animSpeed.load(), m_bgmLength);
    }
    std::fflush(stdout);

    StartAnimThread();
}

void Renderer::PrintBoneList(const std::string& filter) const {
    if (!m_animator) { std::printf("[bones] no character loaded\n"); std::fflush(stdout); return; }
    std::vector<MmdAnimator::BonePose> bones;
    m_animator->ExtractPose(bones);
    int shown = 0, mapped = 0;
    for (size_t i = 0; i < bones.size(); ++i) {
        const std::string c = CanonicalJointName(bones[i].name);
        if (!c.empty()) ++mapped;
        if (!filter.empty() && bones[i].name.find(filter) == std::string::npos &&
            c.find(filter) == std::string::npos) continue;
        std::printf("  [%3zu] parent=%4d  %-24s %s\n", i, bones[i].parent,
                    bones[i].name.c_str(), c.empty() ? "" : ("-> " + c).c_str());
        ++shown;
    }
    std::printf("[bones] %zu bones, %d canonical-tagged%s (%d shown)\n",
                bones.size(), mapped, filter.empty() ? "" : ", filtered", shown);
    std::fflush(stdout);
}

bool Renderer::LoadMmdCameraMotion(const std::wstring& vmdPath) {
    if (!m_animator) {
        std::fprintf(stderr, "[Renderer] no character loaded; cannot load camera motion.\n");
        return false;
    }
    return m_animator->LoadCameraMotion(vmdPath);
}

void Renderer::AddCharacter(const std::string& name, const std::wstring& pmxPath) {
    m_characters.push_back({ name, pmxPath });
}
int Renderer::CharacterCount() const { return static_cast<int>(m_characters.size()); }
std::string Renderer::CharacterName(int i) const {
    return (i >= 0 && i < static_cast<int>(m_characters.size())) ? m_characters[i].name : std::string{};
}
std::wstring Renderer::CharacterPath(int i) const {
    return (i >= 0 && i < static_cast<int>(m_characters.size())) ? m_characters[i].pmx : std::wstring{};
}
bool Renderer::SelectCharacter(int index) {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return false;
    m_pendingCharSwitch.store(index);   // applied on the render thread in Update()
    return true;
}

void Renderer::ApplyCharacterSwitch(int index) {
    if (index < 0 || index >= static_cast<int>(m_characters.size())) return;
    const CharacterAsset asset = m_characters[index];

    // Remember the current motion so it can be rebound onto the new skeleton after the reload.
    std::wstring currentVmd;
    if (m_currentClip >= 0 && m_currentClip < static_cast<int>(m_motionClips.size()))
        currentVmd = m_motionClips[m_currentClip].vmd;

    StopAnimThread();               // the worker owns m_animator; stop before replacing it
    WaitForGpu();                   // m_mmd GPU resources are about to be rebuilt

    if (!LoadMmdModel(asset.pmx)) { // rebuilds m_animator + m_mmd, resets placement
        std::fprintf(stderr, "[char] failed to load [%d] %s\n", index, asset.name.c_str());
        StartAnimThread();
        return;
    }
    m_currentChar = index;
    m_datasetCfg.character = asset.name;   // dataset output path follows the character

    // Fresh skin buffers for the new vertex count; drop any stale frame.
    m_skinVertexCount = m_animator->VertexCount();
    m_skinFront.assign(m_skinVertexCount, Vertex{});
    m_skinBack.assign(m_skinVertexCount, Vertex{});
    m_animHasFrame.store(false);

    if (!currentVmd.empty() && m_animator->LoadMotion(currentVmd))
        m_seekRequest.store(0.0);

    std::printf("[char] now showing [%d] %s\n", index, asset.name.c_str());
    std::fflush(stdout);
    StartAnimThread();
}

bool Renderer::HasCameraMotion() const { return m_animator && m_animator->HasCamera(); }

void Renderer::AddCameraClip(const std::string& name, const std::wstring& vmdPath) {
    m_cameraClips.push_back({ name, vmdPath });
}

int Renderer::CameraClipCount() const { return static_cast<int>(m_cameraClips.size()); }

std::string Renderer::CameraClipName(int i) const {
    return (i >= 0 && i < static_cast<int>(m_cameraClips.size())) ? m_cameraClips[i].name : std::string{};
}

bool Renderer::SelectCamera(int index) {
    if (index < 0 || index >= static_cast<int>(m_cameraClips.size())) return false;
    // The camera track is reloaded into the animator, which the render thread evaluates each
    // frame; so the GUI / console just parks the request and Update() applies it on the render
    // thread via ApplyCameraSwitch.
    m_pendingCameraSwitch.store(index);
    return true;
}

void Renderer::ApplyCameraSwitch(int index) {
    if (index < 0 || index >= static_cast<int>(m_cameraClips.size()) || !m_animator) return;
    const bool loaded = m_animator->LoadCameraMotion(m_cameraClips[index].vmd);
    // The anim worker runs concurrently here and silences stdout in bursts; hold the shared IO
    // lock so this log can't be redirected to NUL mid-write.
    std::lock_guard<std::recursive_mutex> ioLock(ConsoleIoMutex());
    if (loaded) {
        m_currentCamera = index;
        std::printf("[cam] camera track [%d] %s\n", index, m_cameraClips[index].name.c_str());
        std::fflush(stdout);
    } else {
        std::fprintf(stderr, "[cam] failed to load camera track [%d] %s\n",
                     index, m_cameraClips[index].name.c_str());
    }
}

void Renderer::AddEyeWindow(int startFrame, int endFrame, int expr) {
    if (endFrame < startFrame) std::swap(startFrame, endFrame);
    m_eyeWindows.push_back({ startFrame, endFrame, (expr < 0 || expr > 2) ? 0 : expr });
}

void Renderer::ClearEyeWindows() { m_eyeWindows.clear(); }

void Renderer::PrintEyeWindows() const {
    if (m_eyeWindows.empty()) { std::printf("[eye] no windows (eyes = EyeA)\n"); }
    for (const auto& w : m_eyeWindows)
        std::printf("[eye] frames %d..%d -> Eye%c\n", w.start, w.end, static_cast<char>('A' + w.expr));
    std::fflush(stdout);
}

int Renderer::CurrentEyeExpr() const {
    if (m_eyeWindows.empty()) return 0;
    double tsec;
    if (m_audio && m_audio->Playing() && m_bgmLength > 0.1f) tsec = m_audio->PositionSeconds() * m_animSpeed.load();
    else if (m_animator)                                     tsec = m_animator->AnimTime();
    else return 0;
    const int frame = static_cast<int>(tsec * 30.0);   // MMD 30 fps, within the looped motion
    for (const auto& w : m_eyeWindows)
        if (frame >= w.start && frame <= w.end) return w.expr;
    return 0;
}

size_t Renderer::MmdMorphCount() const { return m_animator ? m_animator->MorphCount() : 0; }
std::string Renderer::MmdMorphName(size_t i) const { return m_animator ? m_animator->MorphName(i) : std::string{}; }
int Renderer::FindMmdMorph(const std::string& name) const { return m_animator ? m_animator->FindMorph(name) : -1; }

void Renderer::PushExprWindows() {
    if (!m_animator) return;
    std::vector<MmdAnimator::MorphWindow> w;
    w.reserve(m_exprWindows.size());
    for (const auto& e : m_exprWindows)
        w.push_back({ e.start, e.end, e.morphIdx, e.weight });
    m_animator->SetMorphWindows(w);   // the worker thread reads these each update
    m_animRefresh.store(true);        // re-skin once even if paused, so the change shows
}

void Renderer::AddExprWindow(int morphIdx, int startFrame, int endFrame, float weight) {
    if (endFrame < startFrame) std::swap(startFrame, endFrame);
    m_exprWindows.push_back({ startFrame, endFrame, morphIdx, weight });
    PushExprWindows();
}

void Renderer::ClearExprWindows() { m_exprWindows.clear(); PushExprWindows(); }

void Renderer::PrintExprWindows() const {
    if (m_exprWindows.empty()) { std::printf("[expr] no windows\n"); std::fflush(stdout); return; }
    for (const auto& w : m_exprWindows) {
        const std::string nm = MmdMorphName(static_cast<size_t>(w.morphIdx));
        std::printf("[expr] frames %d..%d -> morph %d \"%s\" @ %.2f\n",
                    w.start, w.end, w.morphIdx, nm.c_str(), w.weight);
    }
    std::fflush(stdout);
}

bool Renderer::LoadBgm(const std::wstring& wavPath) {
    if (!m_audio) m_audio = std::make_unique<Audio>();
    if (!m_audio->Load(wavPath)) return false;
    m_bgmLength = m_audio->LengthSeconds();
    if (!m_animPaused) m_audio->Play();   // the dance auto-plays at launch, so start the music too
    std::printf("[bgm] loaded (%.1f s loop)\n", m_bgmLength);
    std::fflush(stdout);
    return true;
}

void Renderer::StreamSkinnedVertices() {
    // Render-thread side: copy the worker's latest skinned verts into this frame's dynamic
    // VB. Until the worker has produced a frame, keep the static bind-pose VB.
    if (!m_graphicsMemory || m_skinVertexCount == 0 || !m_animHasFrame.load()) return;
    const size_t vc = m_skinVertexCount;
    m_mmdDynamicVB = m_graphicsMemory->Allocate(vc * sizeof(Vertex));
    {
        std::lock_guard<std::mutex> lk(m_skinMutex);
        std::memcpy(m_mmdDynamicVB.Memory(), m_skinFront.data(), vc * sizeof(Vertex));
    }
    m_mmd.vbv.BufferLocation = m_mmdDynamicVB.GpuAddress();
    m_mmd.vbv.SizeInBytes    = static_cast<UINT>(vc * sizeof(Vertex));
    m_mmd.vbv.StrideInBytes  = sizeof(Vertex);
}

void Renderer::SampleAnimation(double seconds) {
    if (!m_animator || !m_animator->HasMotion()) return;
    m_seekRequest.store(seconds);   // applied by the worker thread (it owns the animator)
}

void Renderer::ReleaseBackBuffers() {
    for (auto& b : m_backBuffers) b.Reset();
}

void Renderer::Resize(UINT width, UINT height) {
    if (!m_swap || width == 0 || height == 0) return;
    if (width == m_width && height == m_height) return;

    WaitForGpu();
    ReleaseBackBuffers();
    m_depth.Reset();
    m_normalRT.Reset();
    m_albedoRT.Reset();

    DXGI_SWAP_CHAIN_DESC1 sd{}; m_swap->GetDesc1(&sd);
    ThrowIfFailed(m_swap->ResizeBuffers(kFrameCount, width, height, sd.Format, sd.Flags));
    m_frameIndex = m_swap->GetCurrentBackBufferIndex();

    // ResizeBuffers restarts the swap chain at back buffer 0, so m_frameIndex can jump to a slot
    // whose fence value is STALE (WaitForGpu only bumped the slot we were on). The next
    // MoveToNextFrame() would then Signal() that smaller value — the fence runs backwards — and
    // the following SetEventOnCompletion(INFINITE) waits on a value the queue will never reach:
    // the render loop deadlocks and the window stops responding (classic "maximize freezes it").
    // The GPU is idle right here, so level every slot to one value past everything signalled.
    {
        UINT64 next = 0;
        for (UINT i = 0; i < kFrameCount; ++i) next = std::max(next, m_fenceValues[i]);
        next = std::max(next, m_fence ? m_fence->GetCompletedValue() + 1 : next);
        for (UINT i = 0; i < kFrameCount; ++i) m_fenceValues[i] = next;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        ThrowIfFailed(m_swap->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])));
        m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtv);
        rtv.ptr += m_rtvSize;
    }

    m_width  = width;
    m_height = height;
    UpdateRenderResolution();                // m_rw/m_rh before the RTs are (re)created

    CreateDepthBuffer(m_rw, m_rh);
    CreateGBuffer(m_rw, m_rh);               // ldrRT stays window-size (uses m_width/m_height)
    RecreateGBufferSrvs();
    CreateTileLightBuffer(m_rw, m_rh);       // tile count tracks the (supersampled) render resolution

    m_camera.SetAspect(static_cast<float>(width) / static_cast<float>(height));
}

void Renderer::Render() {
    if (!m_cmd) return;

    // Apply a pending SSAA change here, at a frame boundary with the GPU idle, so the internal
    // render targets can be safely rebuilt at the new supersampled resolution.
    {
        const float ps = m_pendingSsaa.exchange(-1.0f);
        if (ps >= 1.0f && ps != m_ssaa) {
            WaitForGpu();
            m_ssaa = ps;
            UpdateRenderResolution();
            CreateDepthBuffer(m_rw, m_rh);
            CreateGBuffer(m_rw, m_rh);       // ldrRT stays window-size
            RecreateGBufferSrvs();
            CreateTileLightBuffer(m_rw, m_rh);
        }
    }

    ThrowIfFailed(m_allocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_cmd->Reset(m_allocators[m_frameIndex].Get(), nullptr));

    const bool drawImGui = m_imguiReady && m_imguiVisible;
    if (drawImGui) {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        BuildImGuiUI();
        ImGui::Render();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvBase = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvBack = rtvBase;
    rtvBack.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvSize;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvNormal = rtvBase;
    rtvNormal.ptr += static_cast<SIZE_T>(kGBufferNormalRtvIndex) * m_rtvSize;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvAlbedo = rtvBase;
    rtvAlbedo.ptr += static_cast<SIZE_T>(kGBufferAlbedoRtvIndex) * m_rtvSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

    // Main scene passes (G-buffer, SSAO, lighting, forward NPR) render at the SUPERSAMPLED size
    // (vp/scrt); only the final tonemap+FXAA blit runs at the window size (vpWin/scrtWin), which is
    // where the SS image is box-downsampled to the back buffer.
    D3D12_VIEWPORT vp{};
    vp.Width    = static_cast<float>(m_rw);
    vp.Height   = static_cast<float>(m_rh);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    D3D12_RECT scrt{ 0, 0, static_cast<LONG>(m_rw), static_cast<LONG>(m_rh) };
    D3D12_VIEWPORT vpWin{ 0, 0, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f };
    D3D12_RECT     scrtWin{ 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };

    // -------------------- Per-frame constants (shared between passes) --------------------
    auto cb = m_graphicsMemory->AllocateConstant<PerFrameCB>();
    PerFrameCB* data = static_cast<PerFrameCB*>(cb.Memory());

    // Active camera: the VMD camera track when enabled (matrices built in Update), otherwise
    // the free-fly camera. Everything downstream reads vpM / viewStore / cameraPos.
    const bool useVmdCam = m_camMotionOn && m_animator && m_animator->HasCamera();
    XMMATRIX vpM, viewM;
    XMFLOAT3 cameraPos;
    if (useVmdCam) {
        viewM     = XMLoadFloat4x4(&m_camView);
        vpM       = viewM * XMLoadFloat4x4(&m_camProj);
        cameraPos = m_camPos;
    } else {
        viewM     = m_camera.View();
        vpM       = m_camera.ViewProj();
        cameraPos = m_camera.Position();
    }
    XMMATRIX invVpM = XMMatrixInverse(nullptr, vpM);
    XMFLOAT4X4 viewStore; XMStoreFloat4x4(&viewStore, viewM);
    XMStoreFloat4x4(&data->viewProj,    vpM);
    XMStoreFloat4x4(&data->invViewProj, invVpM);
    data->cameraPos       = cameraPos;
    data->viewMode        = static_cast<UINT>(m_view);
    data->lightDirToLight = m_lightDir;
    data->zNear           = kCameraZNear;
    data->lightIntensity  = m_lightTint;   // white by default; RandomLight dataset style tints it
    data->zFar            = kCameraZFar;

    // ---- Directional-light view-projection: an orthographic frustum fit to the scene
    //      bounds, looking from along the light direction toward the scene centre.
    XMFLOAT3 mn = m_scene.boundsMin, mx = m_scene.boundsMax;
    XMVECTOR bmin = XMLoadFloat3(&mn), bmax = XMLoadFloat3(&mx);
    XMVECTOR center = XMVectorScale(XMVectorAdd(bmin, bmax), 0.5f);
    float radius = XMVectorGetX(XMVector3Length(XMVectorSubtract(bmax, bmin))) * 0.5f;
    if (radius < 1.0f) radius = 2000.0f; // no scene loaded yet
    XMVECTOR dirToLight = XMVector3Normalize(XMLoadFloat3(&m_lightDir));
    XMVECTOR eye = XMVectorAdd(center, XMVectorScale(dirToLight, radius * 2.0f));
    XMVECTOR up  = (fabsf(XMVectorGetY(dirToLight)) > 0.99f)
                 ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);
    XMMATRIX lview = XMMatrixLookAtLH(eye, center, up);
    XMMATRIX lproj = XMMatrixOrthographicLH(radius * 2.0f, radius * 2.0f, 0.1f, radius * 4.0f);
    XMStoreFloat4x4(&data->lightViewProj, lview * lproj);
    data->shadowBias    = 0.0015f;
    data->shadowTexel   = 1.0f / static_cast<float>(kShadowMapSize);
    data->outlineDarken = m_outlineDarken;
    data->captureBg     = { m_captureBg.x, m_captureBg.y, m_captureBg.z,
                            m_captureIsolated ? 1.0f : 0.0f };

    // Endfield face SDF: head-bone basis in world space. Model-space bone axes → world (rotation of
    // m_mmd.world) → MMD -front/-right convention + re-orthogonalize (matches EfFaceGetHeadBasis).
    data->headFront = { 0.0f, 0.0f, -1.0f };
    data->headRight = { -1.0f, 0.0f, 0.0f };
    data->headUp    = { 0.0f, 1.0f, 0.0f };
    data->headValid = 0.0f;
    if (m_animator) {
        XMFLOAT3 mFwd, mRight;
        if (m_animator->HeadBasis(mFwd, mRight)) {
            XMMATRIX world  = XMLoadFloat4x4(&m_mmd.world);
            XMVECTOR fwdW   = XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&mFwd),   world));
            XMVECTOR rightW = XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&mRight), world));
            XMVECTOR hf = XMVectorNegate(fwdW);
            XMVECTOR hr = XMVectorNegate(rightW);
            XMVECTOR hu = XMVector3Normalize(XMVector3Cross(hf, hr));
            hr = XMVector3Normalize(XMVector3Cross(hu, hf));   // re-orthogonalize
            XMStoreFloat3(&data->headFront, hf);
            XMStoreFloat3(&data->headRight, hr);
            XMStoreFloat3(&data->headUp,    hu);
            data->headValid = 1.0f;
        }
    }

    // CharDepth range: the character's model-space bounds through its world matrix and into view
    // space. Its own depth span then fills the whole 0..1 output instead of a sliver of [0, zFar].
    {
        float cNear = FLT_MAX, cFar = -FLT_MAX;
        if (!m_mmd.submeshes.empty()) {
            const XMMATRIX toView = XMLoadFloat4x4(&m_mmd.world) * viewM;
            // Prefer the posed geometry over the bounding box: a box around a dancing figure sticks
            // out at its corners, which stretches the range and flattens the whole map to mid-grey.
            // Every 8th skinned vertex is plenty to bracket the span, and costs ~20k dot products.
            // Measured for EVERY view, not just CharDepth: this range is also written into each
            // dataset annotation as the scale of the char_depth image, and it must not depend on
            // which style happened to be rendering when the sample was written.
            bool measured = false;
            if (m_animHasFrame.load() && m_skinVertexCount > 0) {
                std::lock_guard<std::mutex> lk(m_skinMutex);
                if (m_skinFront.size() >= m_skinVertexCount) {
                    for (size_t v = 0; v < m_skinVertexCount; v += 8) {
                        const float z = XMVectorGetZ(
                            XMVector3TransformCoord(XMLoadFloat3(&m_skinFront[v].position), toView));
                        cNear = std::min(cNear, z);
                        cFar  = std::max(cFar,  z);
                    }
                    measured = (cFar > cNear);
                }
            }
            if (!measured) {                        // bind pose / other views: bounding box is fine
                const XMFLOAT3& lo = m_mmd.boundsMin;
                const XMFLOAT3& hi = m_mmd.boundsMax;
                cNear = FLT_MAX; cFar = -FLT_MAX;
                for (int c = 0; c < 8; ++c) {
                    const XMVECTOR corner = XMVectorSet((c & 1) ? hi.x : lo.x,
                                                        (c & 2) ? hi.y : lo.y,
                                                        (c & 4) ? hi.z : lo.z, 1.0f);
                    const float z = XMVectorGetZ(XMVector3TransformCoord(corner, toView));
                    cNear = std::min(cNear, z);
                    cFar  = std::max(cFar,  z);
                }
            }
        }
        if (!(cFar > cNear)) { cNear = kCameraZNear; cFar = kCameraZFar; }   // no character loaded
        data->charDepthRange = { cNear, cFar, 0.0f, 0.0f };
        m_charDepthNear = cNear;   // recorded into the dataset annotation next to the image
        m_charDepthFar  = cFar;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS cbAddr = cb.GpuAddress();

    // Shared helpers (used by the shadow + post passes).
    auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = r;
        b.Transition.StateBefore  = before;
        b.Transition.StateAfter   = after;
        b.Transition.Subresource  = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_cmd->ResourceBarrier(1, &b);
    };
    auto rtvAt = [&](UINT index) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvBase;
        h.ptr += static_cast<SIZE_T>(index) * m_rtvSize;
        return h;
    };
    auto srvAt = [&](UINT slot) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = m_gbufferSrvHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(slot) * m_srvSize;
        return h;
    };

    // ============================== SHADOW PASS (light-view depth) ==============================
    {
        barrier(m_shadowMap.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_CPU_DESCRIPTOR_HANDLE sdsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        sdsv.ptr += static_cast<SIZE_T>(m_dsvSize); // slot 1
        m_cmd->OMSetRenderTargets(0, nullptr, FALSE, &sdsv);
        m_cmd->ClearDepthStencilView(sdsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        D3D12_VIEWPORT svp{ 0, 0, (float)kShadowMapSize, (float)kShadowMapSize, 0.0f, 1.0f };
        D3D12_RECT     ssc{ 0, 0, (LONG)kShadowMapSize, (LONG)kShadowMapSize };
        m_cmd->RSSetViewports(1, &svp);
        m_cmd->RSSetScissorRects(1, &ssc);
        m_cmd->SetPipelineState(m_shadowPSO.Get());
        m_cmd->SetGraphicsRootSignature(m_shadowRS.Get());
        m_cmd->SetGraphicsRootConstantBufferView(0, cbAddr);
        m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        auto drawDepth = [&](const Scene& sc, UINT matId) {
            if (sc.submeshes.empty()) return;
            auto oc = m_graphicsMemory->AllocateConstant<PerObjectCB>();
            PerObjectCB* o = static_cast<PerObjectCB*>(oc.Memory());
            o->world = sc.world; o->materialId = matId;
            o->useFaceMask = 0; o->useNormalMap = 0; o->satBoost = 1.0f;
            o->useSphere = 0;
            m_cmd->SetGraphicsRootConstantBufferView(1, oc.GpuAddress());
            m_cmd->IASetVertexBuffers(0, 1, &sc.vbv);
            m_cmd->IASetIndexBuffer(&sc.ibv);
            for (const auto& sm : sc.submeshes)
                m_cmd->DrawIndexedInstanced(sm.indexCount, 1, sm.indexStart, 0, 0);
        };
        drawDepth(m_scene, 0);
        drawDepth(m_mmd, 1);
        barrier(m_shadowMap.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // ====================== POINT-LIGHT CUBE SHADOWS (first N coloured lights) ======================
    // The first kNumShadowedLights of the 128 Forward+ lights each cast an omnidirectional cube
    // distance-shadow (6 faces). These lights are static and the cubes are BAKED only when dirty
    // (toggled on / character placement changed) — 6×N full-scene passes every frame is far too
    // slow (~10 fps). Character animation is therefore frozen inside these cubes.
    const bool drawPointShadows = m_forwardPlus && m_pointShadowOn;
    if (drawPointShadows != (m_prevForwardPlus && m_prevPointShadow)) m_pointShadowDirty = true;
    m_prevForwardPlus = m_forwardPlus; m_prevPointShadow = m_pointShadowOn;
    if (drawPointShadows && m_pointShadowDirty) {
        m_pointShadowDirty = false;
        struct Face { XMVECTOR dir, up; };
        const Face faces[6] = {
            { XMVectorSet( 1, 0, 0,0), XMVectorSet(0,1, 0,0) },
            { XMVectorSet(-1, 0, 0,0), XMVectorSet(0,1, 0,0) },
            { XMVectorSet( 0, 1, 0,0), XMVectorSet(0,0,-1,0) },
            { XMVectorSet( 0,-1, 0,0), XMVectorSet(0,0, 1,0) },
            { XMVectorSet( 0, 0, 1,0), XMVectorSet(0,1, 0,0) },
            { XMVectorSet( 0, 0,-1,0), XMVectorSet(0,1, 0,0) },
        };

        barrier(m_pointCube.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_VIEWPORT pvp{ 0, 0, (float)kPointShadowSize, (float)kPointShadowSize, 0.0f, 1.0f };
        D3D12_RECT     psc{ 0, 0, (LONG)kPointShadowSize, (LONG)kPointShadowSize };
        m_cmd->RSSetViewports(1, &pvp);
        m_cmd->RSSetScissorRects(1, &psc);
        m_cmd->SetPipelineState(m_pointShadowPSO.Get());
        m_cmd->SetGraphicsRootSignature(m_pointShadowRS.Get());
        m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        D3D12_CPU_DESCRIPTOR_HANDLE faceRtvBase = m_pointRtvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE pdsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        pdsv.ptr += static_cast<SIZE_T>(2) * m_dsvSize;

        for (UINT li = 0; li < kNumShadowedLights; ++li) {
            const XMVECTOR L = XMLoadFloat3(&m_pointLights[li].posWS);
            const float    range = m_pointLights[li].radius;
            const XMMATRIX proj  = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, 5.0f, range);
            for (UINT f = 0; f < 6; ++f) {
                D3D12_CPU_DESCRIPTOR_HANDLE frtv = faceRtvBase;
                frtv.ptr += static_cast<SIZE_T>(li * 6 + f) * m_rtvSize;
                m_cmd->OMSetRenderTargets(1, &frtv, FALSE, &pdsv);
                const float kFar[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
                m_cmd->ClearRenderTargetView(frtv, kFar, 0, nullptr);
                m_cmd->ClearDepthStencilView(pdsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

                auto pcb = m_graphicsMemory->AllocateConstant<PointShadowCB>();
                PointShadowCB* pc = static_cast<PointShadowCB*>(pcb.Memory());
                XMStoreFloat4x4(&pc->faceViewProj, XMMatrixLookToLH(L, faces[f].dir, faces[f].up) * proj);
                pc->lightPos = m_pointLights[li].posWS; pc->lightRange = range;
                m_cmd->SetGraphicsRootConstantBufferView(0, pcb.GpuAddress());

                auto drawPt = [&](const Scene& sc, UINT matId) {
                    if (sc.submeshes.empty()) return;
                    auto oc = m_graphicsMemory->AllocateConstant<PerObjectCB>();
                    PerObjectCB* o = static_cast<PerObjectCB*>(oc.Memory());
                    o->world = sc.world; o->materialId = matId;
                    m_cmd->SetGraphicsRootConstantBufferView(1, oc.GpuAddress());
                    m_cmd->IASetVertexBuffers(0, 1, &sc.vbv);
                    m_cmd->IASetIndexBuffer(&sc.ibv);
                    for (const auto& sm : sc.submeshes)
                        m_cmd->DrawIndexedInstanced(sm.indexCount, 1, sm.indexStart, 0, 0);
                };
                drawPt(m_scene, 0);
                drawPt(m_mmd, 1);
            }
        }
        barrier(m_pointCube.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // ============================== GEOMETRY PASS ==============================

    D3D12_CPU_DESCRIPTOR_HANDLE gRTVs[2] = { rtvNormal, rtvAlbedo };
    m_cmd->OMSetRenderTargets(2, gRTVs, FALSE, &dsv);
    m_cmd->RSSetViewports(1, &vp);
    m_cmd->RSSetScissorRects(1, &scrt);

    static constexpr float kZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_cmd->ClearRenderTargetView(rtvNormal, kZero, 0, nullptr);
    m_cmd->ClearRenderTargetView(rtvAlbedo, kZero, 0, nullptr);
    m_cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Under dataset isolation the Sponza scene is not drawn — only the character ends up in the
    // G-buffer, so the lighting pass sees background everywhere else and cuts the character out.
    if (!m_captureIsolated && !m_scene.submeshes.empty() && m_scene.srvHeap) {
        m_cmd->SetPipelineState(m_geometryPSO.Get());
        m_cmd->SetGraphicsRootSignature(m_geometryRS.Get());

        ID3D12DescriptorHeap* heaps[] = { m_scene.srvHeap.Get() };
        m_cmd->SetDescriptorHeaps(1, heaps);

        m_cmd->SetGraphicsRootConstantBufferView(0, cbAddr);

        auto objCb = m_graphicsMemory->AllocateConstant<PerObjectCB>();
        PerObjectCB* obj = static_cast<PerObjectCB*>(objCb.Memory());
        obj->world        = m_scene.world;
        obj->materialId   = 0;
        obj->useFaceMask  = 0;
        obj->useNormalMap = 0;
        obj->satBoost         = 1.0f;
        obj->view             = viewStore;
        obj->useSphere        = 0;
        obj->faceMaskStrength = 0.0f;
        obj->sphereStrength   = 0.0f;
        obj->contrast         = 1.0f;   // scene: no contrast adjustment
        m_cmd->SetGraphicsRootConstantBufferView(2, objCb.GpuAddress());

        m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_cmd->IASetVertexBuffers(0, 1, &m_scene.vbv);
        m_cmd->IASetIndexBuffer(&m_scene.ibv);

        D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = m_scene.srvHeap->GetGPUDescriptorHandleForHeapStart();
        m_cmd->SetGraphicsRootDescriptorTable(3, gpuBase); // unused slots (white at 0)
        m_cmd->SetGraphicsRootDescriptorTable(4, gpuBase);
        m_cmd->SetGraphicsRootDescriptorTable(5, gpuBase);
        m_cmd->SetGraphicsRootDescriptorTable(6, gpuBase);
        for (const auto& sm : m_scene.submeshes) {
            D3D12_GPU_DESCRIPTOR_HANDLE h = gpuBase;
            h.ptr += static_cast<UINT64>(sm.srvHeapIndex) * m_scene.srvDescriptorSize;
            m_cmd->SetGraphicsRootDescriptorTable(1, h);
            m_cmd->DrawIndexedInstanced(sm.indexCount, 1, sm.indexStart, 0, 0);
        }
    }

    // -------------------- MMD character (shares the G-buffer, no-cull PSO) --------------------
    // Only Cel-profile characters go through the deferred G-buffer here; EndfieldPBR characters are
    // drawn later in their own forward pass (see below), so skip them entirely in the G-buffer.
    if (m_mmd.profile == RenderProfile::Cel && !m_mmd.submeshes.empty() && m_mmd.srvHeap) {
        m_cmd->SetGraphicsRootSignature(m_geometryRS.Get());

        // X-ray reveal: inside a screen circle around the character, reset the scene depth at the
        // character's silhouette (depth-only pass, DepthFunc=ALWAYS, dithered) so the character
        // draw below shows THROUGH an occluding building there. Outside the circle / dithered-out,
        // nothing is reset → normal occlusion. The character keeps correct self-occlusion + true
        // depth (LESS_EQUAL over the reset depth), so its deferred lighting/shadows stay correct.
        if (m_charXray && m_charXrayDepthPSO) {
            // Project the character's centre + top to screen pixels to size the reveal window.
            const XMFLOAT3& bmn = m_mmd.boundsMin; const XMFLOAT3& bmx = m_mmd.boundsMax;
            const XMMATRIX worldM = XMLoadFloat4x4(&m_mmd.world);
            const XMVECTOR cM = XMVectorSet(0.5f*(bmn.x+bmx.x), 0.5f*(bmn.y+bmx.y), 0.5f*(bmn.z+bmx.z), 1.0f);
            const XMVECTOR tM = XMVectorSet(0.5f*(bmn.x+bmx.x), bmx.y,              0.5f*(bmn.z+bmx.z), 1.0f);
            const XMVECTOR cClip = XMVector3Transform(XMVector3TransformCoord(cM, worldM), vpM);
            const XMVECTOR tClip = XMVector3Transform(XMVector3TransformCoord(tM, worldM), vpM);
            const float cw = XMVectorGetW(cClip);
            float cx = 0.0f, cy = 0.0f, radiusPx = 0.0f;
            if (cw > 0.001f) {   // in front of the camera
                cx = (XMVectorGetX(cClip)/cw * 0.5f + 0.5f) * static_cast<float>(m_width);
                cy = (1.0f - (XMVectorGetY(cClip)/cw * 0.5f + 0.5f)) * static_cast<float>(m_height);
                const float tw = XMVectorGetW(tClip);
                if (tw > 0.001f) {
                    const float tx = (XMVectorGetX(tClip)/tw * 0.5f + 0.5f) * static_cast<float>(m_width);
                    const float ty = (1.0f - (XMVectorGetY(tClip)/tw * 0.5f + 0.5f)) * static_cast<float>(m_height);
                    const float halfH = std::sqrt((tx-cx)*(tx-cx) + (ty-cy)*(ty-cy));
                    radiusPx = std::max(halfH * m_xrayRadiusScale, 24.0f);
                }
            }
            m_cmd->SetPipelineState(m_charXrayDepthPSO.Get());
            m_cmd->SetGraphicsRootConstantBufferView(0, cbAddr);
            m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_cmd->IASetVertexBuffers(0, 1, &m_mmd.vbv);
            m_cmd->IASetIndexBuffer(&m_mmd.ibv);
            auto resetCb = m_graphicsMemory->AllocateConstant<PerObjectCB>();
            PerObjectCB* ro = static_cast<PerObjectCB*>(resetCb.Memory());
            ro->world = m_mmd.world;   // VS only needs world + the per-frame viewProj for SV_POSITION
            ro->view  = viewStore;
            ro->xrayReveal   = { cx, cy, radiusPx, std::max(radiusPx * 0.22f, 8.0f) };  // feather = 22% of radius
            ro->xrayStrength = m_xrayStrength;
            m_cmd->SetGraphicsRootConstantBufferView(2, resetCb.GpuAddress());
            // radiusPx==0 (character behind the camera) → PSXrayReveal discards everything → no reveal.
            if (radiusPx > 0.0f)
                for (const auto& sm : m_mmd.submeshes)
                    if (!sm.isEffDecal) m_cmd->DrawIndexedInstanced(sm.indexCount, 1, sm.indexStart, 0, 0);
        }

        m_cmd->SetPipelineState(m_geometryPSONoCull.Get());

        ID3D12DescriptorHeap* heaps[] = { m_mmd.srvHeap.Get() };
        m_cmd->SetDescriptorHeaps(1, heaps);

        m_cmd->SetGraphicsRootConstantBufferView(0, cbAddr);
        m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_cmd->IASetVertexBuffers(0, 1, &m_mmd.vbv);
        m_cmd->IASetIndexBuffer(&m_mmd.ibv);

        D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = m_mmd.srvHeap->GetGPUDescriptorHandleForHeapStart();
        auto srvHandle = [&](UINT slot) {
            D3D12_GPU_DESCRIPTOR_HANDLE h = gpuBase;
            h.ptr += static_cast<UINT64>(slot) * m_mmd.srvDescriptorSize;
            return h;
        };
        // No sphere/face-mask textures — the PMX references none. Bind the white fallback
        // (slot 0) to the unused t1/t3/t4 tables so the root signature stays satisfied.
        m_cmd->SetGraphicsRootDescriptorTable(5, srvHandle(0u));
        m_cmd->SetGraphicsRootDescriptorTable(6, srvHandle(0u));

        // Expression eye-swap: pick which eye texture (A/B/C) the eyes use this frame.
        const int eyeExpr = m_mmd.eyeSwapAvailable ? CurrentEyeExpr() : 0;

        auto drawSubmesh = [&](const Submesh& sm) {
            auto objCb = m_graphicsMemory->AllocateConstant<PerObjectCB>();
            PerObjectCB* obj = static_cast<PerObjectCB*>(objCb.Memory());
            obj->world        = m_mmd.world;
            obj->materialId   = sm.isSkin ? 2u : 1u;  // 2 = skin (cel + SSS), 1 = cloth/other cel
            obj->useFaceMask  = 0;
            // PBR game models ship a "_N" normal map beside the "_D" diffuse; when present (and the
            // GUI toggle is on) perturb the G-buffer normal with it for surface detail the flat
            // diffuse-only render misses. Traditional MMD models have no _N → normalSrvIndex 0.
            const bool useNM  = m_charNormalMap && sm.normalSrvIndex != 0;
            obj->useNormalMap = useNM ? 1u : 0u;
            obj->satBoost         = sm.isEye ? 1.0f : m_charSat;  // eyes keep their authored colour
            obj->contrast         = m_charContrast;
            obj->view             = viewStore;
            obj->useSphere        = 0;
            obj->faceMaskStrength = 0.0f;
            obj->sphereStrength   = 0.0f;
            m_cmd->SetGraphicsRootConstantBufferView(2, objCb.GpuAddress());
            const UINT diffuse = (sm.isEye && eyeExpr != 0) ? m_mmd.eyeSrv[eyeExpr] : sm.srvHeapIndex;
            m_cmd->SetGraphicsRootDescriptorTable(1, srvHandle(diffuse));
            m_cmd->SetGraphicsRootDescriptorTable(3, srvHandle(0u));
            m_cmd->SetGraphicsRootDescriptorTable(4, srvHandle(useNM ? sm.normalSrvIndex : 0u));
            m_cmd->DrawIndexedInstanced(sm.indexCount, 1, sm.indexStart, 0, 0);
        };

        // Body submeshes (V-flip undone at load → eyes/brows layer naturally). The Eff facial
        // decals are skipped here and drawn later in a forward transparent pass so they blend
        // over the lit scene (semi-transparent: eyes show through) instead of overwriting it.
        for (const auto& sm : m_mmd.submeshes)
            if (!sm.isEffDecal) drawSubmesh(sm);
    }

    // -------------------- Geometry → Lighting transitions --------------------
    barrier(m_normalRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barrier(m_albedoRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // Depth is read by the SSAO/lighting pixel shaders AND the light-cull compute shader, so
    // make it readable in both stages (PIXEL | NON_PIXEL shader resource).
    barrier(m_depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // ============================== SSAO (depth+normal → occlusion → blur) ==============================
    {
        SsaoCB s{};
        s.radius = m_ssaoRadius; s.bias = m_ssaoBias; s.intensity = m_ssaoIntensity;
        s.enabled = m_ssaoEnabled ? 1u : 0u;
        s.screenX = (float)m_rw; s.screenY = (float)m_rh;   // SSAO runs at the supersampled size
        auto scb = m_graphicsMemory->AllocateConstant<SsaoCB>();
        *static_cast<SsaoCB*>(scb.Memory()) = s;

        ID3D12DescriptorHeap* heaps[] = { m_gbufferSrvHeap.Get() };
        m_cmd->SetDescriptorHeaps(1, heaps);
        m_cmd->SetGraphicsRootSignature(m_ssaoRS.Get());
        m_cmd->RSSetViewports(1, &vp);
        m_cmd->RSSetScissorRects(1, &scrt);
        m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_cmd->IASetVertexBuffers(0, 0, nullptr);
        m_cmd->IASetIndexBuffer(nullptr);

        // Occlusion: depth(t0)+normal(t1)+noise(t2) → ssaoRT
        D3D12_CPU_DESCRIPTOR_HANDLE ssaoRtv = rtvAt(kSsaoRtvIndex);
        m_cmd->OMSetRenderTargets(1, &ssaoRtv, FALSE, nullptr);
        m_cmd->SetPipelineState(m_ssaoPSO.Get());
        m_cmd->SetGraphicsRootConstantBufferView(0, cbAddr);
        m_cmd->SetGraphicsRootConstantBufferView(1, scb.GpuAddress());
        m_cmd->SetGraphicsRootDescriptorTable(2, srvAt(0));            // depth  (t0)
        m_cmd->SetGraphicsRootDescriptorTable(3, srvAt(1));            // normal (t1)
        m_cmd->SetGraphicsRootDescriptorTable(4, srvAt(kSrvNoise));    // noise  (t2)
        m_cmd->DrawInstanced(3, 1, 0, 0);
        barrier(m_ssaoRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Blur: ssaoRT(t0) → ssaoBlurRT
        D3D12_CPU_DESCRIPTOR_HANDLE blurRtv = rtvAt(kSsaoBlurRtvIndex);
        m_cmd->OMSetRenderTargets(1, &blurRtv, FALSE, nullptr);
        m_cmd->SetPipelineState(m_ssaoBlurPSO.Get());
        m_cmd->SetGraphicsRootConstantBufferView(0, cbAddr);
        m_cmd->SetGraphicsRootConstantBufferView(1, scb.GpuAddress());
        m_cmd->SetGraphicsRootDescriptorTable(2, srvAt(kSrvSsao));     // occlusion (t0)
        m_cmd->SetGraphicsRootDescriptorTable(3, srvAt(kSrvSsao));     // unused (t1) — valid, not the RTV
        m_cmd->SetGraphicsRootDescriptorTable(4, srvAt(kSrvNoise));    // unused (t2)
        m_cmd->DrawInstanced(3, 1, 0, 0);
        barrier(m_ssaoBlurRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // ============================== FORWARD+ LIGHT CULLING (compute) ==============================
    // Upload the (animated) point lights into one transient buffer read by BOTH the cull
    // compute (t0) and the lighting pass (t5).
    auto lightAlloc = m_graphicsMemory->Allocate(sizeof(PointLightGPU) * kNumPointLights);
    std::memcpy(lightAlloc.Memory(), m_pointLights.data(), sizeof(PointLightGPU) * kNumPointLights);
    const D3D12_GPU_VIRTUAL_ADDRESS lightVA = lightAlloc.GpuAddress();

    if (m_forwardPlus) {
        auto ccb = m_graphicsMemory->AllocateConstant<CullCB>();
        CullCB* c = static_cast<CullCB*>(ccb.Memory());
        XMStoreFloat4x4(&c->invViewProj, invVpM);
        c->cameraPos  = cameraPos;
        c->numLights  = kNumPointLights;
        c->screenSize = { m_rw, m_rh };     // cull grid tracks the supersampled depth buffer
        c->tileCount  = m_tileCount;

        ID3D12DescriptorHeap* cheaps[] = { m_gbufferSrvHeap.Get() };
        m_cmd->SetDescriptorHeaps(1, cheaps);
        m_cmd->SetComputeRootSignature(m_lightCullRS.Get());
        m_cmd->SetPipelineState(m_lightCullPSO.Get());
        m_cmd->SetComputeRootConstantBufferView(0, ccb.GpuAddress());
        m_cmd->SetComputeRootShaderResourceView(1, lightVA);
        m_cmd->SetComputeRootUnorderedAccessView(2, m_tileLightBuffer->GetGPUVirtualAddress());
        m_cmd->SetComputeRootDescriptorTable(3, srvAt(0));   // depth (t1)
        m_cmd->Dispatch(m_tileCount.x, m_tileCount.y, 1);
    }
    // Move the per-tile lists to a readable state for the lighting pass. Done unconditionally
    // so the resource state stays consistent if Forward+ is toggled (the cull writes, when
    // they ran, are made visible by this transition barrier).
    barrier(m_tileLightBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // ============================== LIGHTING PASS → HDR ==============================
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHdr = rtvAt(kSceneHdrRtvIndex);
    m_cmd->OMSetRenderTargets(1, &rtvHdr, FALSE, nullptr);
    static constexpr float kClearHdr[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // match sceneHDR's optimized clear
    m_cmd->ClearRenderTargetView(rtvHdr, kClearHdr, 0, nullptr);
    m_cmd->RSSetViewports(1, &vp);
    m_cmd->RSSetScissorRects(1, &scrt);

    m_cmd->SetPipelineState(m_lightingPSO.Get());
    m_cmd->SetGraphicsRootSignature(m_lightingRS.Get());
    ID3D12DescriptorHeap* lheaps[] = { m_gbufferSrvHeap.Get() };
    m_cmd->SetDescriptorHeaps(1, lheaps);
    m_cmd->SetGraphicsRootConstantBufferView(0, cbAddr);
    m_cmd->SetGraphicsRootDescriptorTable(1, srvAt(0));
    m_cmd->SetGraphicsRootDescriptorTable(2, srvAt(kSrvShadow));
    m_cmd->SetGraphicsRootDescriptorTable(3, srvAt(kSrvSsaoBlur));

    // Forward+ bindings: ForwardPlusCB (b1) + light buffer (t5) + per-tile lists (t6).
    {
        auto fcb = m_graphicsMemory->AllocateConstant<ForwardPlusCB>();
        ForwardPlusCB* f = static_cast<ForwardPlusCB*>(fcb.Memory());
        f->tileCount    = m_tileCount;
        f->numLights    = kNumPointLights;
        f->enabled      = m_forwardPlus ? 1u : 0u;
        f->debugHeat    = (m_forwardPlus && m_fpDebugHeat) ? 1u : 0u;
        f->pointEnabled = 0u;
        f->dirEnabled   = m_dirLightOn ? 1u : 0u;
        f->numShadowed  = drawPointShadows ? kNumShadowedLights : 0u;
        f->sssColor = m_sssColor; f->sssStrength = m_sssStrength;
        f->specInt = m_specInt; f->specPow = m_specPow; f->sssWrap = m_sssWrap;
        f->skinFresnel = m_skinFresnel;
        m_cmd->SetGraphicsRootConstantBufferView(4, fcb.GpuAddress());
        m_cmd->SetGraphicsRootShaderResourceView(5, lightVA);
        m_cmd->SetGraphicsRootShaderResourceView(6, m_tileLightBuffer->GetGPUVirtualAddress());
        m_cmd->SetGraphicsRootDescriptorTable(7, srvAt(kSrvPointCube));   // point-shadow cube (t7)
    }

    m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmd->IASetVertexBuffers(0, 0, nullptr);
    m_cmd->IASetIndexBuffer(nullptr);
    m_cmd->DrawInstanced(3, 1, 0, 0);

    // Return the per-tile lists to UAV for next frame's cull dispatch.
    barrier(m_tileLightBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // ============================== BLOOM SPHERE (point-light marker) ==============================
    // Drawn into sceneHDR (still RTV) before bloom so it glows; depth-tested against the scene
    // so geometry occludes it. This also leaves depth in DEPTH_WRITE for next frame's geometry.
    barrier(m_depth.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
    // One small glowing sphere per point light (instanced from a light buffer) marks where
    // every light is. The 128 coloured Forward+ lights each get one; the shadow-casting point
    // light gets one too (a 1-element buffer).
    auto drawLightSpheres = [&](D3D12_GPU_VIRTUAL_ADDRESS lights, UINT count,
                                float radius, float emissiveScale) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHdr2 = rtvAt(kSceneHdrRtvIndex);
        D3D12_CPU_DESCRIPTOR_HANDLE sdsv0   = m_dsvHeap->GetCPUDescriptorHandleForHeapStart(); // slot 0
        m_cmd->OMSetRenderTargets(1, &rtvHdr2, FALSE, &sdsv0);
        m_cmd->RSSetViewports(1, &vp);
        m_cmd->RSSetScissorRects(1, &scrt);
        m_cmd->SetPipelineState(m_spherePSO.Get());
        m_cmd->SetGraphicsRootSignature(m_sphereRS.Get());
        auto scb = m_graphicsMemory->AllocateConstant<SphereCB>();
        SphereCB* s = static_cast<SphereCB*>(scb.Memory());
        XMStoreFloat4x4(&s->viewProj, vpM);
        s->radius = radius; s->emissiveScale = emissiveScale;
        m_cmd->SetGraphicsRootConstantBufferView(0, scb.GpuAddress());
        m_cmd->SetGraphicsRootShaderResourceView(1, lights);
        m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_cmd->IASetVertexBuffers(0, 1, &m_sphereVBV);
        m_cmd->IASetIndexBuffer(&m_sphereIBV);
        m_cmd->DrawIndexedInstanced(m_sphereIndexCount, count, 0, 0, 0);
    };
    // Depth / Normal / CharDepth are raw buffer read-outs, not pictures: a light marker or a blush
    // decal blended over them is corruption, and (further down) so is running FXAA on them — an
    // edge-blurring filter on a depth map invents in-between depths right where the silhouette is.
    const bool bufferDebugView = (m_view == ViewMode::Depth || m_view == ViewMode::Normal ||
                                  m_view == ViewMode::CharDepth);
    if (m_forwardPlus && !bufferDebugView)
        drawLightSpheres(lightVA, kNumPointLights, 9.0f, 4.0f);   // marker at every coloured light

    // ===================== CHARACTER FACIAL DECALS (forward, alpha-blended) =====================
    // The Eff decals (blush / sweat / blue-face / tears) blend over the lit sceneHDR by their
    // own alpha → semi-transparent (the eyes show through) and emissive (true texture colour).
    if (!bufferDebugView && !m_mmd.submeshes.empty() && m_mmd.srvHeap) {
        bool anyDecal = false;
        for (const auto& sm : m_mmd.submeshes) if (sm.isEffDecal) { anyDecal = true; break; }
        if (anyDecal) {
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHdrD = rtvAt(kSceneHdrRtvIndex);
            D3D12_CPU_DESCRIPTOR_HANDLE dsv0    = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
            m_cmd->OMSetRenderTargets(1, &rtvHdrD, FALSE, &dsv0);
            m_cmd->RSSetViewports(1, &vp);
            m_cmd->RSSetScissorRects(1, &scrt);
            m_cmd->SetGraphicsRootSignature(m_geometryRS.Get());
            ID3D12DescriptorHeap* dheaps[] = { m_mmd.srvHeap.Get() };
            m_cmd->SetDescriptorHeaps(1, dheaps);
            m_cmd->SetGraphicsRootConstantBufferView(0, cbAddr);
            m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_cmd->IASetVertexBuffers(0, 1, &m_mmd.vbv);
            m_cmd->IASetIndexBuffer(&m_mmd.ibv);
            D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = m_mmd.srvHeap->GetGPUDescriptorHandleForHeapStart();
            auto srvH = [&](UINT slot) {
                D3D12_GPU_DESCRIPTOR_HANDLE h = gpuBase;
                h.ptr += static_cast<UINT64>(slot) * m_mmd.srvDescriptorSize; return h;
            };
            m_cmd->SetGraphicsRootDescriptorTable(5, srvH(0u));
            m_cmd->SetGraphicsRootDescriptorTable(6, srvH(0u));
            // useDiffuse = bind the diffuse (pass 1, alpha-blend) vs the emission (pass 2, add).
            auto drawDecals = [&](ID3D12PipelineState* pso, bool useDiffuse) {
                m_cmd->SetPipelineState(pso);
                for (const auto& sm : m_mmd.submeshes) {
                    if (!sm.isEffDecal) continue;
                    auto oc = m_graphicsMemory->AllocateConstant<PerObjectCB>();
                    PerObjectCB* o = static_cast<PerObjectCB*>(oc.Memory());
                    o->world = m_mmd.world; o->materialId = 1; o->useFaceMask = 0; o->useNormalMap = 0;
                    o->satBoost = 1.0f; o->contrast = 1.0f; o->view = viewStore; o->useSphere = 0;
                    o->faceMaskStrength = 0.0f; o->sphereStrength = 0.0f;
                    m_cmd->SetGraphicsRootConstantBufferView(2, oc.GpuAddress());
                    m_cmd->SetGraphicsRootDescriptorTable(1, srvH(useDiffuse ? sm.srvHeapIndex : sm.emissiveSrvIndex));
                    m_cmd->SetGraphicsRootDescriptorTable(3, srvH(0u));
                    m_cmd->SetGraphicsRootDescriptorTable(4, srvH(0u));
                    m_cmd->DrawIndexedInstanced(sm.indexCount, 1, sm.indexStart, 0, 0);
                }
            };
            drawDecals(m_decalPSO.Get(),         true);   // diffuse base / brows (multiply darken/tint)
            drawDecals(m_decalEmissivePSO.Get(), false);  // emission glow colour (additive)
        }
    }

    // ===================== ENDFIELD CHARACTER (forward NPR+PBR pass) =====================
    // Drawn over the lit scene into sceneHDR, depth-tested + writing against the scene depth so it
    // occludes correctly against Sponza and self-occludes. Its own shader does the toon/PBR shading
    // (Milestone 1: unlit BaseColor + per-channel debug). Writes alpha 1 → the dataset cut-out
    // coverage still works. Non-Cel (EndfieldPBR/WuwaPBR) characters were skipped in the deferred
    // G-buffer above. WuwaPBR currently reuses this Endfield forward shader as a placeholder until
    // its dedicated shading model is built.
    if (m_mmd.profile != RenderProfile::Cel && !m_mmd.submeshes.empty() &&
        m_mmd.srvHeap && m_endfieldPSO) {
        // Outline scale from the character's ON-SCREEN height: full when large, fading to 0 when
        // small so a far/tiny character drops the outline entirely (no disproportionate thick or
        // aliased line). Project the model-space top & bottom of the bounds through the active vp.
        float outlineScale = 1.0f;
        {
            const XMFLOAT3& bmn = m_mmd.boundsMin; const XMFLOAT3& bmx = m_mmd.boundsMax;
            const XMMATRIX worldM = XMLoadFloat4x4(&m_mmd.world);
            const float cxm = 0.5f * (bmn.x + bmx.x), czm = 0.5f * (bmn.z + bmx.z);
            auto screenY = [&](float my, float& outW) -> float {
                XMVECTOR wp = XMVector3TransformCoord(XMVectorSet(cxm, my, czm, 1.0f), worldM);
                XMVECTOR c  = XMVector4Transform(XMVectorSetW(wp, 1.0f), vpM);
                outW = XMVectorGetW(c);
                return (outW > 1e-3f) ? (1.0f - (XMVectorGetY(c) / outW * 0.5f + 0.5f)) * m_height : 0.0f;
            };
            float tw = 0.0f, bw = 0.0f;
            const float ty = screenY(bmx.y, tw), by = screenY(bmn.y, bw);
            if (tw > 1e-3f && bw > 1e-3f) {
                const float heightPx = std::fabs(by - ty);
                // PROPORTIONAL: outline width tracks the character's on-screen height, so a far/small
                // character gets a proportionally thinner line (constant relative thickness), reaching
                // full width when the character is ~m_outlineRefFrac of the screen. Hard-cut tiny sizes
                // to 0 so no sub-pixel aliased line remains.
                const float refPx = std::max(1.0f, m_height * m_outlineRefFrac);
                outlineScale = std::clamp(heightPx / refPx, 0.0f, 1.0f);
                if (heightPx < 24.0f) outlineScale = 0.0f;
            }
        }
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHdrE = rtvAt(kSceneHdrRtvIndex);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv0    = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        m_cmd->OMSetRenderTargets(1, &rtvHdrE, FALSE, &dsv0);
        m_cmd->RSSetViewports(1, &vp);
        m_cmd->RSSetScissorRects(1, &scrt);
        m_cmd->SetGraphicsRootSignature(m_endfieldRS.Get());
        ID3D12DescriptorHeap* eheaps[] = { m_mmd.srvHeap.Get() };
        m_cmd->SetDescriptorHeaps(1, eheaps);
        m_cmd->SetGraphicsRootConstantBufferView(0, cbAddr);   // PerFrameCB (viewProj/camera/light)
        {   // b2 material/look — same for every submesh, set once
            auto mc = m_graphicsMemory->AllocateConstant<EndfieldMaterialCB>();
            EndfieldMaterialCB* m = static_cast<EndfieldMaterialCB*>(mc.Memory());
            m->metalChan = m_efMetalChan; m->roughChan = m_efRoughChan; m->invertRough = m_efInvertRough;
            m->specStrength = m_efSpec;   m->roughBias = m_efRoughBias;
            m->rimStrength = m_efRim;     m->rimPower = m_efRimPow; m->emissStrength = m_efEmiss;
            m->useNormalMap = (m_efNormalMap && m_charNormalMap) ? 1.0f : 0.0f;
            m->hairStrength = m_efHair;
            m->normalYSign  = m_efFlipNormalY ? -1.0f : 1.0f;
            m->shadowStrength = m_dirLightOn ? m_efShadowStr : 0.0f;
            m->rimColor = m_efRimColor;
            m->shadowDepth = m_efShadowDepth;
            const bool wuwap = (m_mmd.profile == RenderProfile::WuwaPBR);
            // Row5 slot0 (ZZZ's "deepen") is reused by Wuwa as its shadow-tint amount — fed per profile
            // so ZZZ still gets its grade and Wuwa gets its own value (Endfield ignores the slot).
            m->deepen = wuwap ? m_wuwaShadowTint : m_zzzDeepen;
            m->warmth = m_zzzWarmth; m->eyeLift = m_zzzEyeLift;
            m->wuwaExposure = wuwap ? m_wuwaExposure : 1.0f;  // Wuwa-only
            // The tone / spec-focus / leather-sheen tools are ENDFIELD-ONLY — feed 0 for every other
            // profile so ZZZ (which reads specFocus/charShadows/charHighlights) is NOT affected by
            // Endfield tuning (this was the "ZZZ 變糟" leak). Wuwa doesn't read them either way.
            const bool efp = (m_mmd.profile == RenderProfile::EndfieldPBR);
            m->charShadows = efp ? m_shadows : 0.0f;
            m->charHighlights = efp ? m_highlights : 0.0f;
            m->specFocus = efp ? m_efSpecFocus : 0.0f;
            m->sheenStrength = efp ? m_efSheen : 0.0f;
            m->hairRange = m_efHairRange;
            m_cmd->SetGraphicsRootConstantBufferView(7, mc.GpuAddress());
        }
        m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_cmd->IASetVertexBuffers(0, 1, &m_mmd.vbv);
        m_cmd->IASetIndexBuffer(&m_mmd.ibv);
        D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = m_mmd.srvHeap->GetGPUDescriptorHandleForHeapStart();
        auto srvE = [&](UINT slot) {
            D3D12_GPU_DESCRIPTOR_HANDLE h = gpuBase;
            h.ptr += static_cast<UINT64>(slot) * m_mmd.srvDescriptorSize; return h;
        };
        m_cmd->SetGraphicsRootDescriptorTable(8, srvE(m_mmd.shadowSrvSlot));   // t5 shadow map
        // Endfield "full NPR" model-wide face maps t10/t11/t12 (SDF / colour-makeup / highlight). Bound
        // once; white for non-Endfield (the shared RS requires every table set before a draw).
        const bool efNPR = (m_mmd.profile == RenderProfile::EndfieldPBR);
        m_cmd->SetGraphicsRootDescriptorTable(13, srvE(efNPR ? m_mmd.faceSdfSrv : 0));   // t10 face SDF
        m_cmd->SetGraphicsRootDescriptorTable(14, srvE(efNPR ? m_mmd.faceCmSrv  : 0));   // t11 face cm
        m_cmd->SetGraphicsRootDescriptorTable(15, srvE(efNPR ? m_mmd.faceHlSrv  : 0));   // t12 face hl
        // phase: 0 = opaque submeshes, 1 = blended overlays (eye/hair shadow, matAlpha<1).
        auto drawEndfield = [&](ID3D12PipelineState* pso, int transparentMode) {
            m_cmd->SetPipelineState(pso);
            for (const auto& sm : m_mmd.submeshes) {
                const bool smTransparent = sm.matAlpha < 0.99f;
                if ((transparentMode != 0) != smTransparent) continue;   // draw only this phase's set
                auto oc = m_graphicsMemory->AllocateConstant<EndfieldObjectCB>();
                EndfieldObjectCB* o = static_cast<EndfieldObjectCB*>(oc.Memory());
                o->world         = m_mmd.world;
                o->debugMode     = m_endfieldDebug;
                o->outlineWidth  = m_endfieldOutline;
                o->toonThreshold = m_endfieldToonThresh;
                o->toonFeather   = m_endfieldToonFeather;
                o->screenSize    = { static_cast<float>(m_width), static_cast<float>(m_height) };
                const bool zzzp  = (m_mmd.profile == RenderProfile::ZzzNPR);
                o->hasPacked     = ((zzzp ? sm.srvToon : sm.srvPacked) != 0) ? 1 : 0;  // ZZZ: "has toon ramp"
                o->hasEmissive   = (sm.srvEmiss  != 0) ? 1 : 0;
                o->hasNormal     = (sm.normalSrvIndex != 0) ? 1 : 0;
                o->isHair        = sm.isHair ? 1 : 0;
                o->transparentMode = transparentMode;
                o->matClass      = sm.isMetal ? 4 : (sm.isSkin ? 1 : (sm.isHair ? 2 : (sm.isEye ? 3 : 0)));
                // sphereMode: ZZZ uses the real MMD sphere mode; for Endfield we REUSE this slot as a
                // "has matcap" flag (1 when a matcap was found → the shader does the reflection).
                o->sphereMode    = (m_mmd.profile == RenderProfile::ZzzNPR) ? sm.sphereMode
                                 : (m_mmd.profile == RenderProfile::EndfieldPBR && m_mmd.matcapSrv != 0 ? 1 : 0);
                o->matDiffuse    = sm.matDiffuse;
                o->matAlpha      = sm.matAlpha;
                o->outlineScale    = outlineScale;
                o->matcapStrength  = m_zzzMatcap;
                o->satBoost        = m_zzzSat;
                o->outlineDepthBias = m_outlineDepthBias;
                // Texture-colour fidelity: hand the shader the global tonemap knobs so it can
                // pre-invert exposure/ACES/vibrance and keep the character's painted albedo. Applied
                // to ZZZ AND Endfield (both suffer the 整體發白 whitening); Wuwa's shader has no block.
                o->postExposure    = m_exposure;
                o->postVibrance    = m_vibrance;
                o->texFidelity     = zzzp ? m_zzzTexFidelity
                                   : (m_mmd.profile == RenderProfile::EndfieldPBR ? m_efFidelity
                                   : (m_mmd.profile == RenderProfile::WuwaPBR ? m_wuwaFidelity : 0.0f));
                o->nprMask = efNPR ? ((sm.srvRamp       ? 1   : 0) | (sm.srvSubsurf    ? 2   : 0) |
                                      (sm.srvLut        ? 4   : 0) | (sm.srvReflect    ? 8   : 0) |
                                      (sm.srvHairDetail ? 16  : 0) | (m_mmd.faceSdfSrv ? 32  : 0) |
                                      (m_mmd.faceCmSrv  ? 64  : 0) | (m_mmd.faceHlSrv  ? 128 : 0)) : 0;
                m_cmd->SetGraphicsRootConstantBufferView(1, oc.GpuAddress());
                // ZZZ uses the MMD toon ramp (t2) + sphere/MatCap (t3); Endfield/Wuwa use packed (t2)
                // + mask (t3). Same root slots, profile-appropriate textures.
                m_cmd->SetGraphicsRootDescriptorTable(2, srvE(sm.srvHeapIndex));   // t0 BaseColor
                m_cmd->SetGraphicsRootDescriptorTable(3, srvE(sm.normalSrvIndex)); // t1 Normal
                m_cmd->SetGraphicsRootDescriptorTable(4, srvE(zzzp ? sm.srvToon   : sm.srvPacked)); // t2
                // t3: ZZZ = sphere/MatCap, Wuwa = mask (unchanged). Endfield REUSES this slot for the
                // reflection matcap (model-wide) instead of the barely-used _M mask — ZZZ/Wuwa untouched.
                m_cmd->SetGraphicsRootDescriptorTable(5, srvE(
                    m_mmd.profile == RenderProfile::EndfieldPBR ? m_mmd.matcapSrv
                    : (zzzp ? sm.srvSphere : sm.srvMask)));   // t3
                m_cmd->SetGraphicsRootDescriptorTable(6, srvE(sm.srvEmiss));       // t4 Emissive
                // Endfield "full NPR" per-material maps t6..t9 + t13; white for non-Endfield so the
                // shared RS has every table set (Wuwa/ZZZ shaders simply don't read them).
                m_cmd->SetGraphicsRootDescriptorTable(9,  srvE(efNPR ? sm.srvRamp       : 0)); // t6 ramp
                m_cmd->SetGraphicsRootDescriptorTable(10, srvE(efNPR ? sm.srvSubsurf    : 0)); // t7 subsurf
                m_cmd->SetGraphicsRootDescriptorTable(11, srvE(efNPR ? sm.srvLut        : 0)); // t8 lut
                m_cmd->SetGraphicsRootDescriptorTable(12, srvE(efNPR ? sm.srvReflect    : 0)); // t9 reflect
                m_cmd->SetGraphicsRootDescriptorTable(16, srvE(efNPR ? sm.srvHairDetail : 0)); // t13 hair
                m_cmd->DrawIndexedInstanced(sm.indexCount, 1, sm.indexStart, 0, 0);
            }
        };
        // Pick the shader set by profile (each has its own; falls back to Endfield if unbuilt).
        ID3D12PipelineState* mainPSO    = m_endfieldPSO.Get();
        ID3D12PipelineState* outlinePSO = m_endfieldOutlinePSO.Get();
        ID3D12PipelineState* blendPSO   = m_endfieldBlendPSO.Get();
        if (m_mmd.profile == RenderProfile::WuwaPBR && m_wuwaPSO) {
            mainPSO = m_wuwaPSO.Get(); outlinePSO = m_wuwaOutlinePSO.Get(); blendPSO = m_wuwaBlendPSO.Get();
        } else if (m_mmd.profile == RenderProfile::ZzzNPR && m_zzzPSO) {
            mainPSO = m_zzzPSO.Get();  outlinePSO = m_zzzOutlinePSO.Get();  blendPSO = m_zzzBlendPSO.Get();
        }

        // Opaque phase: outline (expanded back hull) → toon-shaded main. Then the semi-transparent
        // eye-/hair-shadow overlays are alpha-blended on top (no outline, no depth write). Debug
        // channel views skip the outline + overlays for a clean read.
        if (m_endfieldOutline > 0.0f && outlinePSO && m_endfieldDebug == 0)
            drawEndfield(outlinePSO, 0);
        drawEndfield(mainPSO, 0);
        if (blendPSO && m_endfieldDebug == 0)
            drawEndfield(blendPSO, 1);
    }

    // ============================== POST: BLOOM + TONE MAP ==============================
    // Bloom works on the SUPERSAMPLED sceneHDR, so its half-res buffers track the SS size.
    const UINT hw = (m_rw + 1) / 2, hh = (m_rh + 1) / 2;
    D3D12_VIEWPORT vpHalf{ 0, 0, (float)hw, (float)hh, 0.0f, 1.0f };
    D3D12_RECT     scHalf{ 0, 0, (LONG)hw, (LONG)hh };

    // A small full-screen-triangle post draw with the shared PostRS.
    auto postDraw = [&](ID3D12PipelineState* pso, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                        D3D12_GPU_DESCRIPTOR_HANDLE srcA, D3D12_GPU_DESCRIPTOR_HANDLE srcB,
                        const D3D12_VIEWPORT& v, const D3D12_RECT& s, const PostCB& cbData) {
        auto pcb = m_graphicsMemory->AllocateConstant<PostCB>();
        *static_cast<PostCB*>(pcb.Memory()) = cbData;
        m_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        m_cmd->RSSetViewports(1, &v);
        m_cmd->RSSetScissorRects(1, &s);
        m_cmd->SetPipelineState(pso);
        m_cmd->SetGraphicsRootSignature(m_postRS.Get());
        // Every handle below lives in the G-buffer SRV heap, but the passes that ran just before
        // (facial decals / Endfield forward) leave the MMD heap bound — binding a table from a heap
        // that isn't current is a D3D12 ERROR, so make this pass state-independent.
        ID3D12DescriptorHeap* postHeaps[] = { m_gbufferSrvHeap.Get() };
        m_cmd->SetDescriptorHeaps(1, postHeaps);
        m_cmd->SetGraphicsRootDescriptorTable(0, srcA);
        m_cmd->SetGraphicsRootDescriptorTable(1, srcB);
        m_cmd->SetGraphicsRootConstantBufferView(2, pcb.GpuAddress());
        m_cmd->IASetVertexBuffers(0, 0, nullptr);
        m_cmd->IASetIndexBuffer(nullptr);
        m_cmd->DrawInstanced(3, 1, 0, 0);
    };

    barrier(m_sceneHDR.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    PostCB pc{};
    pc.exposure      = m_exposure;
    pc.bloomStrength = m_bloomEnabled ? m_bloomStrength : 0.0f;
    pc.threshold     = m_bloomThreshold;
    pc.viewMode      = static_cast<UINT>(m_view);
    pc.vibrance      = m_vibrance;
    pc.isolated      = m_captureIsolated ? 1u : 0u;
    pc.fxaaSubpix    = m_fxaaStrength;

    // Bright pass: sceneHDR -> bloom0 (half res).
    pc.vertical = 0; pc.invTexelX = 1.0f / m_rw; pc.invTexelY = 1.0f / m_rh;
    postDraw(m_brightPSO.Get(), rtvAt(kBloom0RtvIndex), srvAt(kSrvSceneHdr), srvAt(kSrvSceneHdr),
             vpHalf, scHalf, pc);
    barrier(m_bloom0.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Blur horizontal: bloom0 -> bloom1.
    pc.vertical = 0; pc.invTexelX = 1.0f / hw; pc.invTexelY = 1.0f / hh;
    postDraw(m_blurPSO.Get(), rtvAt(kBloom1RtvIndex), srvAt(kSrvBloom0), srvAt(kSrvBloom0),
             vpHalf, scHalf, pc);
    barrier(m_bloom1.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Blur vertical: bloom1 -> bloom0.
    barrier(m_bloom0.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    pc.vertical = 1;
    postDraw(m_blurPSO.Get(), rtvAt(kBloom0RtvIndex), srvAt(kSrvBloom1), srvAt(kSrvBloom1),
             vpHalf, scHalf, pc);
    barrier(m_bloom0.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Tonemap: sceneHDR + bloom0 -> back buffer (or -> LDR RT, then FXAA -> back buffer).
    barrier(m_backBuffers[m_frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // Tonemap + FXAA run at the WINDOW size (vpWin), sampling the supersampled sceneHDR/bloom via
    // the full-screen-triangle UVs — the bilinear fetch box-downsamples SS -> window (a 2x2 average
    // at 2x SSAA). FXAA then cleans any residual edges on the resolved image.
    const bool useFxaa = m_fxaa && m_fxaaPSO && m_ldrRT && !bufferDebugView;
    if (useFxaa) {
        postDraw(m_tonemapPSO.Get(), rtvAt(kLdrRtvIndex), srvAt(kSrvSceneHdr), srvAt(kSrvBloom0), vpWin, scrtWin, pc);
        barrier(m_ldrRT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        pc.invTexelX = 1.0f / m_width; pc.invTexelY = 1.0f / m_height;   // window-res neighbour taps
        postDraw(m_fxaaPSO.Get(), rtvBack, srvAt(kSrvLdr), srvAt(kSrvLdr), vpWin, scrtWin, pc);
        barrier(m_ldrRT.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else {
        postDraw(m_tonemapPSO.Get(), rtvBack, srvAt(kSrvSceneHdr), srvAt(kSrvBloom0), vpWin, scrtWin, pc);
    }

    // ImGui control panel, drawn straight onto the (LDR) back buffer.
    if (drawImGui) {
        m_cmd->OMSetRenderTargets(1, &rtvBack, FALSE, nullptr);
        ID3D12DescriptorHeap* ih[] = { m_imguiSrvHeap.Get() };
        m_cmd->SetDescriptorHeaps(1, ih);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_cmd.Get());
    }

    // -------------------- End-of-frame transitions --------------------
    barrier(m_normalRT.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(m_albedoRT.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // depth is already in DEPTH_WRITE (left there by the bloom-sphere stage)
    barrier(m_sceneHDR.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(m_bloom0.Get(),   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(m_bloom1.Get(),   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(m_ssaoRT.Get(),     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(m_ssaoBlurRT.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(m_backBuffers[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    ThrowIfFailed(m_cmd->Close());
    ID3D12CommandList* lists[] = { m_cmd.Get() };
    m_queue->ExecuteCommandLists(_countof(lists), lists);

    const UINT syncInterval = m_vsync ? 1u : 0u;
    const UINT presentFlags = (!m_vsync && m_tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    ThrowIfFailed(m_swap->Present(syncInterval, presentFlags));
    if (m_graphicsMemory) m_graphicsMemory->Commit(m_queue.Get());
    m_lastPresentedIndex = m_frameIndex;
    MoveToNextFrame();
    DrainInfoQueue();
}

void Renderer::DrainInfoQueue() {
#if defined(_DEBUG)
    if (!m_infoQueue) return;
    // Dedup so a recurring warning is reported once, not every frame.
    static std::unordered_set<std::string> s_seen;
    const UINT64 n = m_infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(m_infoQueue->GetMessage(i, nullptr, &len)) || len == 0) continue;
        std::vector<char> buf(len);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (FAILED(m_infoQueue->GetMessage(i, msg, &len))) continue;
        const char* sev =
            msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" :
            msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR      ? "ERROR"      :
            msg->Severity == D3D12_MESSAGE_SEVERITY_WARNING    ? "WARNING"    : "MESSAGE";
        std::string line = std::string("[D3D12 ") + sev + "] (" +
            std::to_string(static_cast<int>(msg->ID)) + ") " +
            std::string(msg->pDescription, msg->DescriptionByteLength
                ? msg->DescriptionByteLength - 1 : 0);
        if (s_seen.insert(line).second) {
            std::fprintf(stderr, "%s\n", line.c_str());
            std::fflush(stderr);
        }
    }
    m_infoQueue->ClearStoredMessages();
#endif
}

// UTF-8 (JSON/console/GUI labels) -> wide, for building Unicode filesystem paths.
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

void Renderer::ApplyDatasetStyle(int styleId, std::mt19937& rng) {
    // Baseline: a clean cel look; each style overrides from here. GenerateDataset snapshots and
    // restores the user's real settings around the whole run, so this may stomp freely.
    m_view         = ViewMode::Color;
    m_bloomEnabled = true;  m_ssaoEnabled = true;  m_dirLightOn = true;
    m_exposure     = 1.2f;  m_vibrance    = 1.25f;
    m_charSat      = 5.0f;  m_charContrast = 1.0f;  m_specInt = 0.30f;
    m_lightTint    = { 1.0f, 1.0f, 1.0f };
    m_lightDir     = { -0.577f, -0.577f, -0.577f };

    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    switch (static_cast<StyleId>(styleId)) {
        case StyleId::CelFull:      break;                                   // baseline
        case StyleId::FlatAlbedo:   m_view = ViewMode::Albedo;  m_bloomEnabled = false; break;
        case StyleId::ShadedNoPost: m_bloomEnabled = false; m_ssaoEnabled = false; m_vibrance = 1.0f; break;
        case StyleId::NormalMap:    m_view = ViewMode::Normal;  m_bloomEnabled = false; break;
        case StyleId::Depth:        m_view = ViewMode::Depth;   m_bloomEnabled = false; break;
        case StyleId::CharDepth:    m_view = ViewMode::CharDepth; m_bloomEnabled = false; m_ssaoEnabled = false; break;
        case StyleId::Outline:      m_view = ViewMode::Outline; m_bloomEnabled = false; break;
        case StyleId::HighSat:      m_charSat = 8.0f; m_charContrast = 1.25f; m_vibrance = 1.8f; m_exposure = 1.3f; break;
        case StyleId::LowKey:
            m_exposure = 0.6f; m_vibrance = 1.0f;
            m_lightTint = { 0.7f, 0.75f, 1.0f };            // cool, moody
            m_lightDir  = { -0.30f, -0.40f, -0.85f };
            break;
        case StyleId::RimLight:
            m_lightDir = { 0.20f, 0.50f, 0.85f };           // roughly behind -> backlit
            m_specInt  = 0.80f; m_exposure = 1.1f;
            break;
        case StyleId::RandomLight: {
            float az = u01(rng) * 6.2831853f;
            float el = 0.2f + u01(rng) * 1.0f;
            m_lightDir  = { std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az) };
            m_lightTint = { 0.6f + 0.4f * u01(rng), 0.6f + 0.4f * u01(rng), 0.6f + 0.4f * u01(rng) };
            break;
        }
        default: break;
    }
}

int Renderer::StyleCount() const { return static_cast<int>(StyleId::Count); }

void Renderer::PreviewStyle(int styleId) {
    if (styleId >= 0) {
        // Snapshot the user's look once, on the first activation, so "Off" can restore it.
        if (!m_stylePreviewActive) {
            m_styleSnap = { m_view, m_bloomEnabled, m_ssaoEnabled, m_dirLightOn,
                            m_exposure, m_vibrance, m_charSat, m_charContrast, m_specInt,
                            m_lightDir, m_lightTint };
            m_stylePreviewActive = true;
        }
        if (styleId >= static_cast<int>(StyleId::Count)) styleId = static_cast<int>(StyleId::Count) - 1;
        ApplyDatasetStyle(styleId, m_previewRng);
        m_stylePreview = styleId;
        std::printf("[style] preview: %s\n", StyleName(static_cast<StyleId>(styleId)));
        std::fflush(stdout);
    } else {
        if (m_stylePreviewActive) {   // restore the pre-preview look
            m_view = m_styleSnap.view; m_bloomEnabled = m_styleSnap.bloom;
            m_ssaoEnabled = m_styleSnap.ssao; m_dirLightOn = m_styleSnap.dir;
            m_exposure = m_styleSnap.exp; m_vibrance = m_styleSnap.vib;
            m_charSat = m_styleSnap.sat; m_charContrast = m_styleSnap.con; m_specInt = m_styleSnap.spec;
            m_lightDir = m_styleSnap.lightDir; m_lightTint = m_styleSnap.tint;
            m_stylePreviewActive = false;
        }
        m_stylePreview = -1;
        std::printf("[style] preview off (restored)\n");
        std::fflush(stdout);
    }
}

void Renderer::GenerateDataset(const DatasetConfig& cfgIn) {
    if (m_datasetBusy.exchange(true)) return;   // already running
    if (!m_animator || m_mmd.submeshes.empty()) {
        std::fprintf(stderr, "[dataset] no character loaded — nothing to capture.\n");
        std::fflush(stderr);
        m_datasetBusy.store(false);
        return;
    }
    namespace fs = std::filesystem;
    using clock = std::chrono::steady_clock;

    // Local copy so labels can be auto-filled from the loaded character / active clip when left at
    // their defaults (e.g. a console `dataset gen` without `dataset name`).
    DatasetConfig cfg = cfgIn;
    if (cfg.character == "char" && m_currentChar >= 0 && m_currentChar < (int)m_characters.size())
        cfg.character = m_characters[m_currentChar].name;
    if (cfg.motion == "motion" && m_currentClip >= 0 && m_currentClip < (int)m_motionClips.size())
        cfg.motion = m_motionClips[m_currentClip].name;

    // ---- snapshot user-facing state (restored at the end) ----
    const ViewMode sView = m_view;
    const bool  sBloom = m_bloomEnabled, sSsao = m_ssaoEnabled, sDir = m_dirLightOn;
    const bool  sFplus = m_forwardPlus, sHeat = m_fpDebugHeat, sPshadow = m_pointShadowOn;
    const bool  sXray = m_charXray, sCamMotion = m_camMotionOn, sImgui = m_imguiVisible;
    const bool  sPaused = m_animPaused.load(), sVsync = m_vsync, sIsolated = m_captureIsolated;
    const float sExp = m_exposure, sVib = m_vibrance, sSat = m_charSat, sCon = m_charContrast, sSpec = m_specInt;
    const XMFLOAT3 sLightDir = m_lightDir, sTint = m_lightTint;
    const UINT  sW = m_width, sH = m_height;
    const XMFLOAT3 sCamPos = m_camera.Position();
    const float sYaw = m_camera.Yaw(), sPitch = m_camera.Pitch();

    // The worker owns the animator; stop it so we can seek + skin deterministically here.
    StopAnimThread();
    m_camMotionOn = false;                       // use the free-fly camera we aim per view
    // Sponza's 128 point lights stay off for every capture: they belong to a scene the cut-out does
    // not show, and their contribution would depend on where the character happens to stand.
    // Measured on an Endfield character: enabling them moves the mean luminance by less than the
    // run-to-run pose jitter, so there is nothing to gain and determinism to lose.
    m_forwardPlus = false; m_fpDebugHeat = false; m_pointShadowOn = false; m_charXray = false;
    m_imguiVisible = false;                       // keep the control panel out of the shots
    m_vsync = false;                              // capture as fast as the GPU allows

    const int img = (cfg.imgSize > 0) ? cfg.imgSize : (int)sW;
    if ((UINT)img != m_width || (UINT)img != m_height) Resize((UINT)img, (UINT)img);

    // ---- time samples ----
    const double dur = m_animator->MotionDurationSeconds();
    double start = std::max(0.0, cfg.startSec);
    double end   = (cfg.endSec < 0.0) ? std::max(start, dur) : cfg.endSec;
    if (end < start) end = start;
    int frames = std::max(1, cfg.frameCount);
    if (dur <= 0.0) { frames = 1; start = end = 0.0; }   // no motion: a single bind pose

    // ---- styles ----
    std::vector<int> styles = cfg.styles;
    if (styles.empty()) for (int s = 0; s < (int)StyleId::Count; ++s) styles.push_back(s);

    // ---- backgrounds ----
    std::vector<bool> bgs;
    if (cfg.bgIsolated) bgs.push_back(true);
    if (cfg.bgScene)    bgs.push_back(false);
    if (bgs.empty())    bgs.push_back(true);

    // ---- output dirs (Unicode-safe; labels sanitized so a "/" or space in a name can't spawn
    //      stray subdirs — the real labels are still written verbatim into each JSON) ----
    auto sanitize = [](std::string s) {
        for (char& ch : s)
            if (std::strchr("/\\:*?\"<>|", ch) || (unsigned char)ch < 32 || ch == ' ') ch = '_';
        return s;
    };
    fs::path root = fs::path(cfg.outDir) / Utf8ToWide(sanitize(cfg.character))
                                         / Utf8ToWide(sanitize(cfg.motion));
    std::error_code ec; fs::create_directories(root, ec);
    const std::wstring manifest = (root / L"manifest.jsonl").wstring();

    std::mt19937 rng(cfg.seed);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);

    // Native = capture the character exactly as its own profile renders it, with the look that is
    // live right now. Every other style deliberately stomps exposure/saturation/light to a shared
    // baseline, which would flatten the per-profile tuning each model was set up with.
    auto applyStyle = [&](int st) {
        if (st != static_cast<int>(StyleId::Native)) { ApplyDatasetStyle(st, rng); return; }
        m_view         = ViewMode::Color;    // a picture, whatever debug view was on screen
        m_bloomEnabled = sBloom; m_ssaoEnabled = sSsao; m_dirLightOn = sDir;
        m_exposure = sExp; m_vibrance = sVib; m_charSat = sSat; m_charContrast = sCon;
        m_specInt  = sSpec; m_lightDir = sLightDir; m_lightTint = sTint;
    };

    // ---- view list: base hemisphere grid + optional truncated crop views ----
    struct ViewSpec { float az, el; BodyPart part; };
    std::vector<ViewSpec> views;
    const int azc = std::max(1, cfg.azimuthCount), elc = std::max(1, cfg.elevCount);
    for (int e = 0; e < elc; ++e) {
        float el = (elc <= 1) ? 0.5f * (cfg.elevMinDeg + cfg.elevMaxDeg)
                              : cfg.elevMinDeg + (cfg.elevMaxDeg - cfg.elevMinDeg) * e / (elc - 1);
        for (int a = 0; a < azc; ++a)
            views.push_back({ 360.0f * a / azc, el, BodyPart::FullBody });
    }
    if (cfg.enableCrops) {
        const int base = (int)views.size();
        const int ncrop = (int)std::lround(cfg.cropProb * base);
        const BodyPart parts[] = { BodyPart::UpperBody, BodyPart::Face, BodyPart::Arms,
                                   BodyPart::Legs, BodyPart::Torso };
        for (int c = 0; c < ncrop; ++c) {
            float az = u01(rng) * 360.0f;
            float el = cfg.elevMinDeg + (cfg.elevMaxDeg - cfg.elevMinDeg) * u01(rng);
            views.push_back({ az, el, parts[rng() % 5] });
        }
    }

    const int total = frames * (int)views.size() * (int)styles.size() * (int)bgs.size();
    m_datasetTotal.store(total);
    m_datasetDone.store(0);
    std::printf("[dataset] generating %d samples -> %ls  (%d frames x %zu views x %zu styles x %zu bg)\n",
                total, root.wstring().c_str(), frames, views.size(), styles.size(), bgs.size());
    std::fflush(stdout);

    const XMMATRIX worldM = XMLoadFloat4x4(&m_mmd.world);
    const float fov = m_camera.Fov();
    std::vector<MmdAnimator::BonePose> bones;
    std::vector<JointRecord> recs;
    int done = 0;
    auto t0 = clock::now();

    for (int fi = 0; fi < frames; ++fi) {
        const double t = (frames <= 1) ? start : start + (end - start) * fi / (frames - 1);

        // Deterministic pose: seek + CPU-skin on this thread (worker is stopped).
        m_animator->SeekTo(t);
        if (m_skinVertexCount == 0) m_skinVertexCount = m_animator->VertexCount();
        // Size BOTH halves of the skin double buffer: the worker (restarted after this run) writes
        // into m_skinBack, so an undersized back buffer would overflow the heap once it resumes.
        if (m_skinFront.size() != m_skinVertexCount) m_skinFront.assign(m_skinVertexCount, Vertex{});
        if (m_skinBack.size()  != m_skinVertexCount) m_skinBack.assign(m_skinVertexCount, Vertex{});
        m_animator->CopySkinnedVertices(m_skinFront.data());
        m_animHasFrame.store(true);
        m_animator->ExtractPose(bones);

        // World-space joint positions (same transform the mesh uses) + this frame's bbox.
        const int nb = (int)bones.size();
        std::vector<XMFLOAT3> wpos(nb);
        XMVECTOR bbMin = XMVectorReplicate(1e9f), bbMax = XMVectorReplicate(-1e9f);
        for (int b = 0; b < nb; ++b) {
            XMVECTOR wp = XMVector3TransformCoord(XMLoadFloat3(&bones[b].globalPos), worldM);
            XMStoreFloat3(&wpos[b], wp);
            bbMin = XMVectorMin(bbMin, wp); bbMax = XMVectorMax(bbMax, wp);
        }
        // Character height this frame — sets the depth slack of the joint-visibility test.
        const float bodyHeight = std::max(1.0f, XMVectorGetY(bbMax) - XMVectorGetY(bbMin));

        for (int vi = 0; vi < (int)views.size(); ++vi) {
            const ViewSpec& vs = views[vi];

            // Framing bbox: whole body, or the subset of joints in the crop part (limbs then fall
            // outside frame — natural truncation, with those joints marked out-of-frame).
            XMVECTOR pMin = bbMin, pMax = bbMax;
            if (vs.part != BodyPart::FullBody) {
                XMVECTOR mn = XMVectorReplicate(1e9f), mx = XMVectorReplicate(-1e9f);
                bool any = false;
                for (int b = 0; b < nb; ++b)
                    if (CanonicalInPart(CanonicalJointName(bones[b].name), vs.part)) {
                        XMVECTOR wp = XMLoadFloat3(&wpos[b]);
                        mn = XMVectorMin(mn, wp); mx = XMVectorMax(mx, wp); any = true;
                    }
                if (any) { pMin = mn; pMax = mx; }
            }
            XMVECTOR center = XMVectorScale(XMVectorAdd(pMin, pMax), 0.5f);
            float radius = 0.5f * XMVectorGetX(XMVector3Length(XMVectorSubtract(pMax, pMin)));
            if (radius < 1.0f) radius = 50.0f;
            const float dist = radius / std::sin(fov * 0.5f) * cfg.fitMargin;

            const float azr = XMConvertToRadians(vs.az), elr = XMConvertToRadians(vs.el);
            XMVECTOR dir = XMVectorSet(std::cos(elr) * std::sin(azr), std::sin(elr),
                                       std::cos(elr) * std::cos(azr), 0.0f);
            XMFLOAT3 eye, ctr;
            XMStoreFloat3(&eye, XMVectorAdd(center, XMVectorScale(dir, dist)));
            XMStoreFloat3(&ctr, center);
            m_camera.LookAt(eye, ctr);

            const XMMATRIX vpM   = m_camera.ViewProj();
            const XMMATRIX viewM = m_camera.View();

            // Project every joint into this view (shared across styles/backgrounds).
            recs.resize(nb);
            for (int b = 0; b < nb; ++b) {
                const auto& bp = bones[b]; JointRecord& r = recs[b];
                r.name = bp.name; r.canonical = CanonicalJointName(bp.name); r.parent = bp.parent;
                r.worldPos = wpos[b]; r.worldRot = bp.globalRot;
                r.localPos = bp.localPos; r.localRot = bp.localRot;
                XMVECTOR clip = XMVector4Transform(
                    XMVectorSet(wpos[b].x, wpos[b].y, wpos[b].z, 1.0f), vpM);
                const float w = XMVectorGetW(clip);
                if (w > 1e-4f) {
                    const float ndcx = XMVectorGetX(clip) / w, ndcy = XMVectorGetY(clip) / w;
                    r.px = (ndcx * 0.5f + 0.5f) * img;
                    r.py = (1.0f - (ndcy * 0.5f + 0.5f)) * img;
                    r.inFrame = (r.px >= 0 && r.px < img && r.py >= 0 && r.py < img) ? 1 : 0;
                } else { r.px = r.py = -1.0f; r.inFrame = 0; }
                r.visible = 0;   // decided below, from the rendered depth buffer
            }

            for (bool iso : bgs) {
                m_captureIsolated = iso;
                // Occlusion is a property of (frame, view, background) — the styles that follow
                // only change shading — so the depth test runs once per background and the result
                // is reused for every style.
                bool visibilityDone = false;
                int  canonInFrame = 0, canonVisible = 0;

                // Shared for every style below: one annotation per (frame, view, background).
                SampleMeta m;
                m.character = cfg.character; m.motion = cfg.motion;
                m.frameIndex = fi; m.timeSec = t; m.viewIndex = vi;
                m.azimuthDeg = vs.az; m.elevDeg = vs.el;
                m.background = iso ? "isolated" : "scene";
                m.bodyPart = BodyPartName(vs.part);
                m.imgW = img; m.imgH = img;
                m.camEye = eye; m.camTarget = ctr; m.camUp = { 0.0f, 1.0f, 0.0f };
                m.fovY = fov; m.aspect = 1.0f; m.zNear = m_camera.Near(); m.zFar = m_camera.Far();
                XMStoreFloat4x4(&m.view, viewM); XMStoreFloat4x4(&m.proj, m_camera.Proj());

                char annStem[160];
                std::snprintf(annStem, sizeof annStem, "f%04d_v%03d_%s",
                              fi, vi, iso ? "iso" : "scene");
                m.annFile = std::string(annStem) + ".json";

                for (int st : styles) {
                    applyStyle(st);
                    StreamSkinnedVertices();   // upload this frame's skinned verts into the dynamic VB
                    Render();                  // draw + present one capture frame

                    if (!visibilityDone) {
                        MarkJointVisibility(recs, img, bodyHeight, canonInFrame, canonVisible);
                        visibilityDone = true;
                    }

                    const std::string style = StyleName((StyleId)st);
                    const std::string base  = std::string(annStem) + "_" + style;
                    Screenshot((root / (base + ".png")).wstring(), /*verbose*/ false, /*wantAlpha*/ iso);
                    m.styleImages.emplace_back(style, base + ".png");

                    // Manifest stays one line per image (that is the unit you iterate when
                    // training); the pose lives once in the annotation each line points at.
                    m.style     = style;
                    m.imageFile = base + ".png";
                    // Filled in below once the whole background is captured, but the manifest is
                    // flushed per line so a crash keeps what was written — carry them now.
                    m.canonicalInFrame = canonInFrame;
                    m.canonicalVisible = canonVisible;
                    m.visibleFraction  = canonInFrame > 0
                                       ? static_cast<float>(canonVisible) / static_cast<float>(canonInFrame)
                                       : 0.0f;
                    m.charDepthNear = m_charDepthNear;   // scale of a char_depth capture
                    m.charDepthFar  = m_charDepthFar;
                    AppendManifestLine(manifest, m);

                    ++done; m_datasetDone.store(done);
                    if (done % 32 == 0 || done == total) {
                        const double el = std::chrono::duration<double>(clock::now() - t0).count();
                        std::printf("[dataset] %d/%d  (%.1f%%, %.1f/s)\n", done, total,
                                    100.0 * done / total, el > 0 ? done / el : 0.0);
                        std::fflush(stdout);
                    }
                }

                WriteSampleJson((root / m.annFile).wstring(), m,
                                cfg.canonicalJointsOnly ? CompactToCanonical(recs) : recs);
            }
        }
    }

    // ---- restore user state ----
    if (m_width != sW || m_height != sH) Resize(sW, sH);
    m_view = sView; m_bloomEnabled = sBloom; m_ssaoEnabled = sSsao; m_dirLightOn = sDir;
    m_forwardPlus = sFplus; m_fpDebugHeat = sHeat; m_pointShadowOn = sPshadow;
    m_charXray = sXray; m_camMotionOn = sCamMotion; m_imguiVisible = sImgui; m_vsync = sVsync;
    m_exposure = sExp; m_vibrance = sVib; m_charSat = sSat; m_charContrast = sCon; m_specInt = sSpec;
    m_lightDir = sLightDir; m_lightTint = sTint; m_captureIsolated = sIsolated;
    m_camera.SetPosition(sCamPos); m_camera.SetYawPitch(sYaw, sPitch);
    (void)sPaused;

    StartAnimThread();   // resume live animation
    std::printf("[dataset] done: %d samples written to %ls\n", done, root.wstring().c_str());
    std::fflush(stdout);
    m_datasetBusy.store(false);
}

bool Renderer::Screenshot(const std::wstring& path, bool verbose, bool wantAlpha) {
    if (!m_queue || !m_backBuffers[m_lastPresentedIndex]) return false;
    WaitForGpu();
    // Isolated dataset captures carry a coverage alpha (0 = background). Force a 32bpp RGBA PNG
    // so the cut-out stays transparent; the default encoder would otherwise drop to 24bpp RGB.
    const GUID* targetFmt = wantAlpha ? &GUID_WICPixelFormat32bppBGRA : nullptr;
    HRESULT hr = DirectX::SaveWICTextureToFile(
        m_queue.Get(),
        m_backBuffers[m_lastPresentedIndex].Get(),
        GUID_ContainerFormatPng,
        path.c_str(),
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_PRESENT,
        targetFmt, nullptr, true /*forceSRGB: back buffer is UNORM, keep colors as seen*/);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[Renderer] Screenshot failed (HR=0x%08lX): %ls\n",
                     static_cast<unsigned long>(hr), path.c_str());
        return false;
    }
    if (verbose) {
        std::printf("[Renderer] Saved screenshot: %ls\n", path.c_str());
        std::fflush(stdout);
    }
    return true;
}

void Renderer::MarkJointVisibility(std::vector<JointRecord>& recs, int imgSize, float bodyHeight,
                                   int& canonicalInFrame, int& canonicalVisible) {
    canonicalInFrame = canonicalVisible = 0;

    std::vector<float> depth;
    UINT dw = 0, dh = 0;
    const bool haveDepth = ReadDepthBuffer(depth, dw, dh) && dw > 0 && dh > 0;

    // A bone sits INSIDE the mesh, so the surface in front of it is always slightly nearer than the
    // joint itself. Allow about a limb radius of slack (scaled to the character, so this holds for
    // any model size); anything closer than that is a real occluder — the torso in front of a hidden
    // arm, or a Sponza column in front of the whole body.
    const float slack = std::max(1.0f, 0.08f * bodyHeight);

    const XMMATRIX viewM = m_camera.View();
    XMFLOAT4X4 P; XMStoreFloat4x4(&P, m_camera.Proj());
    const float A = P._33, B = P._43;          // ndcZ = A + B / viewZ   (row-vector, LH, [0,1])

    for (auto& r : recs) {
        const bool canonical = !r.canonical.empty();
        if (canonical && r.inFrame) ++canonicalInFrame;
        r.viewDepth = r.surfaceDepth = -1.0f;
        if (!r.inFrame)  { r.visible = 0; continue; }
        if (!haveDepth)  { r.visible = 1; continue; }   // no depth available → don't claim occlusion

        const XMVECTOR vpos = XMVector3TransformCoord(XMLoadFloat3(&r.worldPos), viewM);
        const float jointZ = XMVectorGetZ(vpos);
        r.viewDepth = jointZ;
        if (jointZ <= 0.0f) { r.visible = 0; continue; }

        // The depth buffer runs at the (possibly supersampled) internal resolution.
        const int cx = static_cast<int>(r.px * static_cast<float>(dw) / static_cast<float>(imgSize));
        const int cy = static_cast<int>(r.py * static_cast<float>(dh) / static_cast<float>(imgSize));

        // The surface at the joint's OWN pixel — no neighbourhood filtering, so surfaceDepth means
        // exactly "what the depth map shows here" and the two can be checked against each other.
        // The slack below already covers silhouette aliasing.
        if (cx < 0 || cy < 0 || cx >= static_cast<int>(dw) || cy >= static_cast<int>(dh)) {
            r.visible = 0; continue;
        }
        const float zBuf = depth[static_cast<size_t>(cy) * dw + cx];

        // Cleared depth = nothing was drawn there, so nothing occludes the joint.
        const bool  empty    = (zBuf >= 1.0f);
        const float surfaceZ = empty ? std::numeric_limits<float>::max() : B / (zBuf - A);
        r.surfaceDepth = empty ? -1.0f : surfaceZ;
        r.visible = (jointZ <= surfaceZ + slack) ? 1 : 0;
        if (canonical && r.visible) ++canonicalVisible;
    }
}

bool Renderer::ReadDepthBuffer(std::vector<float>& out, UINT& width, UINT& height) {
    if (!m_depth || !m_device || !m_queue || !m_cmd) return false;

    // Render() has already executed + presented; make sure that work is finished before the copy,
    // and before recycling this frame's allocator to record it.
    WaitForGpu();

    const D3D12_RESOURCE_DESC dd = m_depth->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT   rows     = 0;
    UINT64 rowBytes = 0, totalBytes = 0;
    m_device->GetCopyableFootprints(&dd, 0, 1, 0, &fp, &rows, &rowBytes, &totalBytes);
    if (totalBytes == 0) return false;

    if (!m_depthReadback || m_depthReadbackBytes < totalBytes) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = totalBytes;
        bd.Height           = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels        = 1;
        bd.Format           = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_depthReadback)));
        NameObject(m_depthReadback.Get(), L"DepthReadback");
        m_depthReadbackBytes = totalBytes;
    }

    ThrowIfFailed(m_allocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_cmd->Reset(m_allocators[m_frameIndex].Get(), nullptr));

    // Render() leaves the depth buffer in DEPTH_WRITE (the light-sphere pass transitions it back).
    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = m_depth.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    m_cmd->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource       = m_depthReadback.Get();
    dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = m_depth.Get();
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    m_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    m_cmd->ResourceBarrier(1, &b);

    ThrowIfFailed(m_cmd->Close());
    ID3D12CommandList* lists[] = { m_cmd.Get() };
    m_queue->ExecuteCommandLists(_countof(lists), lists);
    WaitForGpu();

    width  = static_cast<UINT>(dd.Width);
    height = dd.Height;
    out.resize(static_cast<size_t>(width) * height);

    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(totalBytes) };
    ThrowIfFailed(m_depthReadback->Map(0, &readRange, &mapped));
    for (UINT y = 0; y < height; ++y)
        std::memcpy(out.data() + static_cast<size_t>(y) * width,
                    static_cast<const uint8_t*>(mapped) + static_cast<size_t>(y) * fp.Footprint.RowPitch,
                    static_cast<size_t>(width) * sizeof(float));
    D3D12_RANGE noWrite{ 0, 0 };
    m_depthReadback->Unmap(0, &noWrite);
    return true;
}

void Renderer::WaitForGpu() {
    if (!m_fence || !m_queue || !m_fenceEvent) return;
    UINT64 v = m_fenceValues[m_frameIndex];
    ThrowIfFailed(m_queue->Signal(m_fence.Get(), v));
    if (m_fence->GetCompletedValue() < v) {
        ThrowIfFailed(m_fence->SetEventOnCompletion(v, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    m_fenceValues[m_frameIndex] = v + 1;
}

void Renderer::MoveToNextFrame() {
    UINT64 currentValue = m_fenceValues[m_frameIndex];
    ThrowIfFailed(m_queue->Signal(m_fence.Get(), currentValue));
    m_frameIndex = m_swap->GetCurrentBackBufferIndex();
    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex]) {
        ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    m_fenceValues[m_frameIndex] = currentValue + 1;
}

void Renderer::Shutdown() {
    StopAnimThread();   // join the worker before tearing down the animator it uses
    if (m_queue && m_fence && m_fenceEvent) WaitForGpu();
    if (m_imguiReady) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_imguiReady = false;
    }
    m_imguiSrvHeap.Reset();
    if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }

    m_mmdDynamicVB.Reset();
    m_animator.reset();
    m_scene = Scene{};
    m_mmd   = Scene{};

    m_endfieldPSO.Reset();
    m_endfieldOutlinePSO.Reset();
    m_endfieldBlendPSO.Reset();
    m_wuwaPSO.Reset();
    m_wuwaOutlinePSO.Reset();
    m_wuwaBlendPSO.Reset();
    m_zzzPSO.Reset();
    m_zzzOutlinePSO.Reset();
    m_zzzBlendPSO.Reset();
    m_endfieldRS.Reset();
    m_geometryPSO.Reset();
    m_geometryPSONoCull.Reset();
    m_charXrayDepthPSO.Reset();
    m_decalPSO.Reset();
    m_decalEmissivePSO.Reset();
    m_geometryRS.Reset();
    m_lightingPSO.Reset();
    m_lightingRS.Reset();
    m_postRS.Reset();
    m_brightPSO.Reset();
    m_blurPSO.Reset();
    m_tonemapPSO.Reset();
    m_fxaaPSO.Reset();
    m_ldrRT.Reset();
    m_shadowRS.Reset();
    m_shadowPSO.Reset();
    m_ssaoRS.Reset();
    m_ssaoPSO.Reset();
    m_ssaoBlurPSO.Reset();

    // Forward+ / point-shadow / bloom-sphere objects.
    m_lightCullPSO.Reset();
    m_lightCullRS.Reset();
    m_tileLightBuffer.Reset();
    m_pointShadowPSO.Reset();
    m_pointShadowRS.Reset();
    m_pointCube.Reset();
    m_pointCubeDepth.Reset();
    m_pointRtvHeap.Reset();
    m_spherePSO.Reset();
    m_sphereRS.Reset();
    m_sphereVB.Reset();
    m_sphereIB.Reset();

    m_normalRT.Reset();
    m_albedoRT.Reset();
    m_sceneHDR.Reset();
    m_bloom0.Reset();
    m_bloom1.Reset();
    m_shadowMap.Reset();
    m_ssaoRT.Reset();
    m_ssaoBlurRT.Reset();
    m_ssaoNoise.Reset();
    m_depth.Reset();

    m_dsvHeap.Reset();
    m_gbufferSrvHeap.Reset();

    m_graphicsMemory.reset();
    m_cmd.Reset();
    for (auto& a : m_allocators) a.Reset();
    ReleaseBackBuffers();
    m_rtvHeap.Reset();
    m_swap.Reset();
    m_fence.Reset();
    m_queue.Reset();
    m_device.Reset();
    m_factory.Reset();
}

void Renderer::CycleView() {
    int n = static_cast<int>(m_view);
    n = (n + 1) % 4;
    m_view = static_cast<ViewMode>(n);
}

} // namespace dr
