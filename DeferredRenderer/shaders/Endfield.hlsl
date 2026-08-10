// Endfield-style (Arknights: Endfield) character — dedicated FORWARD NPR+PBR pass, drawn over the
// deferred scene into the linear HDR target and depth-tested against the scene depth.
//
// Milestones: 1 unlit BaseColor + channel debug; 3 low-contrast binary toon diffuse; 4 back-face
// outline; 6 NPR+PBR hybrid spec (metal/rough from _P, GUI-selectable channels); 7 (partial) rim
// light + emissive + _N normal detail. No ILM/Ramp/Face-SDF in this rip → cool shadow tint.
cbuffer PerFrame : register(b0)
{
    row_major float4x4 viewProj;
    row_major float4x4 invViewProj;
    float3 cameraPos;        uint  viewMode;
    float3 lightDirToLight;  float zNear;
    float3 lightIntensity;   float zFar;
    row_major float4x4 lightViewProj;   // directional-light shadow matrix
    float  shadowBias;       float shadowTexel;
    float  outlineDarken;    float _pad1;
    float4 captureBg;
    float4 charDepthRange;   // (unused here — layout parity with PerFrameCB)
    float3 headFront;  float headValid;   // Endfield face SDF: head-bone basis (world) + valid flag
    float3 headRight;  float _hpad0;
    float3 headUp;     float _hpad1;
};

cbuffer EndfieldObject : register(b1)
{
    row_major float4x4 world;
    int    debugMode;      // 0 toon, 1 BaseColor, 2 Normal, 3-6 Packed.rgba, 7 Mask, 8 Emissive
    float  outlineWidth;   // outline screen-space width (px)
    float  toonThreshold;  // binary-diffuse threshold on half-lambert
    float  toonFeather;    // anti-alias width around the threshold
    float2 screenSize;     // for constant-width outline extrusion
    // Per-submesh map presence: a missing map falls back to a WHITE 1x1 in the heap, so without
    // these flags a material with no _E would sample white → glow, no _P → metallic=1, etc.
    int    hasPacked;
    int    hasEmissive;
    int    hasNormal;
    int    isHair;         // hair material → anisotropic angel-ring highlight
    int    transparentMode;// 0 = opaque (alpha-clip + full PBR); 1 = blended overlay (flat shadow tint)
    int    matClass;       // 0 cloth,1 skin,2 hair,3 eye,4 metal (used by Wuwa/ZZZ; ignored here)
    float3 matDiffuse;     // PMX material diffuse colour (texture-less overlays carry their tint here)
    float  matAlpha;       // PMX material alpha (overlays are sub-1 → alpha-blended)
    int    sphereMode;
    float  outlineScale;     // 0..1 — CPU-computed from the character's on-screen height; 0 = no outline
    float  matcapStrength; float satBoost;   // ZZZ-only; layout parity here
    float  outlineDepthBias;
    float  postExposure;     // global tonemap exposure  (texture-fidelity pre-inversion, matches ZZZ)
    float  postVibrance;     // global tonemap vibrance
    float  texFidelity;      // 0..1: undo the shared post chain so the char keeps its painted albedo (fixes 發白)
    int    nprMask;          // Endfield full-NPR: bit0 ramp,1 subsurf,2 lut,3 reflect,4 hairdetail,5 sdf,6 cm,7 hl
};

// Constant-screen-pixel outline, scaled by outlineScale (from the character's projected size). When
// the character is small on screen (outlineScale → 0) the offset vanishes, so the outline verts sit
// on the mesh and the main pass hides them → no thin aliased line on a far/small character.
float2 OutlineOffset(float4 clip, float2 nClip, float3 posW, float3 camPos)
{
    return nClip * (outlineWidth * outlineScale * 2.0 / max(screenSize, 1.0)) * clip.w;
}

cbuffer EndfieldMaterial : register(b2)   // look/material params (set once per frame)
{
    int   metalChan;      // which _P channel is metallic (0=R 1=G 2=B 3=A)
    int   roughChan;      // which _P channel is roughness
    int   invertRough;    // 1 = roughness = 1 - channel (pack stores smoothness)
    float specStrength;   // overall highlight strength (kept LOW per spec)
    float roughBias;      // added to roughness
    float rimStrength;
    float rimPower;
    float emissStrength;
    float useNormalMap;   // 1 = perturb by _N
    float hairStrength;   // anisotropic angel-ring highlight strength
    float normalYSign;    // +1 keep, -1 flip green channel (DirectX vs OpenGL normal convention)
    float shadowStrength; // 0 = ignore cast shadow, 1 = full
    float3 rimColor;      float shadowDepth;   // dark-side brightness (lower = darker; low-contrast ≈ 0.6)
    float  _zzz0, _zzz1, _zzz2, _zzz3;         // ZZZ colour-grade row (deepen/warmth/eyeLift/_mpad) — unused here
    float  charShadows, charHighlights, specFocus, sheenStrength;   // per-char tone + spec focus + leather sheen
};

