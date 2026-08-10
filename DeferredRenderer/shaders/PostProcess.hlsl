// Post-processing chain on the HDR scene color: bright-pass + separable Gaussian blur
// (bloom), then tone mapping (ACES) with exposure and gamma. One full-screen-triangle VS
// feeds three pixel shaders selected by PSO.

cbuffer Post : register(b0)
{
    float exposure;
    float bloomStrength;
    float threshold;
    uint  vertical;     // blur direction (1 = vertical)
    float invTexelX;    // 1 / source width
    float invTexelY;    // 1 / source height
    uint  viewMode;     // 3 = Color (tone-mapped); others pass through unchanged
    float vibrance;     // whole-scene saturation (1 = unchanged)
    uint  isolated;     // 1 = write the HDR coverage alpha through (transparent cut-out PNG)
    float fxaaSubpix;   // (_pp0) FXAA sub-pixel aliasing strength 0..1 (GUI "AA" slider)
    float _pp1, _pp2;
};

Texture2D    g_texA : register(t0);   // primary input (HDR / blur source)
Texture2D    g_texB : register(t1);   // bloom input (tonemap only)
SamplerState g_samp : register(s0);   // linear clamp

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

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Bright pass: keep only the energy above the threshold, preserving hue.
float4 BrightPS(VSOut i) : SV_TARGET
{
    float3 c = g_texA.Sample(g_samp, i.uv).rgb;
    float  l = Luma(c);
    float  keep = max(0.0, l - threshold);
    float3 b = (l > 1e-4) ? c * (keep / l) : float3(0, 0, 0);
    return float4(b, 1.0);
}

// Separable 9-tap Gaussian.
float4 BlurPS(VSOut i) : SV_TARGET
{
    float2 dir = (vertical != 0) ? float2(0.0, invTexelY) : float2(invTexelX, 0.0);
    const float w[5] = { 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216 };
    float3 r = g_texA.Sample(g_samp, i.uv).rgb * w[0];
    [unroll]
    for (int k = 1; k < 5; ++k)
    {
        r += g_texA.Sample(g_samp, i.uv + dir * k).rgb * w[k];
        r += g_texA.Sample(g_samp, i.uv - dir * k).rgb * w[k];
    }
    return float4(r, 1.0);
}

// Narkowicz ACES filmic approximation.
float3 ACES(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 TonemapPS(VSOut i) : SV_TARGET
{
    float4 hdrS = g_texA.Sample(g_samp, i.uv);
    float3 hdr  = hdrS.rgb;
    float  outA = (isolated != 0) ? hdrS.a : 1.0;   // coverage cut-out under isolation
    if (viewMode != 3)              // debug / line-art views: show as-is
        return float4(hdr, outA);

    float3 bloom = g_texB.Sample(g_samp, i.uv).rgb;
    float3 col = (hdr + bloomStrength * bloom) * exposure;
    col = ACES(col);
    col = pow(col, 1.0 / 2.2);      // linear -> sRGB-ish

    // Global vibrance: push saturation for a punchier final image (GUI-controlled).
    float luma = dot(col, float3(0.2126, 0.7152, 0.0722));
    col = saturate(lerp(luma.xxx, col, vibrance));
    return float4(col, outA);
}

// FXAA 3.11 QUALITY (PC preset) — full edge-endpoint search + sub-pixel aliasing. Far stronger on
// the diagonal silhouette/outline jaggies than the console 4-tap version. Runs on the tonemapped
// (gamma) LDR image. invTexelX/Y = 1/width, 1/height. fxaaSubpix (0..1) = GUI-controlled strength.
float FxaaLuma(float3 c) { return sqrt(dot(c, float3(0.299, 0.587, 0.114))); }  // perceptual (gamma) luma

#define FXAA_EDGE_THRESHOLD     0.125     // local contrast needed to run the edge path
#define FXAA_EDGE_THRESHOLD_MIN 0.0312    // absolute floor (kills work in near-flat regions)
#define FXAA_SEARCH_STEPS       12        // edge-end search iterations each side

static const float kFxaaQuality[12] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };

