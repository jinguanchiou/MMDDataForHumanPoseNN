// Wuthering Waves (鸣潮) style — dedicated forward PBR-based NPR pass. Shares the forward root
// signature / constant buffers with the Endfield pass, but its own shading model: underneath is
// Cook-Torrance GGX (all materials), stylised on top into 2-band toon diffuse with a COLD/colourful
// shadow tint (warm-red for skin), stronger metallic, higher contrast + saturation than Endfield,
// and — per the spec — the character does NOT receive the scene shadow map (no self-shadow).
cbuffer PerFrame : register(b0)
{
    row_major float4x4 viewProj;
    row_major float4x4 invViewProj;
    float3 cameraPos;        uint  viewMode;
    float3 lightDirToLight;  float zNear;
    float3 lightIntensity;   float zFar;
    row_major float4x4 lightViewProj;
    float  shadowBias;       float shadowTexel;
    float  outlineDarken;    float _pad1;
    float4 captureBg;
};

cbuffer WuwaObject : register(b1)   // same layout as EndfieldObject
{
    row_major float4x4 world;
    int    debugMode;
    float  outlineWidth;
    float  toonThreshold;
    float  toonFeather;
    float2 screenSize;
    int    hasPacked;
    int    hasEmissive;
    int    hasNormal;
    int    isHair;
    int    transparentMode;
    int    matClass;         // 0 cloth/default, 1 skin, 2 hair, 3 eye, 4 metal
    float3 matDiffuse;
    float  matAlpha;
    int    sphereMode;
    float  outlineScale;     // 0..1 from the character's on-screen height; 0 = no outline
    float  matcapStrength; float satBoost;
    float  outlineDepthBias;
    float  postExposure, postVibrance, texFidelity;   // undo the shared post chain → stop the white wash
};

float2 OutlineOffset(float4 clip, float2 nClip, float3 posW)
{
    return nClip * (outlineWidth * outlineScale * 2.0 / max(screenSize, 1.0)) * clip.w;
}

cbuffer WuwaMaterial : register(b2)   // same layout as EndfieldMaterial
{
    int   metalChan;
    int   roughChan;
    int   invertRough;
    float specStrength;
    float roughBias;
    float rimStrength;
    float rimPower;
    float emissStrength;
    float useNormalMap;
    float hairStrength;
    float normalYSign;
    float shadowStrength;   // (Endfield self-shadow amount; unused by Wuwa) — keep layout parity
    float3 rimColor;
    float shadowDepth;      // dark-side brightness (lower = darker)
    // Row5 (shared with ZZZ's colour-grade slots — Wuwa uses slot0 = shadow-tint, slot3 = exposure).
    float wuwaShadowTint, _wd1, _wd2, wuwaExposure;   // shadowTint: 1 = full cold blue, 0 = neutral grey
};

Texture2D    gBase   : register(t0);
Texture2D    gNormal : register(t1);
Texture2D    gPacked : register(t2);
Texture2D    gMask   : register(t3);
Texture2D    gEmiss  : register(t4);
Texture2D<float>       gShadow     : register(t5);   // bound but unused (Wuwa: no self-shadow)
SamplerState           gSamp       : register(s0);
SamplerComparisonState gShadowSamp : register(s1);

struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float3 nrmW : NORMAL; float2 uv : TEXCOORD0; float3 posW : TEXCOORD1; };

VSOut VSMain(VSIn i)
{
    VSOut o;
    float4 wp = mul(float4(i.pos, 1.0), world);
    o.posW = wp.xyz;
    o.pos  = mul(wp, viewProj);
    o.nrmW = normalize(mul(i.nrm, (float3x3)world));
    o.uv   = i.uv;
    return o;
}

VSOut VSOutline(VSIn i)
{
    VSOut o;
    float4 wp   = mul(float4(i.pos, 1.0), world);
    float3 nW   = normalize(mul(i.nrm, (float3x3)world));
    wp.xyz += normalize(wp.xyz - cameraPos) * outlineDepthBias;   // depth-aware push-back (see Endfield)
    float4 clip = mul(wp, viewProj);
    float2 nClip = normalize(mul(nW, (float3x3)viewProj).xy + 1e-5);
    clip.xy += OutlineOffset(clip, nClip, wp.xyz);
    o.pos  = clip; o.posW = wp.xyz; o.nrmW = nW; o.uv = i.uv;
    return o;
}

float4 PSOutline(VSOut i) : SV_TARGET
{
    float4 base = gBase.Sample(gSamp, i.uv);
    base.rgb *= matDiffuse; base.a *= matAlpha;
    clip(base.a - 0.3);
    float3 bl = pow(max(base.rgb, 0.0), 2.2);
    float  luma = dot(bl, float3(0.2126, 0.7152, 0.0722));
    return float4(max(lerp(luma.xxx, bl, 1.25), 0.0) * 0.30, 1.0);   // deep tint of the interior colour
}