Texture2D    gBase   : register(t0);   // _D BaseColor
Texture2D    gNormal : register(t1);   // _N / _HN
Texture2D    gPacked : register(t2);   // _P packed PBR (channels selectable)
Texture2D    gMask   : register(t3);   // _M mask
Texture2D    gEmiss  : register(t4);   // _E emissive
Texture2D<float>       gShadow     : register(t5);   // directional shadow map (received)
// Endfield "full NPR" experiment maps (t6..t13) — white 1x1 when absent.
Texture2D    gRamp     : register(t6);    // _RD 1D toon diffuse ramp (dark→light)
Texture2D    gSubsurf  : register(t7);    // _ST subsurface scatter tint
Texture2D    gLut      : register(t8);    // colour-grade LUT (1024x32 unwrapped 32^3)
Texture2D    gReflect  : register(t9);    // _RS reflection/spec env sphere
Texture2D    gSdf      : register(t10);   // face shadow SDF        (model-wide)
Texture2D    gCm       : register(t11);   // face colour/makeup mask (model-wide)
Texture2D    gHl       : register(t12);   // face highlight mask     (model-wide)
Texture2D    gHairDet  : register(t13);   // hair strand / hairline detail mask
SamplerState           gSamp       : register(s0);
SamplerComparisonState gShadowSamp : register(s1);
SamplerState           gClamp      : register(s2);   // clamp-linear for ramps/LUT/matcap lookups

// PCF directional-shadow visibility (1 = lit, 0 = shadowed) — the character now RECEIVES the
// directional shadow (self-shadow + cast from the scene), matching the deferred path.
float ShadowVis(float3 worldPos, float ndl)
{
    float4 lp = mul(float4(worldPos, 1.0), lightViewProj);
    lp.xyz /= lp.w;
    float2 uv = lp.xy * float2(0.5, -0.5) + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || lp.z > 1.0) return 1.0;
    float bias = max(shadowBias * (1.0 - ndl), shadowBias * 0.2);
    float cmp = lp.z - bias;
    float s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            s += gShadow.SampleCmpLevelZero(gShadowSamp, uv + float2(x, y) * shadowTexel, cmp);
    return s / 9.0;
}

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

// Outline pass VS: back-face expansion. Extrude along the normal in clip space, scaled by w and
// the screen size so the outline is a constant pixel width regardless of distance.
VSOut VSOutline(VSIn i)
{
    VSOut o;
    float4 wp   = mul(float4(i.pos, 1.0), world);
    float3 nW   = normalize(mul(i.nrm, (float3x3)world));
    // Depth-aware: push the outline shell AWAY from the camera. The body (drawn after, nearer)
    // then hides the outline wherever a self-overlap's depth gap is smaller than the push — so
    // internal overlaps lose the line while true silhouettes (large gap vs background) keep it.
    wp.xyz += normalize(wp.xyz - cameraPos) * outlineDepthBias;
    float4 clip = mul(wp, viewProj);
    float2 nClip = normalize(mul(nW, (float3x3)viewProj).xy + 1e-5);
    clip.xy += OutlineOffset(clip, nClip, wp.xyz, cameraPos);
    o.pos  = clip;
    o.posW = wp.xyz;
    o.nrmW = nW;
    o.uv   = i.uv;
    return o;
}

float4 PSOutline(VSOut i) : SV_TARGET
{
    float4 base = gBase.Sample(gSamp, i.uv);
    clip(base.a - 0.3);
    return float4(0.0, 0.0, 0.0, 1.0);   // pure black outline (user preference)
}

float chan4(float4 v, int c) { return (c == 0) ? v.r : (c == 1) ? v.g : (c == 2) ? v.b : v.a; }

// Camera-basis view-space normal → matcap/reflection UV (no view matrix needed). .y ~ how much the
// normal points "up" in view space — used to place a top studio softbox for the latex reflection.
float2 MatCapUV(float3 N, float3 posW)
{
    float3 vdir   = normalize(cameraPos - posW);
    float3 vright = normalize(cross(float3(0, 1, 0), vdir));
    float3 vup    = cross(vdir, vright);
    return float2(dot(N, vright), dot(N, vup)) * 0.5 + 0.5;
}

