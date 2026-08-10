// Screen-space ambient occlusion. World-space hemisphere sampling around each pixel's
// normal (reconstructed from the depth buffer), rotated per-pixel by a 4x4 noise tile,
// then a 4x4 box blur. Reuses PerFrame (b0) for the camera matrices.

cbuffer PerFrame : register(b0)
{
    row_major float4x4 viewProj;
    row_major float4x4 invViewProj;
    float3 cameraPos;        uint  viewMode;
    float3 lightDirToLight;  float zNear;
    float3 lightIntensity;   float zFar;
    row_major float4x4 lightViewProj;
    float shadowBias;        float shadowTexel;
    float2 _pad;
};

cbuffer SsaoCB : register(b1)
{
    float  radius;
    float  bias;
    float  intensity;
    uint   enabled;
    float2 screen;       // render-target size in pixels
    float2 _ssaoPad;
};

Texture2D<float>  g_tex0  : register(t0);   // depth (SSAO pass) or occlusion (blur pass)
Texture2D<float4> g_tex1  : register(t1);   // normal (SSAO pass)
Texture2D<float4> g_noise : register(t2);   // 4x4 random rotations

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float3 ReconstructWorld(float2 uv, float zNdc)
{
    float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, zNdc, 1.0);
    float4 w = mul(ndc, invViewProj);
    return w.xyz / w.w;
}

// 16-sample hemisphere kernel (tangent space, +Z = normal), lengths biased toward centre.
static const float3 kKernel[16] =
{
    float3( 0.21,  0.05,  0.10), float3(-0.14,  0.18,  0.12), float3( 0.03, -0.22,  0.15),
    float3(-0.19, -0.10,  0.20), float3( 0.28,  0.16,  0.22), float3(-0.24,  0.22,  0.26),
    float3( 0.11, -0.30,  0.28), float3(-0.05,  0.12,  0.34), float3( 0.36, -0.10,  0.30),
    float3(-0.32, -0.28,  0.34), float3( 0.18,  0.38,  0.36), float3(-0.40,  0.10,  0.42),
    float3( 0.07, -0.20,  0.55), float3( 0.44,  0.30,  0.48), float3(-0.30,  0.40,  0.56),
    float3( 0.10,  0.05,  0.70),
};

float SsaoPS(VSOut i) : SV_TARGET
{
    if (enabled == 0) return 1.0;

    int2 ip = int2(i.pos.xy);
    float z = g_tex0.Load(int3(ip, 0));
    if (z >= 1.0) return 1.0;                       // background

    float3 P = ReconstructWorld(i.uv, z);
    float3 N = normalize(g_tex1.Load(int3(ip, 0)).xyz);
    float3 rnd = normalize(g_noise.Load(int3(ip & 3, 0)).xyz * 2.0 - 1.0);
    float3 T = normalize(rnd - N * dot(rnd, N));
    float3 B = cross(N, T);
    float3x3 TBN = float3x3(T, B, N);

    float occ = 0.0;
    [unroll]
    for (int k = 0; k < 16; ++k)
    {
        float3 sp = P + mul(kKernel[k], TBN) * radius;
        float4 clip = mul(float4(sp, 1.0), viewProj);
        if (clip.w <= 0.0) continue;
        clip.xyz /= clip.w;
        float2 suv = clip.xy * float2(0.5, -0.5) + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;

        int2 sip = int2(suv * screen);
        float sz = g_tex0.Load(int3(sip, 0));
        if (sz >= 1.0) continue;
        float3 Q = ReconstructWorld(suv, sz);

        float dS = distance(cameraPos, sp);
        float dQ = distance(cameraPos, Q);
        float range = smoothstep(0.0, 1.0, radius / (distance(P, Q) + 1e-3));
        if (dQ < dS - bias) occ += range;
    }
    occ = 1.0 - (occ / 16.0) * intensity;
    return saturate(occ);
}

// 4x4 box blur to wipe out the per-pixel noise rotation.
float BlurPS(VSOut i) : SV_TARGET
{
    int2 ip = int2(i.pos.xy);
    float sum = 0.0;
    [unroll]
    for (int y = -2; y <= 1; ++y)
        [unroll]
        for (int x = -2; x <= 1; ++x)
            sum += g_tex0.Load(int3(ip + int2(x, y), 0));
    return sum / 16.0;
}
