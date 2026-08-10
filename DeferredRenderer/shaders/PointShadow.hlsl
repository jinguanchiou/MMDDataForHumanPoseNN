// Point-light omnidirectional shadow: render the scene 6x (once per cube face) storing the
// normalised distance from the light to each fragment into an R32_FLOAT cube face. The
// lighting pass later samples the cube by the light->surface direction and compares.

cbuffer PointShadowCB : register(b0)
{
    row_major float4x4 faceViewProj; // this cube face's view-projection
    float3 lightPos;
    float  lightRange;               // far distance (normalises the stored value to 0..1)
};

cbuffer PerObject : register(b1)
{
    row_major float4x4 world;
    uint  materialId;
    float3 _objPad;
};

struct VSOut { float4 pos : SV_POSITION; float3 worldPos : TEXCOORD0; };

VSOut VSMain(float3 pos : POSITION)
{
    VSOut o;
    float4 wp  = mul(float4(pos, 1.0), world);
    o.worldPos = wp.xyz;
    o.pos      = mul(wp, faceViewProj);
    return o;
}

float PSMain(VSOut i) : SV_TARGET
{
    return saturate(length(i.worldPos - lightPos) / lightRange);
}
