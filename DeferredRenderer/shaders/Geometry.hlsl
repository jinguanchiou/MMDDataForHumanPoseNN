// Geometry pass: writes Normal (RGBA16F) + Albedo (RGBA8) MRTs and the depth buffer.
cbuffer PerFrame : register(b0)
{
    row_major float4x4 viewProj;
    row_major float4x4 invViewProj;
    float3 cameraPos;        uint  viewMode;
    float3 lightDirToLight;  float zNear;
    float3 lightIntensity;   float zFar;
};

// Per-object: world transform + material id (0 = scene/Blinn-Phong, 1 = cel-shaded MMD
// character). The material id rides in the G-buffer normal target's alpha channel.
cbuffer PerObject : register(b1)
{
    row_major float4x4 world;
    uint  materialId;
    uint  useFaceMask;   // 1 = multiply diffuse by g_faceMask (baked face shadow / AO)
    uint  useNormalMap;  // 1 = perturb the normal with g_normalMap (tangent space)
    float satBoost;      // albedo saturation multiplier (1 = unchanged)
    row_major float4x4 view; // camera view matrix (for sphere-map view-space normal)
    uint  useSphere;         // 1 = apply MMD sphere maps (mul = sph, add = spa)
    float faceMaskStrength;  // 0..1 face shadow/AO darkening
    float sphereStrength;    // 0..1 sphere-map blend
    float contrast;          // albedo contrast (1 = unchanged)
    float4 xrayReveal;       // x-ray window: (centerPx.xy, radiusPx, featherPx)
    float  xrayStrength;     // reveal opacity inside the window (1 = solid character)
    float3 _xpad;
};

Texture2D    g_diffuse   : register(t0);
Texture2D    g_faceMask  : register(t1);   // MMD face AO/shadow mask (white = lit, dark = shadow)
Texture2D    g_normalMap : register(t2);   // tangent-space normal map
Texture2D    g_sphereAdd : register(t3);   // additive sphere map (spa)
Texture2D    g_sphereMul : register(t4);   // multiply sphere map (sph)
SamplerState g_sampler   : register(s0);

struct VSIn
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
};

struct VSOut
{
    float4 svpos    : SV_POSITION;
    float3 normal   : NORMAL;
    float3 worldPos : TEXCOORD1;
    float2 uv       : TEXCOORD0;
};

// Tangent frame from screen-space derivatives (no precomputed vertex tangents needed).
float3 PerturbNormal(float3 N, float3 wp, float2 uv, float3 mapN)
{
    float3 dp1 = ddx(wp),  dp2 = ddy(wp);
    float2 du1 = ddx(uv),  du2 = ddy(uv);
    float3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    float3 T = dp2perp * du1.x + dp1perp * du2.x;
    float3 B = dp2perp * du1.y + dp1perp * du2.y;
    float invmax = rsqrt(max(dot(T, T), dot(B, B)));
    float3x3 TBN = float3x3(T * invmax, B * invmax, N);
    return normalize(mul(mapN, TBN));
}

float3 BoostSaturation(float3 c, float s)
{
    float l = dot(c, float3(0.2126, 0.7152, 0.0722));
    return max(0.0, lerp(l.xxx, c, s));
}

struct PSOut
{
    float4 normal : SV_TARGET0;
    float4 albedo : SV_TARGET1;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    float4 worldPos = mul(float4(i.pos, 1.0), world);
    o.svpos    = mul(worldPos, viewProj);
    o.normal   = mul(i.normal, (float3x3)world);
    o.worldPos = worldPos.xyz;
    o.uv       = i.uv;
    return o;
}

