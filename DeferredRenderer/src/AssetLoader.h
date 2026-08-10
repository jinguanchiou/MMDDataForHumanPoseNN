#pragma once
#include "Common.h"
#include "Scene.h"
#include <string>

struct ID3D12Device;
struct ID3D12CommandQueue;

namespace dr {

bool LoadSceneFromObj(
    ID3D12Device*       device,
    ID3D12CommandQueue* queue,
    const std::wstring& objPath,
    Scene&              outScene);

} // namespace dr
