// Lighting pass: full-screen triangle samples the G-buffer and outputs the final
// image. Depth/Normal/Albedo views are debug visualisations, Color is the actual
// Blinn-Phong directional-light result.
cbuffer PerFrame : register(b0)
{
    row_major float4x4 viewProj;
    row_major float4x4 invViewProj;
    float3 cameraPos;        uint  viewMode;
    float3 lightDirToLight;  float zNear;
    float3 lightIntensity;   float zFar;
    row_major float4x4 lightViewProj;
    float shadowBias;        float shadowTexel;
    float outlineDarken;     float _pad1;   // cel outline: edge = interior colour x this
    float4 captureBg;        // dataset isolation: xyz = bg colour, w = 1 when isolating character
    float4 charDepthRange;   // CharDepth view: x = character near, y = character far (view space)
};

Texture2D<float>  g_depth  : register(t0);
Texture2D<float4> g_normal : register(t1);
Texture2D<float4> g_albedo : register(t2);
Texture2D<float>  g_shadow : register(t3);
Texture2D<float>  g_ssao   : register(t4);
SamplerComparisonState g_shadowSamp : register(s0);

// ---- Forward+ tiled point lights (culled per 16x16 tile by LightCulling.hlsl) ----
#define FP_TILE_SIZE    16
#define FP_MAX_PER_TILE 64

struct PointLight { float3 posWS; float radius; float3 color; float intensity; };

cbuffer ForwardPlusCB : register(b1)
{
    uint2 fpTileCount;   // tiles across x,y
    uint  fpNumLights;
    uint  fpEnabled;     // 1 = accumulate tiled point lights

    uint  fpDebugHeat;   // 1 = show per-tile light-count heatmap
    uint  fpPointEnabled;// (unused) kept for layout
    uint  fpDirEnabled;  // 1 = directional "sun" on
    uint  fpNumShadowed; // the first N point lights cast cube shadows (0 = none)

    // Character (cel) material controls — GUI sliders.
    float3 charSssColor;   float charSssStrength;   // subsurface scatter tint + amount
    float  charSpecInt;    float charSpecPow;       // normal-based specular highlight
    float  charSssWrap;    float charSkinFresnel;   // SSS wrap; skin view/normal sheen amount
};

StructuredBuffer<PointLight> g_pointLights : register(t5);
StructuredBuffer<uint>       g_tileLights  : register(t6);
TextureCubeArray<float>      g_pointShadow : register(t7);   // normalised distance cubes (per shadowed light)
SamplerState                 g_linearSamp  : register(s1);

// Omnidirectional shadow for shadowed point light `li`: compare this surface's distance to
// the light against cube slice `li`'s stored nearest distance along the same direction.
float PointCubeVisibility(uint li, float3 worldPos, float3 lightPos, float range)
{
    float3 d      = worldPos - lightPos;
    float  cur    = length(d) / range;
    float  stored = g_pointShadow.SampleLevel(g_linearSamp, float4(d, (float)li), 0);
    return (cur - 0.03 > stored) ? 0.0 : 1.0;     // 0.03 normalised bias vs self-shadow acne
}

// Accumulate this pixel's tile light list. Returns added radiance; outputs the tile's
// light count for the debug heatmap. Lights with index < fpNumShadowed are shadowed.
float3 AccumPointLights(int2 ip, float3 worldPos, float3 N, float3 albedo, out uint tileLightCount)
{
    tileLightCount = 0;
    if (fpEnabled == 0) return float3(0, 0, 0);
    uint2 tile    = uint2(ip) / FP_TILE_SIZE;
    uint  tileIdx = tile.y * fpTileCount.x + tile.x;
    uint  base    = tileIdx * (FP_MAX_PER_TILE + 1);
    uint  n       = g_tileLights[base];
    tileLightCount = n;

    float3 sum = float3(0, 0, 0);
    for (uint k = 0; k < n; ++k)
    {
        uint       li = g_tileLights[base + 1 + k];
        PointLight L  = g_pointLights[li];
        float3 d    = L.posWS - worldPos;
        float  dist = length(d);
        if (dist < L.radius)
        {
            float3 Ld    = d / max(dist, 1e-4);
            float  atten = saturate(1.0 - dist / L.radius); atten *= atten; // smooth quadratic falloff
            float  ndl   = saturate(dot(N, Ld));
            float  vis   = (li < fpNumShadowed)
                         ? PointCubeVisibility(li, worldPos, L.posWS, L.radius) : 1.0;
            sum += albedo * L.color * (L.intensity * ndl * atten * vis);
        }
    }
    return sum;
}

// Blue -> cyan -> green -> yellow -> red ramp for the tile light-count heatmap.
float3 HeatColor(uint count)
{
    float t = saturate(count / 32.0);
    float3 c = lerp(float3(0, 0, 1), float3(0, 1, 1), saturate(t * 4.0));
    c = lerp(c, float3(0, 1, 0), saturate(t * 4.0 - 1.0));
    c = lerp(c, float3(1, 1, 0), saturate(t * 4.0 - 2.0));
    c = lerp(c, float3(1, 0, 0), saturate(t * 4.0 - 3.0));
    return c;
}

