// Shadow map pass: render scene + character depth from the directional light's point of
// view. Depth-only (no pixel shader output needed). Reuses the PerFrame (b0) light matrix
// and the PerObject (b1) world transform.

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

cbuffer PerObject : register(b1)
{
    row_major float4x4 world;
    uint  materialId;
    float3 _objPad;
};

float4 VSMain(float3 pos : POSITION) : SV_POSITION
{
    float4 worldPos = mul(float4(pos, 1.0), world);
    return mul(worldPos, lightViewProj);
}
