#include "AssetLoader.h"
#include <d3d12.h>
#include <DirectXMath.h>

#include <directxtk12/DDSTextureLoader.h>
#include <directxtk12/ResourceUploadBatch.h>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <unordered_map>

using namespace DirectX;
namespace fs = std::filesystem;

namespace dr {
namespace {

ComPtr<ID3D12Resource> CreateDefaultBuffer(ID3D12Device* device, UINT64 size) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type                 = D3D12_HEAP_TYPE_DEFAULT;
    hp.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask     = 1;
    hp.VisibleNodeMask      = 1;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment          = 0;
    rd.Width              = size;
    rd.Height             = 1;
    rd.DepthOrArraySize   = 1;
    rd.MipLevels          = 1;
    rd.Format             = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count   = 1;
    rd.SampleDesc.Quality = 0;
    rd.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags              = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> res;
    ThrowIfFailed(device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&res)));
    return res;
}

ComPtr<ID3D12Resource> CreateWhite1x1(ID3D12Device* device, ResourceUploadBatch& upload) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width              = 1;
    td.Height             = 1;
    td.DepthOrArraySize   = 1;
    td.MipLevels          = 1;
    td.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count   = 1;
    td.SampleDesc.Quality = 0;
    td.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags              = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> res;
    ThrowIfFailed(device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&res)));
    NameObject(res.Get(), L"FallbackWhite1x1");

    static const uint8_t kWhite[4] = { 255, 255, 255, 255 };
    D3D12_SUBRESOURCE_DATA sd{};
    sd.pData      = kWhite;
    sd.RowPitch   = 4;
    sd.SlicePitch = 4;

    upload.Upload(res.Get(), 0, &sd, 1);
    upload.Transition(res.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return res;
}

fs::path ResolveTexturePath(const fs::path& baseDir, const std::string& assimpRel) {
    fs::path orig = baseDir / assimpRel;
    fs::path low = orig; low.replace_extension(".dds");
    if (fs::exists(low)) return low;
    fs::path up = orig;  up.replace_extension(".DDS");
    if (fs::exists(up))  return up;
    if (fs::exists(orig)) return orig;
    return {};
}

} // namespace