float chan4(float4 v, int c) { return (c == 0) ? v.r : (c == 1) ? v.g : (c == 2) ? v.b : v.a; }

// Analytic inverse of the Narkowicz ACES curve — pre-invert the shared exposure→ACES tonemap so the
// character keeps its painted albedo instead of washing to white (the "貼圖被白光蓋掉" fix; same as
// Endfield/ZZZ). Undoes exposure + ACES only; vibrance is kept so colours stay punchy.
float3 ACESInv(float3 y)
{
    y = clamp(y, 0.0, 0.9999);
    float3 disc = max(-1.0127 * y * y + 1.3702 * y + 0.0009, 0.0);
    return (0.03 - 0.59 * y - sqrt(disc)) / (4.86 * y - 5.02);
}

float3 PerturbNormal(float3 N, float3 posW, float2 uv, float3 mapN)
{
    float3 dp1 = ddx(posW), dp2 = ddy(posW);
    float2 du1 = ddx(uv),   du2 = ddy(uv);
    float3 T = dp1 * du2.y - dp2 * du1.y;
    float3 B = dp2 * du1.x - dp1 * du2.x;
    float invmax = rsqrt(max(dot(T, T), dot(B, B)) + 1e-8);
    return normalize(mapN.x * (T * invmax) + mapN.y * (B * invmax) + mapN.z * N);
}

// Per-material default metallic/roughness when the model ships no _P (this rig mostly doesn't).
void ClassDefaults(int cls, out float metal, out float rough)
{
    if      (cls == 1) { metal = 0.0;  rough = 0.55; }   // skin
    else if (cls == 2) { metal = 0.15; rough = 0.40; }   // hair
    else if (cls == 3) { metal = 0.0;  rough = 0.15; }   // eye (glossy)
    else               { metal = 0.0;  rough = 0.60; }   // cloth/default
}