// Percentage-closer-filtered shadow visibility (1 = lit, 0 = fully shadowed).
float ShadowVisibility(float3 worldPos, float NdotL)
{
    float4 lp = mul(float4(worldPos, 1.0), lightViewProj);
    lp.xyz /= lp.w;
    float2 uv = lp.xy * float2(0.5, -0.5) + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || lp.z > 1.0)
        return 1.0;                                    // outside the light frustum: lit

    // Slope-scaled bias to fight acne on grazing surfaces.
    float bias = max(shadowBias * (1.0 - NdotL), shadowBias * 0.2);
    float cmp = lp.z - bias;

    float sum = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x)
            sum += g_shadow.SampleCmpLevelZero(g_shadowSamp, uv + float2(x, y) * shadowTexel, cmp);
    return sum / 9.0;
}

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float LinearizeDepth(float zNdc, float n, float f)
{
    // LH perspective: zNdc = f/(f-n) * (1 - n/zView)  =>  zView = (n*f) / (f - zNdc*(f-n))
    return (n * f) / (f - zNdc * (f - n));
}

float3 ReconstructWorldPos(float2 uv, float zNdc)
{
    float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, zNdc, 1.0);
    float4 world = mul(ndc, invViewProj);
    return world.xyz / world.w;
}

// Screen-space outline for the cel-shaded character: a pixel is an edge if a nearby
// sample is background, jumps in (linear) depth, or turns sharply in normal. Sampling
// integer offsets keeps it independent of resolution scaling.
bool DetectCharacterEdge(int2 ip, float3 N, float centerLinDepth)
{
    // 1-pixel taps keep the outline a thin, constant ~1px screen line (a 2px tap reads as
    // a thick rim, especially when the character is small/far away). Thresholds are kept
    // a little loose so a real silhouette/crease still registers at one pixel.
    const int2 offs[4] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };
    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        int2 p = ip + offs[k];
        float zn = g_depth.Load(int3(p, 0));
        if (zn >= 1.0) return true;                                   // silhouette vs sky/bg
        float dn = LinearizeDepth(zn, zNear, zFar);
        if (abs(dn - centerLinDepth) > centerLinDepth * 0.04) return true; // depth crease/silhouette
        float3 nn = normalize(g_normal.Load(int3(p, 0)).xyz);
        if (1.0 - dot(N, nn) > 0.45) return true;                     // sharp normal change only
    }
    return false;
}

