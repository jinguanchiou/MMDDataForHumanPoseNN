#pragma once
#include "Common.h"
#include "Input.h"
#include <DirectXMath.h>

namespace dr {

class Camera {
public:
    void SetPerspective(float fovYRadians, float aspect, float zNear, float zFar);
    void SetAspect(float aspect);

    void SetPosition(DirectX::XMFLOAT3 p) { m_pos = p; }
    void SetYawPitch(float yaw, float pitch) { m_yaw = yaw; m_pitch = pitch; }
    void SetSpeed(float s) { m_speed = s; }

    // Place the camera at `eye` and orient it to look at `target` (matches this camera's
    // yaw/pitch forward convention), so free-fly resumes correctly afterward.
    void LookAt(DirectX::XMFLOAT3 eye, DirectX::XMFLOAT3 target);

    float Fov()  const { return m_fov; }   // vertical FOV, radians
    float Near() const { return m_near; }
    float Far()  const { return m_far; }
    float Aspect() const { return m_aspect; }

    void Update(const Input& input, float dt);

    DirectX::XMMATRIX View() const;
    DirectX::XMMATRIX Proj() const { return m_proj; }
    DirectX::XMMATRIX ViewProj() const { return DirectX::XMMatrixMultiply(View(), m_proj); }
    DirectX::XMFLOAT3 Position() const { return m_pos; }
    float Yaw()   const { return m_yaw; }
    float Pitch() const { return m_pitch; }

private:
    DirectX::XMFLOAT3 m_pos { 0.0f, 400.0f, 0.0f };
    float             m_yaw   = 0.0f;
    float             m_pitch = 0.0f;
    DirectX::XMMATRIX m_proj  = DirectX::XMMatrixIdentity();

    float m_fov         = DirectX::XM_PIDIV4;
    float m_aspect      = 1.0f;
    float m_near        = 1.0f;
    float m_far         = 10000.0f;
    float m_speed       = 800.0f;
    float m_sensitivity = 0.005f;
};

} // namespace dr
