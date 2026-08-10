// Zenless Zone Zero (绝区零) style — forward Ramp + Mask / MatCap NPR. American-comic look: high
// saturation, clean colour blocks, sharp shading breaks, thick inked outline. MMD ramp texture
// (t2) drives the shadow tone; sphere/MatCap (t3) gives metal its sheen; the character RECEIVES a
// self-shadow (directional shadow map). Shares the forward root signature / CBs with Endfield/Wuwa.
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

cbuffer ZzzObject : register(b1)   // same layout as EndfieldObject
{
    row_major float4x4 world;
    int    debugMode;
    float  outlineWidth;
    float  toonThreshold;
    float  toonFeather;
    float2 screenSize;
    int    hasToon;          // (hasPacked slot) 1 = a real toon ramp is bound on t2
    int    hasEmissive;
    int    hasNormal;
    int    isHair;
    int    transparentMode;
    int    matClass;         // 0 cloth,1 skin,2 hair,3 eye,4 metal
    float3 matDiffuse;
    float  matAlpha;
    int    sphereMode;       // 0 none, 1 mul, 2 add
    float  outlineScale;     // 0..1 from the character's on-screen height; 0 = no outline
    float  matcapStrength;   // metal MatCap strength
    float  satBoost;         // extra saturation
    float  outlineDepthBias;
    float  postExposure;     // global tonemap exposure  (texture-fidelity pre-inversion)
    float  postVibrance;     // global tonemap vibrance  (texture-fidelity pre-inversion)
    float  texFidelity;      // 0..1 strength of the pre-inversion (0 = stylised, 1 = exact texture)
};

float hash12(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }

cbuffer ZzzMaterial : register(b2)   // same layout as EndfieldMaterial
{
    int   metalChan;   int roughChan;  int invertRough;  float specStrength;
    float roughBias;   float rimStrength;  float rimPower;  float emissStrength;
    float useNormalMap; float hairStrength; float normalYSign; float shadowStrength;
    float3 rimColor;   float shadowDepth;
    float deepen;      float warmth;   float eyeLift;   float _mpad;   // ZZZ colour grade
    float charShadows; float charHighlights; float specFocus; float _mpad2;   // per-char tone + spec focus
};

Texture2D    gBase   : register(t0);
Texture2D    gNormal : register(t1);
Texture2D    gToon   : register(t2);   // MMD toon ramp (ZZZ)
Texture2D    gSphere : register(t3);   // MMD sphere / MatCap (ZZZ)
Texture2D    gEmiss  : register(t4);
Texture2D<float>       gShadow     : register(t5);
SamplerState           gSamp       : register(s0);
SamplerComparisonState gShadowSamp : register(s1);

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

VSOut VSOutline(VSIn i)   // hand-drawn ink line: distance-faded + UNEVEN width (per-vertex jitter)
{
    VSOut o;
    float4 wp   = mul(float4(i.pos, 1.0), world);
    float3 nW   = normalize(mul(i.nrm, (float3x3)world));
    wp.xyz += normalize(wp.xyz - cameraPos) * outlineDepthBias;   // depth-aware push-back (see Endfield)
    float4 clip = mul(wp, viewProj);
    float2 nClip = normalize(mul(nW, (float3x3)viewProj).xy + 1e-5);
    float w = outlineWidth * outlineScale;   // 0 when the character is small on screen → no outline
    // UNIFORM width. (The previous per-vertex hash jitter (0.6..1.35×) read as an uneven "有粗有細"
    // line with visible width steps between vertices; a clean constant width + SSAA on the edge
    // looks far better. screenSize is the WINDOW size so the line stays a fixed px width after the
    // supersample downsample.)
    clip.xy += nClip * (w * 2.0 / max(screenSize, 1.0)) * clip.w;
    o.pos = clip; o.posW = wp.xyz; o.nrmW = nW; o.uv = i.uv;
    return o;
}

float4 PSOutline(VSOut i) : SV_TARGET
{
    float4 base = gBase.Sample(gSamp, i.uv);
    base.rgb *= matDiffuse; base.a *= matAlpha;
    clip(base.a - 0.3);
    // Ink = a DEEP tint of the local BaseColor (saturation pushed, then darkened) — a coloured ink,
    // not grey — with subtle density variation so the line reads as hand-drawn.
    float3 bl = pow(max(base.rgb, 0.0), 2.2);
    float  luma = dot(bl, float3(0.2126, 0.7152, 0.0722));
    float3 ink = max(lerp(luma.xxx, bl, 1.3), 0.0) * (0.16 + 0.08 * hash12(i.uv * 512.0));
    return float4(ink, 1.0);
}

