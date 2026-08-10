// Bloom spheres: a small emissive sphere drawn at EACH point light's position (instanced
// from the light buffer) into the HDR scene buffer, after lighting and before bloom, so the
// bloom pass makes them glow — a visual marker of where every point light is. Depth-tested
// against the scene so walls occlude them.

cbuffer SphereCB : register(b0)
{
    row_major float4x4 viewProj;
    float radius;          // marker sphere radius (world units)
    float emissiveScale;   // light.color * this = HDR emissive (>1 so it blooms)
    float2 _pad;
};

struct PointLight { float3 posWS; float radius; float3 color; float intensity; };
StructuredBuffer<PointLight> g_lights : register(t0);

struct VSOut { float4 pos : SV_POSITION; float3 emissive : COLOR0; };

VSOut VSMain(float3 pos : POSITION, uint iid : SV_InstanceID)
{
    PointLight L = g_lights[iid];
    float3 wp = L.posWS + pos * radius;     // unit sphere → marker at this light
    VSOut o;
    o.pos      = mul(float4(wp, 1.0), viewProj);
    o.emissive = L.color * emissiveScale;
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET { return float4(i.emissive, 1.0); }
