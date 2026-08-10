// Forward+ tiled light culling (compute). One thread group per 16x16 screen tile builds
// the tile's view frustum (4 side planes through the camera) and tests every point light's
// bounding sphere against it, writing a per-tile list of affected light indices. The
// deferred Lighting pass then only iterates that short list per pixel instead of all lights.
//
// Tile-list layout: g_tileLights is one flat uint array; tile t owns the slot range
// [t*(MAX_LIGHTS_PER_TILE+1) .. ]; slot 0 is the (clamped) light count, then the indices.

#define TILE_SIZE           16
#define MAX_LIGHTS_PER_TILE 64

struct PointLight
{
    float3 posWS;
    float  radius;
    float3 color;
    float  intensity;
};

cbuffer CullCB : register(b0)
{
    row_major float4x4 invViewProj;
    float3 cameraPos;     uint  numLights;
    uint2  screenSize;    uint2 tileCount;
};

StructuredBuffer<PointLight> g_lights     : register(t0);
Texture2D<float>             g_depth      : register(t1);
RWStructuredBuffer<uint>     g_tileLights : register(u0);

groupshared uint gCount;
groupshared uint gMinZ;   // tile depth bounds (NDC z as uint bits, for InterlockedMin/Max)
groupshared uint gMaxZ;
groupshared uint gList[MAX_LIGHTS_PER_TILE];

float3 WorldFromNDC(float2 ndcXY, float ndcZ)
{
    float4 p = mul(float4(ndcXY, ndcZ, 1.0), invViewProj);
    return p.xyz / p.w;
}

// Inward-facing plane (normal, d) through points a,b,c, flipped to face `inside`.
void MakePlane(float3 a, float3 b, float3 c, float3 inside, out float3 n, out float d)
{
    n = normalize(cross(b - a, c - a));
    d = -dot(n, a);
    if (dot(n, inside) + d < 0.0) { n = -n; d = -d; }
}

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void CSMain(uint3 groupId : SV_GroupID, uint3 dtid : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    if (groupIndex == 0) { gCount = 0; gMinZ = 0x7F7FFFFF; gMaxZ = 0; }
    GroupMemoryBarrierWithGroupSync();

    // Per-tile depth bounds: each thread folds in its pixel's depth (skip the far plane /
    // background). Tighter bounds → far fewer lights survive than a full-range tile frustum.
    if (dtid.x < screenSize.x && dtid.y < screenSize.y)
    {
        float z = g_depth.Load(int3(dtid.xy, 0));
        if (z < 1.0)
        {
            InterlockedMin(gMinZ, asuint(z));
            InterlockedMax(gMaxZ, asuint(z));
        }
    }
    GroupMemoryBarrierWithGroupSync();

    uint tileIdx = groupId.y * tileCount.x + groupId.x;
    uint base    = tileIdx * (MAX_LIGHTS_PER_TILE + 1);

    // Empty tile (all background): no geometry to light here.
    if (gMaxZ == 0)
    {
        if (groupIndex == 0) g_tileLights[base] = 0;
        return;
    }
    float minZ = asfloat(gMinZ);
    float maxZ = asfloat(gMaxZ);

    // Tile screen rect -> NDC rect (y flipped: screen down = NDC up).
    float2 t0px = float2(groupId.xy) * TILE_SIZE;
    float2 t1px = min(t0px + TILE_SIZE, float2(screenSize));
    float2 ndcMin = float2(t0px.x / screenSize.x * 2.0 - 1.0, 1.0 - t1px.y / screenSize.y * 2.0);
    float2 ndcMax = float2(t1px.x / screenSize.x * 2.0 - 1.0, 1.0 - t0px.y / screenSize.y * 2.0);

    // Eight frustum corners: 4 at the near (minZ) plane, 4 at the far (maxZ) plane.
    float3 ntl = WorldFromNDC(float2(ndcMin.x, ndcMax.y), minZ);
    float3 ntr = WorldFromNDC(float2(ndcMax.x, ndcMax.y), minZ);
    float3 nbl = WorldFromNDC(float2(ndcMin.x, ndcMin.y), minZ);
    float3 nbr = WorldFromNDC(float2(ndcMax.x, ndcMin.y), minZ);
    float3 ftl = WorldFromNDC(float2(ndcMin.x, ndcMax.y), maxZ);
    float3 ftr = WorldFromNDC(float2(ndcMax.x, ndcMax.y), maxZ);
    float3 fbl = WorldFromNDC(float2(ndcMin.x, ndcMin.y), maxZ);
    float3 fbr = WorldFromNDC(float2(ndcMax.x, ndcMin.y), maxZ);
    float3 center = (ntl + ntr + nbl + nbr + ftl + ftr + fbl + fbr) * 0.125;

    float3 nrm[6]; float dst[6];
    MakePlane(nbl, nbr, ntr, center, nrm[0], dst[0]); // near
    MakePlane(fbl, ftr, fbr, center, nrm[1], dst[1]); // far
    MakePlane(nbl, ntl, fbl, center, nrm[2], dst[2]); // left
    MakePlane(nbr, fbr, ntr, center, nrm[3], dst[3]); // right
    MakePlane(ntl, ntr, ftl, center, nrm[4], dst[4]); // top
    MakePlane(nbl, fbl, nbr, center, nrm[5], dst[5]); // bottom

    // Cooperative cull: each thread tests a strided subset of the lights.
    for (uint li = groupIndex; li < numLights; li += TILE_SIZE * TILE_SIZE)
    {
        PointLight L = g_lights[li];
        bool inside = true;
        [unroll]
        for (int p = 0; p < 6; ++p)
            if (dot(nrm[p], L.posWS) + dst[p] < -L.radius) { inside = false; break; }
        if (inside)
        {
            uint slot;
            InterlockedAdd(gCount, 1, slot);
            if (slot < MAX_LIGHTS_PER_TILE) gList[slot] = li;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Write the tile's list (count clamped so the overflow beyond the cap is dropped, not corrupt).
    uint n = min(gCount, (uint)MAX_LIGHTS_PER_TILE);
    if (groupIndex == 0) g_tileLights[base] = n;
    for (uint k = groupIndex; k < n; k += TILE_SIZE * TILE_SIZE)
        g_tileLights[base + 1 + k] = gList[k];
}