bool LoadSceneFromObj(
    ID3D12Device*       device,
    ID3D12CommandQueue* queue,
    const std::wstring& objPath,
    Scene&              outScene)
{
    Assimp::Importer importer;
    const std::string objPathUtf8 = Narrow(objPath);

    const unsigned flags =
          aiProcess_Triangulate
        | aiProcess_GenSmoothNormals
        | aiProcess_ConvertToLeftHanded
        | aiProcess_PreTransformVertices
        | aiProcess_JoinIdenticalVertices
        | aiProcess_ImproveCacheLocality;

    const aiScene* scene = importer.ReadFile(objPathUtf8, flags);
    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        std::fprintf(stderr, "[AssetLoader] Assimp error: %s\n", importer.GetErrorString());
        return false;
    }

    std::vector<Vertex> vertices;
    std::vector<UINT>   indices;
    std::vector<Submesh> submeshes;
    std::vector<UINT>    submeshMatIdx;
    submeshes.reserve(scene->mNumMeshes);
    submeshMatIdx.reserve(scene->mNumMeshes);

    XMFLOAT3 bmin{  FLT_MAX,  FLT_MAX,  FLT_MAX };
    XMFLOAT3 bmax{ -FLT_MAX, -FLT_MAX, -FLT_MAX };

    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        if (!mesh || mesh->mNumVertices == 0 || mesh->mNumFaces == 0) continue;
        if (!mesh->HasPositions() || !mesh->HasNormals())             continue;

        Submesh sm;
        sm.indexStart = static_cast<UINT>(indices.size());
        const UINT baseVertex = static_cast<UINT>(vertices.size());

        vertices.reserve(vertices.size() + mesh->mNumVertices);
        for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
            const aiVector3D& p = mesh->mVertices[v];
            const aiVector3D& n = mesh->mNormals[v];
            aiVector3D uv{ 0.0f, 0.0f, 0.0f };
            if (mesh->HasTextureCoords(0)) uv = mesh->mTextureCoords[0][v];

            Vertex vert{};
            vert.position = { p.x, p.y, p.z };
            vert.normal   = { n.x, n.y, n.z };
            vert.uv       = { uv.x, uv.y };
            vertices.push_back(vert);

            bmin.x = std::min(bmin.x, p.x); bmin.y = std::min(bmin.y, p.y); bmin.z = std::min(bmin.z, p.z);
            bmax.x = std::max(bmax.x, p.x); bmax.y = std::max(bmax.y, p.y); bmax.z = std::max(bmax.z, p.z);
        }

        indices.reserve(indices.size() + (size_t)mesh->mNumFaces * 3);
        for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            if (face.mNumIndices != 3) continue;
            indices.push_back(baseVertex + face.mIndices[0]);
            indices.push_back(baseVertex + face.mIndices[1]);
            indices.push_back(baseVertex + face.mIndices[2]);
        }
        sm.indexCount = static_cast<UINT>(indices.size()) - sm.indexStart;
        if (sm.indexCount == 0) continue;

        submeshes.push_back(sm);
        submeshMatIdx.push_back(mesh->mMaterialIndex);
    }

    if (vertices.empty() || indices.empty() || submeshes.empty()) {
        std::fprintf(stderr, "[AssetLoader] No usable geometry\n");
        return false;
    }

    ResourceUploadBatch upload(device);
    upload.Begin();

    // Vertex buffer
    const UINT64 vbSize = static_cast<UINT64>(vertices.size()) * sizeof(Vertex);
    outScene.vertexBuffer = CreateDefaultBuffer(device, vbSize);
    NameObject(outScene.vertexBuffer.Get(), L"Sponza VB");
    {
        D3D12_SUBRESOURCE_DATA sd{};
        sd.pData      = vertices.data();
        sd.RowPitch   = static_cast<LONG_PTR>(vbSize);
        sd.SlicePitch = sd.RowPitch;
        upload.Upload(outScene.vertexBuffer.Get(), 0, &sd, 1);
        upload.Transition(outScene.vertexBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    }
    outScene.vbv.BufferLocation = outScene.vertexBuffer->GetGPUVirtualAddress();
    outScene.vbv.SizeInBytes    = static_cast<UINT>(vbSize);
    outScene.vbv.StrideInBytes  = sizeof(Vertex);

    // Index buffer
    const UINT64 ibSize = static_cast<UINT64>(indices.size()) * sizeof(UINT);
    outScene.indexBuffer = CreateDefaultBuffer(device, ibSize);
    NameObject(outScene.indexBuffer.Get(), L"Sponza IB");
    {
        D3D12_SUBRESOURCE_DATA sd{};
        sd.pData      = indices.data();
        sd.RowPitch   = static_cast<LONG_PTR>(ibSize);
        sd.SlicePitch = sd.RowPitch;
        upload.Upload(outScene.indexBuffer.Get(), 0, &sd, 1);
        upload.Transition(outScene.indexBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_INDEX_BUFFER);
    }
    outScene.ibv.BufferLocation = outScene.indexBuffer->GetGPUVirtualAddress();
    outScene.ibv.SizeInBytes    = static_cast<UINT>(ibSize);
    outScene.ibv.Format         = DXGI_FORMAT_R32_UINT;

    // Materials → textures
    fs::path baseDir = fs::path(objPath).parent_path();

    std::vector<ComPtr<ID3D12Resource>> textures;
    textures.reserve(scene->mNumMaterials + 1);
    const UINT whiteIndex = static_cast<UINT>(textures.size());
    textures.push_back(CreateWhite1x1(device, upload));

    std::vector<UINT> matSrvIdx(scene->mNumMaterials, whiteIndex);
    std::unordered_map<std::string, UINT> texCache;

    for (unsigned mi = 0; mi < scene->mNumMaterials; ++mi) {
        const aiMaterial* mat = scene->mMaterials[mi];
        aiString rel;
        if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &rel) != AI_SUCCESS) continue;
        const std::string relStr = rel.C_Str();
        if (relStr.empty()) continue;

        if (auto it = texCache.find(relStr); it != texCache.end()) {
            matSrvIdx[mi] = it->second;
            continue;
        }

        fs::path resolved = ResolveTexturePath(baseDir, relStr);
        if (resolved.empty()) {
            std::fprintf(stderr, "[AssetLoader] Texture not found: %s\n", relStr.c_str());
            continue;
        }

        ComPtr<ID3D12Resource> tex;
        HRESULT hr = CreateDDSTextureFromFile(device, upload, resolved.c_str(), &tex);
        if (FAILED(hr)) {
            std::fprintf(stderr, "[AssetLoader] DDS load failed for %s (HR=0x%08lX)\n",
                         resolved.string().c_str(), static_cast<unsigned long>(hr));
            continue;
        }
        const UINT idx = static_cast<UINT>(textures.size());
        textures.push_back(tex);
        texCache.emplace(relStr, idx);
        matSrvIdx[mi] = idx;
    }

    auto finish = upload.End(queue);
    finish.wait();

    // SRV heap
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = static_cast<UINT>(textures.size());
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&outScene.srvHeap)));
    NameObject(outScene.srvHeap.Get(), L"Scene SRV Heap");

    const UINT srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    outScene.srvDescriptorSize = srvSize;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = outScene.srvHeap->GetCPUDescriptorHandleForHeapStart();
    for (auto& tex : textures) {
        const D3D12_RESOURCE_DESC rd = tex->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format                    = rd.Format;
        sd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip = 0;
        sd.Texture2D.MipLevels       = rd.MipLevels;
        sd.Texture2D.PlaneSlice      = 0;
        sd.Texture2D.ResourceMinLODClamp = 0.0f;
        device->CreateShaderResourceView(tex.Get(), &sd, handle);
        handle.ptr += srvSize;
    }

    for (size_t i = 0; i < submeshes.size(); ++i) {
        const UINT mIdx = submeshMatIdx[i];
        submeshes[i].srvHeapIndex = (mIdx < matSrvIdx.size()) ? matSrvIdx[mIdx] : whiteIndex;
    }

    outScene.submeshes   = std::move(submeshes);
    outScene.textures    = std::move(textures);
    outScene.boundsMin   = bmin;
    outScene.boundsMax   = bmax;
    outScene.vertexCount = static_cast<UINT>(vertices.size());
    outScene.indexCount  = static_cast<UINT>(indices.size());

    std::printf(
        "[AssetLoader] %u verts, %u indices, %zu submeshes, %zu textures, "
        "bounds (%.1f,%.1f,%.1f)..(%.1f,%.1f,%.1f)\n",
        outScene.vertexCount, outScene.indexCount,
        outScene.submeshes.size(), outScene.textures.size(),
        bmin.x, bmin.y, bmin.z, bmax.x, bmax.y, bmax.z);
    return true;
}

} // namespace dr