float4 PSMain(VSOut i) : SV_TARGET
{
    float4 base = gBase.Sample(gSamp, i.uv);
    base.rgb *= matDiffuse;
    base.a   *= matAlpha;
    clip(base.a - (transparentMode == 0 ? 0.3 : 0.004));

    if (debugMode >= 2) {
        float3 c;
        if      (debugMode == 2) c = gNormal.Sample(gSamp, i.uv).rgb;
        else if (debugMode == 3) c = gPacked.Sample(gSamp, i.uv).rrr;
        else if (debugMode == 4) c = gPacked.Sample(gSamp, i.uv).ggg;
        else if (debugMode == 5) c = gPacked.Sample(gSamp, i.uv).bbb;
        else if (debugMode == 6) c = gPacked.Sample(gSamp, i.uv).aaa;
        else if (debugMode == 7) c = gMask.Sample(gSamp, i.uv).rgb;
        else                     c = gEmiss.Sample(gSamp, i.uv).rgb;
        return float4(pow(max(c, 0.0), 2.2), 1.0);
    }

    float3 baseLin = pow(max(base.rgb, 0.0), 2.2);
    if (debugMode == 1) return float4(baseLin, 1.0);

    float3 N = normalize(i.nrmW);
    if (useNormalMap > 0.5 && hasNormal != 0) {
        float3 mapN;
        mapN.xy = gNormal.Sample(gSamp, i.uv).xy * 2.0 - 1.0;
        mapN.y *= normalYSign;
        mapN.z = sqrt(saturate(1.0 - dot(mapN.xy, mapN.xy)));   // reconstruct Z (RG normal maps have B=0)
        N = PerturbNormal(N, i.posW, i.uv, mapN);
    }
    float3 L = normalize(lightDirToLight);
    float3 V = normalize(cameraPos - i.posW);
    float3 H = normalize(L + V);
    float  NdotL = dot(N, L);
    float  ndl = saturate(NdotL), ndv = saturate(dot(N, V)), ndh = saturate(dot(N, H));

    // Overlay meshes (eye/hair shadow, matAlpha<1) → flat dark blend, no highlights.
    if (transparentMode != 0) {
        float3 shade = lerp(baseLin * float3(0.7, 0.72, 0.85) * shadowDepth, baseLin,
                            smoothstep(toonThreshold - toonFeather, toonThreshold + toonFeather, NdotL * 0.5 + 0.5));
        return float4(shade * lightIntensity, base.a);
    }

    // --- PBR-based NPR diffuse: 2-band toon on half-lambert (higher contrast than Endfield),
    //     cold colourful shadow tint (warm-red for skin) with a value/saturation clamp ---
    float halfLambert = NdotL * 0.5 + 0.5;
    float b1  = smoothstep(toonThreshold - toonFeather, toonThreshold + toonFeather, halfLambert);
    float b2  = smoothstep(0.72 - toonFeather, 0.72 + toonFeather, halfLambert);
    float litLevel = saturate(b1 * 0.65 + b2 * 0.35);
    // Cold shadow tint, dialled from full blue-violet → neutral grey by wuwaShadowTint (GUI). This is
    // the "藍色光害" the user saw — it's this shader's stylised shadow colour, NOT any external light.
    float3 coldTint = lerp(float3(0.72, 0.72, 0.74), float3(0.52, 0.58, 0.85), saturate(wuwaShadowTint));
    float3 warmTint = float3(0.98, 0.62, 0.55);          // warm red — skin subsurface feel
    float3 darkTint = (matClass == 1) ? warmTint : coldTint;
    float3 darkCol  = baseLin * darkTint * shadowDepth;  // shadowDepth ~0.55 → deeper than Endfield
    float3 diffuse  = lerp(darkCol, baseLin, litLevel);
    float3 col = diffuse * lightIntensity + baseLin * 0.05;

    // --- GGX specular for ALL materials (Cook-Torrance), stylised (energy kept, shape sharpened) ---
    float metal, rough;
    ClassDefaults(matClass, metal, rough);
    if (hasPacked) {
        metal = saturate(chan4(gPacked.Sample(gSamp, i.uv), metalChan));
        rough = saturate((invertRough ? 1.0 - chan4(gPacked.Sample(gSamp, i.uv), roughChan)
                                       : chan4(gPacked.Sample(gSamp, i.uv), roughChan)) + roughBias);
    }
    rough = clamp(rough, 0.05, 1.0);
    float a  = rough * rough;
    float a2 = a * a;
    float d  = (ndh * ndh * (a2 - 1.0) + 1.0);
    float D  = a2 / (3.14159 * d * d + 1e-5);
    float k  = (rough + 1.0) * (rough + 1.0) / 8.0;
    float G  = (ndv / (ndv * (1.0 - k) + k)) * (ndl / (ndl * (1.0 - k) + k));
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), baseLin, metal);
    float3 F  = F0 + (1.0 - F0) * pow(1.0 - saturate(dot(H, V)), 5.0);
    float3 ggx = (D * G) * F / (4.0 * ndv * ndl + 1e-4) * ndl;
    ggx = pow(ggx, 1.3);                                  // stylise: crisper highlight, energy kept
    col += ggx * specStrength * lightIntensity;

    // --- Hair anisotropic angel ring (matClass 2) ---
    if (matClass == 2 && hairStrength > 0.0) {
        float3 T = normalize(cross(N, float3(0.0, 1.0, 0.0)) + 1e-4);
        float sinTH = sqrt(saturate(1.0 - pow(dot(T, H), 2.0)));
        col += pow(sinTH, 100.0) * hairStrength * litLevel * lightIntensity;
    }

    // --- Rim (fresnel, stronger on the backlit side per spec) ---
    float backlit = saturate(-dot(V, L)) * 0.5 + 0.5;
    float rim = pow(1.0 - ndv, rimPower) * rimStrength * backlit;
    col += rim * rimColor;

    if (hasEmissive != 0)
        col += pow(max(gEmiss.Sample(gSamp, i.uv).rgb, 0.0), 2.2) * emissStrength;

    // --- Wuwa tonality: higher saturation + contrast (traditional-anime punchy) ---
    const float kWuwaSaturation = 1.35;
    float luma = dot(col, float3(0.2126, 0.7152, 0.0722));
    col = max(lerp(luma.xxx, col, kWuwaSaturation), 0.0);
    col *= wuwaExposure;                       // Wuwa-only brightness (GUI): dim the over-bright char

    // Texture-colour fidelity: undo the shared exposure→ACES tonemap (KEEP vibrance) so the pale-blue
    // dress stops washing to pure white — the character reproduces its painted albedo. LDR range only;
    // bright glints keep their excess so they still bloom. texFidelity blends stylised (0) ↔ exact (1).
    if (texFidelity > 0.001) {
        float3 clamped = saturate(col);
        float3 excess  = col - clamped;
        float3 tSrgb   = pow(clamped, 1.0 / 2.2);
        float  invExp  = 1.0 / max(postExposure, 0.01);
        float3 comp    = min(ACESInv(pow(tSrgb, 2.2)) * invExp, 2.0) + excess * invExp;
        col = lerp(col, comp, saturate(texFidelity));
    }
    return float4(col, 1.0);
}
