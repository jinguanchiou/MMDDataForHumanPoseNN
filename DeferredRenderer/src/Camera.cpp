#include "Camera.h"
#include <cmath>

using namespace DirectX;

namespace dr {

void Camera::SetPerspective(float fovY, float aspect, float zNear, float zFar) {
    m_fov = fovY; m_aspect = aspect; m_near = zNear; m_far = zFar;
    m_proj = XMMatrixPerspectiveFovLH(m_fov, m_aspect, m_near, m_far);
}

void Camera::SetAspect(float aspect) {
    m_aspect = aspect;
    m_proj = XMMatrixPerspectiveFovLH(m_fov, m_aspect, m_near, m_far);
}

void Camera::LookAt(XMFLOAT3 eye, XMFLOAT3 target) {
    m_pos = eye;
    XMVECTOR f = XMVector3Normalize(
        XMVectorSubtract(XMLoadFloat3(&target), XMLoadFloat3(&eye)));
    float fx = XMVectorGetX(f), fy = XMVectorGetY(f), fz = XMVectorGetZ(f);
    m_yaw   = std::atan2f(fx, fz);   // forward.x = sin(yaw)cos(pitch), forward.z = cos(yaw)cos(pitch)
    m_pitch = -std::asinf(fy);       // forward.y = -sin(pitch)
}

void Camera::Update(const Input& input, float dt) {
    XMVECTOR forward = XMVectorSet(
        std::sinf(m_yaw) * std::cosf(m_pitch),
        -std::sinf(m_pitch),
        std::cosf(m_yaw) * std::cosf(m_pitch),
        0.0f);
    XMVECTOR right = XMVectorSet(std::cosf(m_yaw), 0.0f, -std::sinf(m_yaw), 0.0f);
    XMVECTOR up    = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    if (input.lookActive) {
        if (input.rmb) {
            const float panScale = m_speed * 0.0025f;
            XMVECTOR pos = XMLoadFloat3(&m_pos);
            pos = XMVectorSubtract(pos, XMVectorScale(right, input.mouseDX * panScale));
            pos = XMVectorAdd     (pos, XMVectorScale(up,    input.mouseDY * panScale));
            XMStoreFloat3(&m_pos, pos);
        } else {
            m_yaw   += input.mouseDX * m_sensitivity;
            m_pitch += input.mouseDY * m_sensitivity;
            const float lim = XM_PIDIV2 - 0.01f;
            if (m_pitch >  lim) m_pitch =  lim;
            if (m_pitch < -lim) m_pitch = -lim;

            forward = XMVectorSet(
                std::sinf(m_yaw) * std::cosf(m_pitch),
                -std::sinf(m_pitch),
                std::cosf(m_yaw) * std::cosf(m_pitch),
                0.0f);
            right = XMVectorSet(std::cosf(m_yaw), 0.0f, -std::sinf(m_yaw), 0.0f);
        }
    }

    const float speed = (input.keys[VK_SHIFT] ? 4.0f : 1.0f) * m_speed * dt;

    XMVECTOR pos = XMLoadFloat3(&m_pos);
    if (input.keys['W']) pos = XMVectorAdd(pos, XMVectorScale(forward, speed));
    if (input.keys['S']) pos = XMVectorSubtract(pos, XMVectorScale(forward, speed));
    if (input.keys['D']) pos = XMVectorAdd(pos, XMVectorScale(right, speed));
    if (input.keys['A']) pos = XMVectorSubtract(pos, XMVectorScale(right, speed));
    if (input.keys['E'] || input.keys[VK_SPACE])   pos = XMVectorAdd(pos, XMVectorScale(up, speed));
    if (input.keys['Q'] || input.keys[VK_CONTROL]) pos = XMVectorSubtract(pos, XMVectorScale(up, speed));
    XMStoreFloat3(&m_pos, pos);
}

XMMATRIX Camera::View() const {
    XMVECTOR forward = XMVectorSet(
        std::sinf(m_yaw) * std::cosf(m_pitch),
        -std::sinf(m_pitch),
        std::cosf(m_yaw) * std::cosf(m_pitch),
        0.0f);
    XMVECTOR pos = XMLoadFloat3(&m_pos);
    XMVECTOR target = XMVectorAdd(pos, forward);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    return XMMatrixLookAtLH(pos, target, up);
}

} // namespace dr