float3 ShadeCel(int2 ip, float3 N, float3 albedo, float zNdc, float2 uv, float ao, bool isSkin)
{
    float3 worldPos = ReconstructWorldPos(uv, zNdc);
    float3 V = normalize(cameraPos - worldPos);
    float3 L = normalize(lightDirToLight);
    float3 H = normalize(L + V);

    float ndl = dot(N, L);
    float vis = ShadowVisibility(worldPos, saturate(ndl));

    // Subsurface scattering — SKIN ONLY (body + face skin). Cheap wrap-diffuse: light wraps
    // past the terminator and the wrapped-in part is tinted by charSssColor, softening skin.
    // Cloth/other parts get plain N·L (no wrap, no scatter).
    float  effWrap = isSkin ? charSssWrap : 0.0;
    float  wrapNdl = saturate((ndl + effWrap) / (1.0 + effWrap));
    float  scatter = isSkin ? saturate(wrapNdl - saturate(ndl)) : 0.0;
    float3 sssTerm = charSssColor * (scatter * charSssStrength) * albedo;

    // Banded ("gradient") diffuse — three flat tones, on the wrapped N·L.
    float litNdotL = wrapNdl * vis;                // shadow pushes toward the dark band
    float toon  = (litNdotL > 0.60) ? 1.00
                : (litNdotL > 0.30) ? 0.65
                : (litNdotL > 0.05) ? 0.40
                                    : 0.30;

    // Normal-based specular highlight (Blinn-Phong N·H), GUI-controlled (intensity + power).
    float NdotH = saturate(dot(N, H));
    float spec  = pow(NdotH, charSpecPow) * charSpecInt * vis;

    // Rim light around the silhouette-facing edges.
    float rim = smoothstep(0.55, 1.0, 1.0 - saturate(dot(N, V))) * 0.45;

    float3 aoTint = lerp(1.0, ao, 0.6);
    float3 fill   = (albedo * toon + sssTerm) * lightIntensity * aoTint;  // diffuse cel + SSS
    float3 col    = fill + (spec.xxx + rim.xxx) * aoTint;                  // + highlights

    // Skin sheen: a view+normal (camera-dependent) Fresnel highlight on skin only — brightens
    // the grazing-angle skin as the camera moves. GUI-controlled (charSkinFresnel).
    if (isSkin && charSkinFresnel > 0.0)
    {
        float fres = pow(1.0 - saturate(dot(N, V)), 3.0);
        col += fres * charSkinFresnel;
    }

    // Outline: a deeper extension of the interior colour rather than a flat black line.
    // Darken the DIFFUSE FILL (not the full colour) — the rim light whitens silhouette edges,
    // so darkening a rim-washed colour produced a grey line on skin; using the fill keeps the
    // skin/material hue. outlineDarken (uniform) controls how dark: lower = darker.
    float linDepth = LinearizeDepth(zNdc, zNear, zFar);
    if (DetectCharacterEdge(ip, N, linDepth))
        return fill * outlineDarken;

    return col;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    int2  ip      = int2(i.pos.xy);
    float zNdc    = g_depth.Load(int3(ip, 0));
    float4 nSamp  = g_normal.Load(int3(ip, 0));
    float4 albedo = g_albedo.Load(int3(ip, 0));
    float3 N      = normalize(nSamp.xyz);

    // Unified background: a far-plane pixel has no geometry. Under dataset isolation
    // (captureBg.w > 0.5) paint the solid capture colour with alpha 0 so the PNG cuts the
    // character out (the tonemap passes this coverage alpha through); otherwise the original
    // opaque black. Coverage = 1 for all geometry pixels below.
    if (zNdc >= 1.0)
    {
        float3 bg = (captureBg.w > 0.5) ? captureBg.rgb : float3(0.0, 0.0, 0.0);
        float  a  = (captureBg.w > 0.5) ? 0.0 : 1.0;
        return float4(bg, a);
    }

    if (viewMode == 0) // Depth — linearised, divided by zFar
    {
        float zLin = LinearizeDepth(zNdc, zNear, zFar);
        return float4(saturate(zLin / zFar).xxx, 1.0);
    }
    if (viewMode == 5) // Character depth — the character's own near..far span, background dropped
    {
        // normal.a is the G-buffer material id: 0 = scene, 1 = character cloth, 2 = skin. Anything
        // that is not the character is background here, whatever its distance — so a wall right in
        // front of the camera cannot squash the character's range or leak into the map.
        if (nSamp.a < 0.5)
        {
            float3 bg = (captureBg.w > 0.5) ? captureBg.rgb : float3(0.0, 0.0, 0.0);
            float  a  = (captureBg.w > 0.5) ? 0.0 : 1.0;
            return float4(bg, a);
        }
        float zLin  = LinearizeDepth(zNdc, zNear, zFar);
        float span  = max(1e-3, charDepthRange.y - charDepthRange.x);
        float t     = saturate((zLin - charDepthRange.x) / span);
        return float4((1.0 - t).xxx, 1.0);            // nearest = white, farthest = black
    }
    if (viewMode == 1) // Normal — [-1,1] -> [0,1]
    {
        return float4(N * 0.5 + 0.5, 1.0);
    }
    if (viewMode == 2) // Albedo
    {
        return float4(albedo.rgb, 1.0);
    }
    if (viewMode == 4) // Outline / line-art: flat colour fill with inked silhouette + crease edges
    {
        float linD = LinearizeDepth(zNdc, zNear, zFar);
        bool  edge = DetectCharacterEdge(ip, N, linD);
        return edge ? float4(albedo.rgb * (outlineDarken * 0.3), 1.0)
                    : float4(albedo.rgb, 1.0);
    }

    // Color: directional light. The G-buffer normal alpha carries a material id —
    // 1 = MMD character (cel shading + outline), otherwise the scene's Blinn-Phong.
    float  ao       = g_ssao.Load(int3(ip, 0));
    float3 worldPos = ReconstructWorldPos(i.uv, zNdc);

    // Base shading: cel for the character (normal.a > 0.5), Blinn-Phong for the scene.
    // The directional "sun" can be switched off (master light toggle) — its whole
    // contribution is gated, leaving only the point lights below.
    // normal.a carries the material id: 0 = scene, 1 = character cloth, 2 = character skin.
    // (The Eff facial decals are NOT in the G-buffer — drawn later as a forward blended pass.)
    float3 base;
    if (nSamp.a > 0.5)
    {
        bool isSkin = nSamp.a > 1.5;   // 2 = skin (gets SSS), 1 = cloth
        base = (fpDirEnabled != 0) ? ShadeCel(ip, N, albedo.rgb, zNdc, i.uv, ao, isSkin)
                                   : float3(0, 0, 0);
    }
    else
    {
        float3 V = normalize(cameraPos - worldPos);
        float3 L = normalize(lightDirToLight);
        float3 H = normalize(L + V);
        float  NdotL = saturate(dot(N, L));
        float  NdotH = saturate(dot(N, H));
        const float shininess = 64.0;

        float  vis     = ShadowVisibility(worldPos, NdotL);
        float3 ambient = 0.10 * albedo.rgb * ao;
        float3 diffuse = albedo.rgb * NdotL * vis;
        float3 spec    = pow(NdotH, shininess).xxx * 0.2 * vis;
        base = (fpDirEnabled != 0) ? (ambient + diffuse + spec) * lightIntensity
                                   : float3(0, 0, 0);
    }

    // Forward+ tiled point lights, added on top of the directional result.
    uint tileLightCount;
    base += AccumPointLights(ip, worldPos, N, albedo.rgb, tileLightCount);
    if (fpDebugHeat != 0)
        return float4(HeatColor(tileLightCount), 1.0);

    return float4(base, 1.0);
}