// Analytic inverse of the Narkowicz ACES curve (PostProcess.hlsl). Given a post-tonemap value y in
// [0,1), returns the HDR x with ACES(x)=y — used to pre-invert the shared exposure→ACES→gamma→vibrance
// tonemap so the character reproduces its painted albedo (the "整體發白" fix; same as ZZZ).
float3 ACESInv(float3 y)
{
    y = clamp(y, 0.0, 0.9999);
    float3 disc = max(-1.0127 * y * y + 1.3702 * y + 0.0009, 0.0);
    return (0.03 - 0.59 * y - sqrt(disc)) / (4.86 * y - 5.02);
}

// Colour-grade LUT lookup: 32^3 cube unwrapped to a 1024x32 strip (skincolor / cloth grade). Input
// a gamma-space [0,1] albedo. Ported VERBATIM from endfield_face.hlsl EfFaceSampleSkinLut: MyZmd's
// 1024x32 LUT is 32 horizontal 32x32 tiles — TILE index from R, within-tile U from G, V from B (flipped).
float3 ApplyLut(Texture2D lut, float3 albedoSrgb)
{
    albedoSrgb = saturate(albedoSrgb);
    albedoSrgb = albedoSrgb.brg;   // EF_*_LUT_USE_BRG=1 (default): B chooses the slice, R/G the plane
    float2 lutUv     = albedoSrgb.xz * float2(31.0, 0.96875);
    float  lutFloorX = floor(lutUv.x);
    float2 lutUvYZ   = albedoSrgb.yz * float2(0.0302734375, 0.96875) + float2(0.00048828125, 0.015625);
    float  lutUvX    = lutFloorX * 0.03125 + lutUvYZ.x;
    float2 lutUvFinal = float2(lutUvX, 1.0 - lutUvYZ.y);
    float  lutTileLerp = albedoSrgb.x * 31.0 - lutFloorX;
    float3 c0 = lut.SampleLevel(gClamp, lutUvFinal, 0).rgb;
    float3 c1 = lut.SampleLevel(gClamp, lutUvFinal + float2(0.03125, 0.0), 0).rgb;
    return lerp(c0, c1, lutTileLerp);
}

// ===== Endfield toon-lighting core — ported verbatim from the reference endfield_lighting.hlsl
// (chris0214 Arknights-Endfield-MME-Shader). See ENDFIELD_RENDERING.md §2. =====
float Ef_Lum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Back-light: camera facing away from the light lifts the dark side.
float EfBackLight(float3 camFwd, float2 lightDirXZ)
{
    float2 cf = normalize(camFwd.xz + float2(1e-6, 0.0));
    float  b  = saturate(-dot(cf, lightDirXZ));
    float  by = saturate(-abs(camFwd.y) + 0.75);
    by = by * by * (3.0 - 2.0 * by);   // smoothstep
    return b * by;
}

// Ramp U: quadratic-NoL remap (NOT half-Lambert), back-light lifts the dark side.
float EfRampNoL(float NoL, float backLight)
{
    float rampN  = 0.5 - 0.5 * NoL * NoL;
    float finalN = clamp(rampN * backLight + NoL, -1.0, 1.0);
    return finalN * 0.5 + 0.5;
}

// 3-layer diffuse: AO & shadow & ramp.a SELECT between the light albedo and the dark colour
// (dark = LUT colour, later). They never multiply toward black — that's the "no dirty/grey dark" key.
float3 EfDiffuseBRDF(float3 baseLight, float3 baseDark, float ao, float shadow, float4 ramp, float rampNoF)
{
    float3 diffDark     = baseDark;
    float3 diffDarkAttn = diffDark * 0.65;
    float  aoShaNoF     = (ao * shadow) * rampNoF;
    float  minShadow    = min(min(ao, shadow), ramp.w);
    float3 darkLerp     = lerp(diffDarkAttn, diffDark, saturate(aoShaNoF + ramp.w));
    return lerp(darkLerp, baseLight, minShadow);
}

// Ramp colour: saturation-adaptive HUE only (grey ramp → neutral), with luminance compensation so
// the ramp never acts as a second brightness multiplier.
float3 EfApplyRampColor(float3 diffuse, float4 ramp)
{
    float rampSat  = max(max(ramp.r, ramp.g), ramp.b) - min(min(ramp.r, ramp.g), ramp.b);
    float3 rampEff = ramp.rgb * rampSat + 1.0 - rampSat;
    float3 diffRamp = diffuse * rampEff;
    float  rampCtrl = clamp(Ef_Lum(diffuse) / max(0.01, Ef_Lum(diffRamp)), 0.0, 1.5);
    return diffRamp * rampCtrl;
}