// Camera-facing basis → MatCap/sphere UV (no separate view matrix needed).
float2 MatCapUV(float3 N, float3 posW)
{
    float3 vdir  = normalize(cameraPos - posW);
    float3 vright = normalize(cross(float3(0, 1, 0), vdir));
    float3 vup    = cross(vdir, vright);
    return float2(dot(N, vright), dot(N, vup)) * 0.5 + 0.5;
}

// Analytic inverse of the Narkowicz ACES curve used by the global tonemap (PostProcess.hlsl).
// Given a post-tonemap value y in [0,1), returns the HDR x with ACES(x) = y. Used to pre-invert
// the tonemap so the character reproduces its texture colour after the shared post chain.
float3 ACESInv(float3 y)
{
    y = clamp(y, 0.0, 0.9999);
    float3 disc = max(-1.0127 * y * y + 1.3702 * y + 0.0009, 0.0);
    return (0.03 - 0.59 * y - sqrt(disc)) / (4.86 * y - 5.02);
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
        else if (debugMode == 3) c = gToon.Sample(gSamp, i.uv).rrr;
        else if (debugMode == 4) c = gToon.Sample(gSamp, i.uv).ggg;
        else if (debugMode == 5) c = gToon.Sample(gSamp, i.uv).bbb;
        else if (debugMode == 6) c = gSphere.Sample(gSamp, i.uv).rgb;
        else if (debugMode == 7) c = gSphere.Sample(gSamp, MatCapUV(normalize(i.nrmW), i.posW)).rgb;
        else                     c = gEmiss.Sample(gSamp, i.uv).rgb;
        return float4(pow(max(c, 0.0), 2.2), 1.0);
    }

    float3 baseLin = pow(max(base.rgb, 0.0), 2.2);
    if (debugMode == 1) return float4(baseLin, 1.0);

    float3 N = normalize(i.nrmW);
    if (useNormalMap > 0.5 && hasNormal != 0) {
        float3 dp1 = ddx(i.posW), dp2 = ddy(i.posW);
        float2 du1 = ddx(i.uv),   du2 = ddy(i.uv);
        float3 T = dp1 * du2.y - dp2 * du1.y;
        float3 B = dp2 * du1.x - dp1 * du2.x;
        float3 mn; mn.xy = gNormal.Sample(gSamp, i.uv).xy * 2.0 - 1.0; mn.y *= normalYSign;
        mn.z = sqrt(saturate(1.0 - dot(mn.xy, mn.xy)));   // reconstruct Z (RG normal maps have B=0)
        float im = rsqrt(max(dot(T, T), dot(B, B)) + 1e-8);
        N = normalize(mn.x * T * im + mn.y * B * im + mn.z * N);
    }
    float3 L = normalize(lightDirToLight);
    float3 V = normalize(cameraPos - i.posW);
    float3 H = normalize(L + V);
    float  NdotL = dot(N, L);
    float  ndl = saturate(NdotL), ndv = saturate(dot(N, V)), ndh = saturate(dot(N, H));

    if (transparentMode != 0) {  // eye/hair shadow overlays (e.g. 目影): flat dark blend, faded by eyeLift
        float3 shTint  = lerp(float3(0.72, 0.74, 0.85), float3(1, 1, 1), eyeLift);   // toward neutral
        float  shDepth = lerp(shadowDepth, 1.0, eyeLift * 0.6);                       // raise the floor
        float  shAlpha = base.a * (1.0 - eyeLift);                                    // eyeLift 1 → overlay gone
        return float4(baseLin * shTint * shDepth * lightIntensity, shAlpha);
    }

    // --- Ramp diffuse: lit value (half-lambert × self-shadow), then the MMD toon ramp gives the
    //     shadow tone. Clean colour blocks: a narrow smoothstep sharpens the shading break. ---
    float halfLambert = NdotL * 0.5 + 0.5;
    // Eyeball (眼白/眼睛/目光) is rendered CLEAN like real anime/ZZZ eyes: no self-shadow, lifted
    // toward fully-lit so the ramp samples the bright end → the painted iris/sclera shows through
    // instead of a dark shadow band. eyeLift dials how bright (0 = half-lit, 1 = flat unlit albedo).
    float shadow = (matClass == 3) ? 1.0 : ShadowVis(i.posW, ndl);   // ZZZ self-shadow, but not on eyes
    float litValue = saturate(halfLambert * lerp(1.0, shadow, 0.85 * shadowStrength));
    if (matClass == 3)
        litValue = lerp(litValue, 1.0, saturate(0.5 + 0.5 * eyeLift));
    // Sharpen toward clean colour blocks, but ANTI-ALIAS the shading break in screen space: widen
    // the smoothstep to at least ~1.5 px of on-screen gradient (fwidth) so the boundary between two
    // colour blocks never hard-steps or shimmers/"馬賽克" when the camera moves. toonFeather sets the
    // artistic minimum softness; fwidth guarantees the edge is always resolved to sub-pixel.
    float aaW = max(toonFeather, fwidth(litValue) * 0.75);
    float lit = smoothstep(toonThreshold - aaW, toonThreshold + aaW, litValue);
    float toneU = lerp(0.15, 0.95, lit);
    float3 ramp = hasToon ? gToon.Sample(gSamp, float2(toneU, 0.5)).rgb
                          : lerp(float3(0.55, 0.6, 0.85) * shadowDepth, float3(1, 1, 1), lit);
    float3 col = baseLin * ramp * lightIntensity + baseLin * 0.04;

    // --- Material-branch specular (NPR for skin/cloth, MatCap/PBR for metal) ---
    if (matClass == 4) {                                         // metal: MatCap sheen (亮而銳)
        float3 mc = pow(max(gSphere.Sample(gSamp, MatCapUV(N, i.posW)).rgb, 0.0), 2.2);
        // Push the matcap toward a metallic look: raise contrast + tint by the base albedo, then
        // add strongly (matcapStrength). Metal reflects its own colour → multiply-tint the sheen.
        mc = (mc * mc) * lerp(float3(1,1,1), baseLin * 2.0, 0.6);
        if (sphereMode == 1) col *= max(mc, 0.2);                // multiply sphere
        else                 col += mc * matcapStrength;         // add (default)
        col += pow(ndh, lerp(120.0, 320.0, specFocus)) * specStrength * 2.0 * lightIntensity;  // sharp glint (specFocus tightens)
        col *= 1.15;                                             // metals read brighter/punchier
    } else if (matClass == 2) {                                  // hair angel ring
        float3 T2 = normalize(cross(N, float3(0, 1, 0)) + 1e-4);
        float sinTH = sqrt(saturate(1.0 - pow(dot(T2, H), 2.0)));
        col += pow(sinTH, lerp(110.0, 300.0, specFocus)) * hairStrength * lit * lightIntensity;   // specFocus tightens the ring
    } else {                                                     // skin / cloth: crisp NPR highlight
        float spec = smoothstep(lerp(0.68, 0.90, specFocus), lerp(0.70, 0.93, specFocus), ndh) * lit;  // specFocus → smaller spot
        col += spec * specStrength * 0.4 * lightIntensity;
    }

    // Rim — TINTED BY THE LOCAL ALBEDO (not a white/blue wash) and kept subtle on skin/cloth so a
    // cylindrical limb (legs / stockings) doesn't get a broad grazing-angle rim that washes the
    // whole surface to white. Metals keep the brighter light-coloured rim.
    float rimAmt  = pow(1.0 - ndv, rimPower) * rimStrength * (saturate(-dot(V, L)) * 0.5 + 0.5);
    float3 rimTint = (matClass == 4) ? rimColor : lerp(baseLin, rimColor, 0.25);
    float  rimGain = (matClass == 1 || matClass == 0) ? 0.4 : 1.0;   // skin/cloth muted
    col += rimAmt * rimTint * rimGain;
    if (hasEmissive != 0)
        col += pow(max(gEmiss.Sample(gSamp, i.uv).rgb, 0.0), 2.2) * emissStrength;

    // ZZZ tonality: stay CLOSE TO THE TEXTURE. Keep saturation modest — the global tonemap vibrance
    // already adds ~25%, so a big boost here double-counts and reads neon.
    float sat = (satBoost > 0.01) ? satBoost : 1.15;
    float luma = dot(col, float3(0.2126, 0.7152, 0.0722));
    col = max(lerp(luma.xxx, col, sat), 0.0);

    // Soft highlight roll-off (skip metal, which should keep its bright sheen): compress only the
    // top of the range so low-saturation near-white tones — the faint skin showing through the
    // stockings — sit just under the bloom threshold / ACES white shoulder instead of clipping to
    // pure white. Hue is preserved (all channels scaled by the peak).
    if (matClass != 4) {
        float pk = max(max(col.r, col.g), col.b);
        const float knee = 0.85;
        if (pk > knee)
            col *= (knee + (pk - knee) / (1.0 + (pk - knee) * 4.0)) / pk;
    }

    // ---- Colour grade: warm/orange push + overall deepen ---------------------------------------
    // Applied BEFORE the fidelity pre-inversion so the grade survives the shared post chain and is
    // what actually shows. Warmth lifts R / drops G a little / drops B more → yellow (hair) shifts
    // toward orange, the whole character reads warmer. Deepen darkens with a mild gamma (shadows
    // deepen more than highlights) for a richer, less washed look.
    if (warmth > 0.001) {
        // General warm bias everywhere (subtle): lift R, drop B.
        float3 warmGain = float3(1.0 + 0.07 * warmth, 1.0 - 0.04 * warmth, 1.0 - 0.20 * warmth);
        col *= warmGain;
        // Targeted yellow → orange: pull GREEN down only where the pixel is yellow (high R&G, low
        // B) so the hair turns orange while skin / whites / blue coat stay put.
        float yellowness = saturate(min(col.r, col.g) - col.b);
        col.g -= col.g * yellowness * 0.42 * warmth;
    }
    if (deepen > 0.001) {
        col = pow(max(col, 0.0), 1.0 + 0.40 * deepen) * lerp(1.0, 0.84, deepen);
    }

    // ---- Character-only Highlights / Shadows detail (luminance-masked, multiplicative). Applied
    //      BEFORE the fidelity inversion so it's part of the intended look. shadows>0 lifts dark
    //      detail; highlights<0 recovers bright detail. Only the character, not Sponza. ----
    if (abs(charShadows) + abs(charHighlights) > 1e-4) {
        float l      = sqrt(saturate(dot(col, float3(0.2126, 0.7152, 0.0722))));
        float shMask = 1.0 - smoothstep(0.0, 0.5, l);
        float hiMask = smoothstep(0.5, 1.0, l);
        col *= 1.0 + charShadows    * 0.6 * shMask;
        col *= 1.0 + charHighlights * 0.6 * hiMask;
        col = max(col, 0.0);
    }

    // ---- Texture-colour fidelity ---------------------------------------------------------------
    // The character is composited into the HDR scene and then run through the shared post chain
    // (bloom → *exposure → ACES → gamma → vibrance) tuned for Sponza. That chain brightens and
    // DESATURATES the character away from its painted albedo (the "發白 / whitening"). Pre-invert
    // that chain here so the COMPOSITED character lands back on the colour we shaded — i.e. its
    // texture. We invert only the LDR (albedo) range; HDR glints keep their excess so specular
    // still blooms and gets the ACES shoulder. The clamped compensation caps runaway near-white
    // bloom. texFidelity blends stylised (0) ↔ exact texture (1).
    if (texFidelity > 0.001) {
        float3 intended = col;
        float3 clamped  = saturate(intended);
        float3 excess   = intended - clamped;                     // >0 only on HDR highlights
        float3 tSrgb    = pow(clamped, 1.0 / 2.2);                // the sRGB we want to SEE
        float  lt       = dot(tSrgb, float3(0.2126, 0.7152, 0.0722));
        float3 preVib   = (postVibrance > 0.01) ? saturate(lt + (tSrgb - lt) / postVibrance) : tSrgb;
        float  invExp   = 1.0 / max(postExposure, 0.01);
        float3 comp     = min(ACESInv(pow(preVib, 2.2)) * invExp, 2.0) + excess * invExp;
        col = lerp(col, comp, saturate(texFidelity));
    }
    return float4(col, 1.0);
}