float4 FxaaPS(VSOut i) : SV_TARGET
{
    float2 px = float2(invTexelX, invTexelY);
    float2 uv = i.uv;

    float4 sM  = g_texA.Sample(g_samp, uv);
    float  outA = sM.a;                                 // preserve coverage alpha (dataset cut-out)
    float  lM = FxaaLuma(sM.rgb);
    float  lN = FxaaLuma(g_texA.Sample(g_samp, uv + float2( 0, -1) * px).rgb);
    float  lS = FxaaLuma(g_texA.Sample(g_samp, uv + float2( 0,  1) * px).rgb);
    float  lE = FxaaLuma(g_texA.Sample(g_samp, uv + float2( 1,  0) * px).rgb);
    float  lW = FxaaLuma(g_texA.Sample(g_samp, uv + float2(-1,  0) * px).rgb);

    float rangeMin = min(lM, min(min(lN, lS), min(lE, lW)));
    float rangeMax = max(lM, max(max(lN, lS), max(lE, lW)));
    float range    = rangeMax - rangeMin;
    if (range < max(FXAA_EDGE_THRESHOLD_MIN, rangeMax * FXAA_EDGE_THRESHOLD))
        return float4(sM.rgb, outA);                    // near-flat: keep the pixel

    float lNW = FxaaLuma(g_texA.Sample(g_samp, uv + float2(-1, -1) * px).rgb);
    float lNE = FxaaLuma(g_texA.Sample(g_samp, uv + float2( 1, -1) * px).rgb);
    float lSW = FxaaLuma(g_texA.Sample(g_samp, uv + float2(-1,  1) * px).rgb);
    float lSE = FxaaLuma(g_texA.Sample(g_samp, uv + float2( 1,  1) * px).rgb);

    // Horizontal vs vertical edge (weighted second derivative across the 3x3).
    float edgeHorz = abs(-2.0 * lN + lNW + lNE) + abs(-2.0 * lM + lW + lE) * 2.0 + abs(-2.0 * lS + lSW + lSE);
    float edgeVert = abs(-2.0 * lW + lNW + lSW) + abs(-2.0 * lM + lN + lS) * 2.0 + abs(-2.0 * lE + lNE + lSE);
    bool  horzSpan = edgeHorz >= edgeVert;

    float luma1 = horzSpan ? lN : lW;
    float luma2 = horzSpan ? lS : lE;
    float grad1 = luma1 - lM;
    float grad2 = luma2 - lM;
    bool  is1Steepest = abs(grad1) >= abs(grad2);
    float gradScaled  = 0.25 * max(abs(grad1), abs(grad2));

    float stepLen = horzSpan ? px.y : px.x;
    float lumaLocalAvg;
    if (is1Steepest) { stepLen = -stepLen; lumaLocalAvg = 0.5 * (luma1 + lM); }
    else             {                     lumaLocalAvg = 0.5 * (luma2 + lM); }

    float2 curUv = uv;
    if (horzSpan) curUv.y += stepLen * 0.5;
    else          curUv.x += stepLen * 0.5;

    float2 off = horzSpan ? float2(px.x, 0.0) : float2(0.0, px.y);
    float2 uv1 = curUv - off;
    float2 uv2 = curUv + off;

    float lumaEnd1 = FxaaLuma(g_texA.Sample(g_samp, uv1).rgb) - lumaLocalAvg;
    float lumaEnd2 = FxaaLuma(g_texA.Sample(g_samp, uv2).rgb) - lumaLocalAvg;
    bool  reached1 = abs(lumaEnd1) >= gradScaled;
    bool  reached2 = abs(lumaEnd2) >= gradScaled;
    bool  reachedBoth = reached1 && reached2;
    if (!reached1) uv1 -= off;
    if (!reached2) uv2 += off;

    [unroll]
    for (int q = 2; q < FXAA_SEARCH_STEPS; ++q)
    {
        if (reachedBoth) break;
        if (!reached1) lumaEnd1 = FxaaLuma(g_texA.Sample(g_samp, uv1).rgb) - lumaLocalAvg;
        if (!reached2) lumaEnd2 = FxaaLuma(g_texA.Sample(g_samp, uv2).rgb) - lumaLocalAvg;
        reached1 = abs(lumaEnd1) >= gradScaled;
        reached2 = abs(lumaEnd2) >= gradScaled;
        reachedBoth = reached1 && reached2;
        if (!reached1) uv1 -= off * kFxaaQuality[q];
        if (!reached2) uv2 += off * kFxaaQuality[q];
    }

    float dist1 = horzSpan ? (uv.x - uv1.x) : (uv.y - uv1.y);
    float dist2 = horzSpan ? (uv2.x - uv.x) : (uv2.y - uv.y);
    bool  isDir1 = dist1 < dist2;
    float distFinal = min(dist1, dist2);
    float edgeLen   = dist1 + dist2;
    float pixelOffset = -distFinal / edgeLen + 0.5;

    bool isCenterSmaller = lM < lumaLocalAvg;
    bool correctVariation = ((isDir1 ? lumaEnd1 : lumaEnd2) < 0.0) != isCenterSmaller;
    float finalOffset = correctVariation ? pixelOffset : 0.0;

    // Sub-pixel aliasing (thin features / near-flat gradients), scaled by the GUI strength.
    float lumaAvg = (1.0 / 12.0) * (2.0 * (lN + lS + lE + lW) + lNW + lNE + lSW + lSE);
    float subA = saturate(abs(lumaAvg - lM) / range);
    float subB = (-2.0 * subA + 3.0) * subA * subA;
    float subOffset = subB * subB * saturate(fxaaSubpix);
    finalOffset = max(finalOffset, subOffset);

    float2 finalUv = uv;
    if (horzSpan) finalUv.y += finalOffset * stepLen;
    else          finalUv.x += finalOffset * stepLen;

    return float4(g_texA.Sample(g_samp, finalUv).rgb, outA);
}