// Face SDF (Goo) shadow → the ramp U for the face. 1 = lit, 0 = shadowed. Ported from endfield_face.hlsl.
// projLight = the to-light L projected onto the face plane (normalized), side = dot(projLight, headRight)
// — both computed once in the caller and reused by the Rim. A continuous atan2 angular threshold drives a
// sharp sigmoid over the averaged SDF R/G channels; the SDF UV is mirrored by the light side.
float EfFaceSdfMask(Texture2D sdfTex, float2 uv, float3 projLight, float side, float3 hFront)
{
    float2 sdfUV = float2(lerp(1.0 - uv.x, uv.x, step(0.0, side)), uv.y);  // right-side light keeps U
    float4 sdf   = sdfTex.Sample(gClamp, sdfUV);
    float  angle01   = abs(atan2(side, dot(projLight, hFront))) * (1.0 / 3.14159265);
    float  sdfAvg    = saturate(0.5 * (sdf.r + sdf.g));
    float  gooCenter = saturate(angle01) + 0.1;   // EF_FACE_SDF_GOO_CENTER
    float  gooExp    = -3.0 * 0.5 * (sdfAvg - gooCenter);   // EF_FACE_SDF_GOO_SHARP = 0.5
    float  gooDenom  = 1.0 + pow(100000.0, gooExp);         // EF_FACE_SDF_GOO_BASE
    return saturate(1.0 / max(gooDenom, 1e-4));
}

// Kajiya-Kay hair angel ring, distilled from endfield_hair_specular.hlsl. The strand binormal =
// cross(HN, strandDir); the RS LUT is indexed by along-strand anisotropy (U = sin^powStr · reflec) and
// a view/light-facing gate (V). Uses a CAMERA-flattened normal so the ring rides the head, not the light.
float3 EfHairKajiyaKay(Texture2D rsTex, float3 HN, float3 strandTan, float3 camFwd, float3 V, float3 L,
                       float reflec, float powStr, float vPow)
{
    float3 camRight = normalize(cross(float3(0, 1, 0), camFwd) + float3(1e-6, 0, 0));
    float3 cylN     = normalize(HN - dot(HN, camRight) * camRight);
    float3 flatHN   = normalize(lerp(HN, cylN, 0.6));
    float3 strandO  = strandTan - HN * dot(HN, strandTan);
    float3 strandD  = dot(strandO, strandO) > 1e-8 ? normalize(strandO) : strandTan;
    float3 hairBin  = normalize(cross(HN, strandD));
    float  ToH      = dot(normalize(V + L), hairBin);
    float  sinT     = sqrt(max(1e-4, 1.0 - ToH * ToH));
    float  lutU     = saturate(exp2(powStr * log2(sinT)) * reflec);
    float2 vdP      = float2(dot(V,  camRight), dot(V,  camFwd));
    float2 hnP      = float2(dot(HN, camRight), dot(HN, camFwd));
    float  VoHN     = pow(saturate(dot(vdP, hnP)), vPow);
    float  lutV     = VoHN * VoHN * step(0.0, ToH);
    return rsTex.Sample(gClamp, float2(lutU, lutV)).rgb;
}

// Tangent-space normal perturbation from screen-space derivatives (no vertex tangents needed).
float3 PerturbNormal(float3 N, float3 posW, float2 uv, float3 mapN)
{
    float3 dp1 = ddx(posW), dp2 = ddy(posW);
    float2 du1 = ddx(uv),   du2 = ddy(uv);
    float3 T = dp1 * du2.y - dp2 * du1.y;
    float3 B = dp2 * du1.x - dp1 * du2.x;
    float invmax = rsqrt(max(dot(T, T), dot(B, B)) + 1e-8);
    return normalize(mapN.x * (T * invmax) + mapN.y * (B * invmax) + mapN.z * N);
}