PSOut PSMain(VSOut i)
{
    float4 tex = g_diffuse.Sample(g_sampler, i.uv);
    // Alpha test: the MMD facial-feature planes (eyebrows / eyelashes / eye-lines in the
    // Eff_Facial texture) and the eye iris sheets sit on transparent backgrounds. Discard
    // transparent texels so only the features show. Low threshold keeps soft iris edges.
    clip(tex.a - 0.3);

    float3 albedo = tex.rgb;
    // Baked face shadow: this model's Face_AOMask darkens the forehead/hairline where the
    // bangs occlude it. Multiply it into the face albedo.
    // OutlineMask alpha clip: the Face diffuse is an atlas; the mask is black on the
    // non-face parts (tongue/brows/ears packed in the image) — discard them so they
    // aren't pasted onto the head/skull geometry.
    if (useFaceMask != 0)
        clip(g_faceMask.Sample(g_sampler, i.uv).r - faceMaskStrength);
    // Per-character contrast about mid-grey, then saturation.
    if (contrast != 1.0)
        albedo = saturate((albedo - 0.5) * contrast + 0.5);
    if (satBoost != 1.0)
        albedo = BoostSaturation(albedo, satBoost);

    float3 N = normalize(i.normal);
    if (useNormalMap != 0)
    {
        float3 mapN = g_normalMap.Sample(g_sampler, i.uv).rgb * 2.0 - 1.0;
        N = PerturbNormal(N, i.worldPos, i.uv, mapN);
    }

    // MMD sphere maps: view-space normal indexes a radial map. sph multiplies (shading),
    // spa adds (a view-following highlight / sheen).
    if (useSphere != 0)
    {
        float3 vn = normalize(mul(N, (float3x3)view));
        float2 suv = vn.xy * float2(0.5, -0.5) + 0.5;
        float3 sphered = albedo * g_sphereMul.Sample(g_sampler, suv).rgb
                                + g_sphereAdd.Sample(g_sampler, suv).rgb;
        albedo = lerp(albedo, sphered, sphereStrength);
    }

    PSOut o;
    // Store material id in normal.a so the lighting pass can branch (cel vs Blinn-Phong).
    o.normal = float4(N, (float)materialId);
    o.albedo = float4(albedo, 1.0);
    return o;
}

// Forward transparent pass for the Eff facial decals (blush / sweat / blue-face / tears).
// Drawn over the already-lit scene with alpha blending so they are semi-transparent (the eyes
// show through) and emissive (true texture colour, unshaded). Uses VSMain as the vertex shader.
// Diffuse sub-pass — alpha-weighted MULTIPLY (darken/tint), blend = DEST_COLOR x src. Output
// lerp(white, colour, alpha): the transparent/white atlas background (alpha 0) leaves the face
// unchanged, while dark/coloured decal texels (青面 dark-green, brows) darken & tint the face by
// their TRUE colour — and the eyes stay visible (multiply darkens, it doesn't replace).
float4 DecalMultiplyPS(VSOut i) : SV_TARGET
{
    float4 tex = g_diffuse.Sample(g_sampler, i.uv);
    return float4(lerp(float3(1, 1, 1), tex.rgb, tex.a), 1.0);
}

// Emission sub-pass — additive glow (the vivid colours on black), blend = ONE/ONE.
float4 DecalPS(VSOut i) : SV_TARGET
{
    return g_diffuse.Sample(g_sampler, i.uv);
}

// X-ray reveal pre-pass (character "see-through buildings" window). Run with DepthFunc=ALWAYS,
// depth-write ON, no colour targets. Inside a screen circle around the character it resets the
// depth to far (SV_Depth=1) so the following character draw wins over an occluder; an ordered
// dither softens the circle edge and (with xrayStrength<1) lets the occluder show through, so
// the object reads as see-through rather than the whole character being pasted on top. Outside
// the circle / dithered-out, the fragment is discarded → normal occlusion is preserved.
float PSXrayReveal(float4 svpos : SV_POSITION) : SV_Depth
{
    float2 c       = xrayReveal.xy;
    float  radius  = xrayReveal.z;
    float  feather = max(xrayReveal.w, 1.0);
    float  d       = distance(svpos.xy, c);
    float  edge    = saturate((radius - d) / feather);   // 1 inside, ramps to 0 at the rim
    float  reveal  = edge * xrayStrength;

    // 4x4 ordered (Bayer) dither for a soft edge + see-through translucency.
    const float bayer[16] = { 0.0, 8.0, 2.0, 10.0,  12.0, 4.0, 14.0, 6.0,
                              3.0, 11.0, 1.0, 9.0,   15.0, 7.0, 13.0, 5.0 };
    int   bx = (int)svpos.x & 3;
    int   by = (int)svpos.y & 3;
    float thr = (bayer[by * 4 + bx] + 0.5) / 16.0;

    if (reveal <= thr) discard;   // keep scene depth here → occluder stays opaque
    return 1.0;                   // reset depth → the character draw shows through here
}