float4 PSMain(VSOut i) : SV_TARGET
{
    float4 base = gBase.Sample(gSamp, i.uv);
    // Fold in the PMX material colour/alpha. Texture-less overlay meshes (eye-shadow / hair-shadow)
    // carry a dark diffuse + sub-1 alpha here → this turns the white fallback into their true tint.
    base.rgb *= matDiffuse;
    base.a   *= matAlpha;
    // Opaque materials use a hard cutout (lashes/hair edges); blended overlays only drop fully clear.
    clip(base.a - (transparentMode == 0 ? 0.3 : 0.004));

    // --- Debug channel views (2..8) ---
    if (debugMode >= 2 && debugMode <= 8) {
        float3 c;
        // Gate each channel by the map's presence, so a material that LACKS the map shows BLACK
        // rather than the white 1x1 fallback (which made the _P-less face read as "fully metallic").
        if      (debugMode == 2) c = (hasNormal   != 0) ? gNormal.Sample(gSamp, i.uv).rgb : float3(0, 0, 0);
        else if (debugMode == 3) c = (hasPacked   != 0) ? gPacked.Sample(gSamp, i.uv).rrr : float3(0, 0, 0);
        else if (debugMode == 4) c = (hasPacked   != 0) ? gPacked.Sample(gSamp, i.uv).ggg : float3(0, 0, 0);
        else if (debugMode == 5) c = (hasPacked   != 0) ? gPacked.Sample(gSamp, i.uv).bbb : float3(0, 0, 0);
        else if (debugMode == 6) c = (hasPacked   != 0) ? gPacked.Sample(gSamp, i.uv).aaa : float3(0, 0, 0);
        else if (debugMode == 7) c = gMask.Sample(gSamp, i.uv).rgb;   // (no hasMask flag)
        else                     c = (hasEmissive != 0) ? gEmiss.Sample(gSamp, i.uv).rgb : float3(0, 0, 0);
        return float4(pow(max(c, 0.0), 2.2), 1.0);
    }

    float3 baseLin = pow(max(base.rgb, 0.0), 2.2);
    if (debugMode == 1) return float4(baseLin, 1.0);   // unlit BaseColor

    // --- Normal (perturbed by _N only where the material actually has one; face has no _N →
    //     geometric normal, per spec. Without the hasNormal gate the white 1x1 fallback would
    //     read (1,1,1) and skew the normal) ---
    float3 N = normalize(i.nrmW);
    if (useNormalMap > 0.5 && hasNormal != 0) {
        float3 mapN;
        mapN.xy = gNormal.Sample(gSamp, i.uv).xy * 2.0 - 1.0;
        mapN.y *= normalYSign;   // DirectX (Y+) vs OpenGL (Y-) tangent-normal green-channel convention
        // RECONSTRUCT Z from XY — these rips ship 2-channel (RG) normal maps with B=0; reading B
        // directly gave z=-1, which FLIPPED the surface normal → wrong lighting / the "weird
        // reflection". This is also correct for 3-channel maps (a unit normal has B=sqrt(1-x²-y²)).
        mapN.z = sqrt(saturate(1.0 - dot(mapN.xy, mapN.xy)));
        N = PerturbNormal(N, i.posW, i.uv, mapN);
    }
    float3 L = normalize(lightDirToLight);
    float3 V = normalize(cameraPos - i.posW);
    float3 H = normalize(L + V);

    // --- STEP 1: Endfield core toon diffuse (ported from endfield_lighting.hlsl). Quadratic-NoL ramp,
    //     3-layer blend where AO/shadow/ramp.a SELECT the dark colour (never multiply toward black),
    //     saturation-adaptive ramp hue. dark colour is a plain darken here → STEP 2 swaps in the LUT. ---
    float NoL       = dot(N, L);
    float rawShadow = ShadowVis(i.posW, saturate(NoL));         // 1 lit, 0 shadowed
    bool   isFaceMat = (matClass == 1) && ((nprMask & 2) != 0);
    float3 faceMask  = isFaceMat ? gSubsurf.Sample(gSamp, i.uv).rgb : float3(1, 0, 0);   // _ST
    float4 faceCm    = (isFaceMat && (nprMask & 64)) ? gCm.Sample(gSamp, i.uv) : float4(0, 0, 0, 0);

    // Face-plane light projection (the SDF and the Rim share it).
    float3 projLight = L;  float side = 0.0;
    if (isFaceMat) {
        projLight = L - dot(L, headUp) * headUp;
        float pl  = length(projLight);
        projLight = (pl > 1e-4) ? projLight / pl : headFront;
        side      = dot(projLight, headRight);
    }

    // STEP 4: received scene shadow. Body: _ST.R lift + shadowStrength. Face: the cm_M gate so the FRONT
    // never takes the scene shadow — only the neck (cm.g) and the camera-behind back of head (cm.b) do.
    float shadow;
    if (isFaceMat && (nprMask & 64)) {
        float camFrontDot      = dot(headFront, V);            // >0 = camera in front of the face
        float cameraShadowArea = saturate(-2.0 * camFrontDot);
        cameraShadowArea = cameraShadowArea * cameraShadowArea * (3.0 - 2.0 * cameraShadowArea);
        float shadowArea = max(saturate(faceCm.g), cameraShadowArea * saturate(faceCm.b));
        shadow = lerp(1.0, rawShadow, saturate(shadowArea));
    } else {
        shadow = isFaceMat ? lerp(1.0, rawShadow, faceMask.r) : rawShadow;
        shadow = lerp(1.0, shadow, shadowStrength);
    }

    float3 camFwd    = normalize(i.posW - cameraPos);
    float  backLight = EfBackLight(camFwd, normalize(L.xz + float2(1e-6, 0.0)));
    float4 P  = gPacked.Sample(gSamp, i.uv);
    // STEP 3: the face drives the ramp U from the SDF Goo shadow (not NoL); cm_M.g blends toward
    // geometric NoL on the neck/non-face. Face AO lives in _D.alpha. Everything else uses the ramp NoL.
    float  rampNoF;
    float  ao;
    if (isFaceMat && (nprMask & 32) && headValid > 0.5) {
        float gooMask  = EfFaceSdfMask(gSdf, i.uv, projLight, side, headFront);
        rampNoF = lerp(gooMask, saturate(NoL), saturate(faceCm.g));
        ao      = saturate(base.a);                            // face AO = _D.alpha
    } else {
        rampNoF = EfRampNoL(NoL, backLight);
        ao      = hasPacked ? saturate(P.b) : 1.0;
    }
    // edbg 9 (face only): R = SDF Goo mask (1 lit / 0 shadow), G = headValid, B = has-SDF-map.
    if (debugMode == 9 && isFaceMat) return float4(rampNoF, headValid, (nprMask & 32) ? 1.0 : 0.0, 1.0);
    float4 rd = (nprMask & 1) ? gRamp.Sample(gClamp, float2(rampNoF, 0.5)) : float4(1, 1, 1, 1);

    // STEP 2: dark colour = the skin/cloth LUT lookup (sRGB in → sRGB out → linearise), NOT a darken.
    float3 baseDark;
    if (nprMask & 4) {
        float3 lutSrgb = ApplyLut(gLut, pow(max(baseLin, 0.0), 1.0 / 2.2));
        baseDark = pow(max(lutSrgb, 0.0), 2.2);
    } else {
        baseDark = baseLin * 0.35;
    }
    // STEP 4: face SSS (cm_M.r) — warm the BRIGHT albedo where the face turns away from view; the LUT
    // dark colour is deliberately left untouched, so shadows keep their authored tone.
    float3 baseLight = baseLin;
    if (isFaceMat && (nprMask & 64)) {
        float sssNoV     = saturate(dot(N, V)) * 0.85 + 0.15;
        float headFrontD = saturate(dot(headFront, V));
        float viewSss    = lerp(saturate(headFrontD + 0.5), 1.0, saturate(faceCm.g)) * saturate(faceCm.r);
        float sssArea    = saturate(0.5 * viewSss * (1.0 - sssNoV));   // EF_FACE_SSS_AREA = 0.5
        const float3 SSS_COLOR = float3(0.822936, 0.669170, 0.648409);
        baseLight = baseLin * lerp(float3(1, 1, 1), SSS_COLOR, sssArea);
    }
    float3 diffuse   = EfDiffuseBRDF(baseLight, baseDark, ao, shadow, rd, rampNoF);
    diffuse          = EfApplyRampColor(diffuse, rd);
    float3 col = diffuse * lightIntensity;
    float  lit = saturate(min(min(ao, shadow), rd.a));          // lit-side factor for spec/rim gating

    // STEP 4: face Rim (cm_M.a) — authored rim region × lit UV half × grazing/front-facing NoV × front-light.
    if (isFaceMat && (nprMask & 64)) {
        float faceHalf    = step(i.uv.x, 0.5);
        faceHalf          = lerp(1.0 - faceHalf, faceHalf, step(0.0, side));
        float faceNoV     = saturate(dot(headFront, V));
        float rimViewMask = saturate(faceNoV - 0.75);           // EF_FACE_RIM_NOV_THRESHOLD
        float rimFront    = saturate(dot(projLight, headFront));
        float rimMask     = saturate(saturate(faceCm.a) * faceHalf * rimViewMask * rimFront);
        col += rimMask * lightIntensity;                        // EF_FACE_RIM_COLOR = white
    }

    // Blended overlays (eye-shadow / hair-shadow) are just a flat dark tint over the face — skip
    // all highlights (spec/rim/hair/emissive would put white glints on the black shadow) and blend.
    if (transparentMode != 0)
        return float4(col, base.a);

    // --- Milestone 6: NPR+PBR hybrid highlight (metal/rough from _P; kept low-intensity) ---
    // No _P → neutral dielectric (metal 0, medium roughness) rather than the white fallback's 1,1.
    // (P was sampled above for AO; reuse it for metal/rough.)
    float  metal = hasPacked ? saturate(chan4(P, metalChan)) : 0.0;
    float  rough = hasPacked
                 ? saturate((invertRough ? 1.0 - chan4(P, roughChan) : chan4(P, roughChan)) + roughBias)
                 : 0.5;
    // Metals have (almost) NO diffuse — their look is the reflection, not a lit base colour. Our
    // shader lit the metal albedo as diffuse, so gunmetal/buckles came out grey instead of black.
    // Suppress the diffuse where metallic (from _P.R) so those parts read dark, per the game.
    col *= (1.0 - metal * 0.9);
    float  focusRough = rough * (1.0 - specFocus * 0.7);   // specFocus tightens the GGX lobe ONLY
    float  ndh = saturate(dot(N, H));
    float  ndl = saturate(dot(N, L));
    float  ndv = saturate(dot(N, V));
    // NPR: clean narrow Blinn-Phong highlight, gated to the lit side. specFocus shifts + narrows the
    // smoothstep window so the highlight becomes a smaller, more concentrated spot.
    float  nprSpec = smoothstep(lerp(0.72, 0.90, specFocus), lerp(0.76, 0.93, specFocus), ndh) * lit;
    // PBR: GGX (Cook-Torrance NDF), higher roughness reads matte per spec.
    float  a  = max(focusRough * focusRough, 0.002);
    float  d  = (ndh * ndh * (a * a - 1.0) + 1.0);
    float  ggx = (a * a) / (3.14159 * d * d + 1e-5);
    float  pbrSpec = ggx * ndl;
    float3 specTint = lerp(float3(1.0, 1.0, 1.0), baseLin, metal);   // metal tints the highlight
    float  spec = lerp(nprSpec, pbrSpec, metal) * specStrength;
    // Face _ST.G marks the nose/cheek highlight-strengthen region → lift the highlight there (kept
    // restrained per the low-contrast style).
    if (isFaceMat) spec *= (1.0 + faceMask.g * 0.6);
    col += spec * specTint * lightIntensity;

    // --- Leather / latex sheen: the broad, wet specular that makes the bodysuit read as leather.
    //     It is NOT a tight glint — it is a WIDE soft lobe + a Fresnel edge-glow that spreads over the
    //     whole curved surface. Gated by hasPacked (face/skin have no _P → NO sheen, so the face stays
    //     matte) and scaled by SMOOTHNESS² from _P.A, so only the low-roughness leather glows strongly
    //     while matte cloth/skin stay flat — this is how the game gets "衣服亮、臉不亮" from one control. ---
    if (hasPacked != 0 && sheenStrength > 0.0) {
        // VIRTUAL front key light (camera-relative, tilted up): the scene sun is at a fixed off-angle
        // that never reflects toward the camera, so add a fake studio light in front of the character
        // just for this sheen — its reflection lands facing the camera → the broad latex sweep. NOT
        // gated by the scene shadow (latex reflects the environment even on the shadow side).
        // Key light tilted strongly UP-and-front (not straight at the camera) so the reflection is a
        // DEFINED band down the lit curves, not a flat wash over the whole camera-facing surface.
        // "Leather" = a DARK dielectric (the latex bodysuit), detected by ALBEDO DARKNESS (roughness
        // can't separate it from the also-mid-rough white jacket, but dark-vs-white does).
        float  albLuma    = dot(baseLin, float3(0.2126, 0.7152, 0.0722));
        float  leatherAmt = saturate(1.0 - albLuma * 3.5) * (1.0 - metal);
        float2 mc         = MatCapUV(N, i.posW);
        // REAL matcap (bound to t3 for Endfield when the model ships one — a dark studio-env sphere
        // with a bright window/star reflection) → dark latex + wet highlight. Falls back to a
        // procedural studio band if no matcap. LERP (not add) so the dark areas stay dark, no wash.
        float3 studio;
        if (sphereMode != 0)   // sphereMode reused as "has matcap" for Endfield
            studio = pow(max(gMask.Sample(gSamp, float2(mc.x, 1.0 - mc.y)).rgb, 0.0), 2.2);
        else {
            float band = smoothstep(0.55, 0.97, mc.y);
            studio = lerp(float3(0.015, 0.015, 0.02), float3(1.0, 1.0, 1.0), band);
        }
        float  reflW = leatherAmt * saturate(sheenStrength) * lerp(0.5, 1.0, pow(1.0 - ndv, 3.0));
        col = lerp(col * lerp(1.0, 0.55, leatherAmt * saturate(sheenStrength)), studio, saturate(reflW));
    }

    // --- STEP 5: hair Kajiya-Kay "angel ring" (ported from endfield_hair_specular.hlsl). Strand
    //     direction from the UV gradient (hair runs along V); the RS LUT (t9) supplies the highlight
    //     colour/shape; P.g is the spec rhythm; P.a × hairline (t13) inks the dark strand lines. ---
    if (isHair != 0 && hairStrength > 0.0) {
        // Along-strand world tangent from the V-axis UV gradient.
        float3 dpx = ddx(i.posW), dpy = ddy(i.posW);
        float2 dux = ddx(i.uv),   duy = ddy(i.uv);
        float  det = dux.x * duy.y - dux.y * duy.x;
        float3 strandTan = normalize((dpx * dux.y - dpy * duy.y) * (det < 0.0 ? -1.0 : 1.0) + float3(1e-6, 0, 0));
        float  reflec = hasPacked ? saturate(P.g + 0.15) : 0.5;   // _P.G spec rhythm
        float3 kk = (nprMask & 8)
                  ? EfHairKajiyaKay(gReflect, N, strandTan, camFwd, V, L, reflec, 8.0, 2.0)
                  : pow(sqrt(saturate(1.0 - dot(normalize(cross(N, float3(0,1,0))), H) * dot(normalize(cross(N, float3(0,1,0))), H))), 120.0).xxx;
        col += kk * hairStrength * lit * lightIntensity;
        // Dark strand lines: _P.A × hairline mask deepens the ink between strands.
        if (nprMask & 16) {
            float darkLine = saturate(P.a) * saturate(gHairDet.Sample(gSamp, i.uv).r);
            col *= lerp(1.0, 0.65, darkLine);
        }
    }

    // --- Milestone 7: rim light (subtle, environment-tinted) + emissive ---
    float rim = pow(1.0 - ndv, rimPower) * rimStrength * lit;
    col += rim * rimColor;
    if (hasEmissive != 0)                     // no _E → no glow (was sampling the white fallback)
        col += pow(max(gEmiss.Sample(gSamp, i.uv).rgb, 0.0), 2.2) * emissStrength;

    // --- Character-only Highlights / Shadows detail (luminance-masked, MULTIPLICATIVE so it survives
    //     the global exposure→ACES→gamma tonemap). shadows>0 lifts dark detail; highlights<0 recovers
    //     bright detail (pulls blown highlights down). Only the character is affected, not Sponza. ---
    if (abs(charShadows) + abs(charHighlights) > 1e-4) {
        float l      = sqrt(saturate(dot(col, float3(0.2126, 0.7152, 0.0722))));   // perceptual-ish luma
        float shMask = 1.0 - smoothstep(0.0, 0.5, l);
        float hiMask = smoothstep(0.5, 1.0, l);
        col *= 1.0 + charShadows    * 0.6 * shMask;
        col *= 1.0 + charHighlights * 0.6 * hiMask;
        col = max(col, 0.0);
    }

    // --- Texture-colour fidelity (fixes "整體發白"): the character is composited into the HDR scene
    //     then run through the shared exposure→ACES→gamma→vibrance tonemap tuned for Sponza, which
    //     brightens + desaturates it (a dark latex bodysuit washes to grey). Pre-invert that chain so
    //     the COMPOSITED character lands back on the colour we shaded. LDR range only; HDR glints keep
    //     their excess so specular still blooms. texFidelity blends stylised (0) ↔ exact texture (1). ---
    if (texFidelity > 0.001) {
        float3 clamped = saturate(col);
        float3 excess  = col - clamped;
        float3 tSrgb   = pow(clamped, 1.0 / 2.2);
        float  invExp  = 1.0 / max(postExposure, 0.01);
        // Undo exposure + ACES ONLY (this is what whitened the dark leather). KEEP vibrance — undoing
        // it was what drained the skin/hair colour. (postVibrance is intentionally unused here now.)
        float3 comp    = min(ACESInv(pow(tSrgb, 2.2)) * invExp, 2.0) + excess * invExp;
        col = lerp(col, comp, saturate(texFidelity));
    }
    return float4(col, base.a);
}
