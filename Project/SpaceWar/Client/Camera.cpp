#include "Camera.h"
#include <cmath>

using namespace DirectX;

// ── 카메라 감각 튜닝 ─────────────────────────────────────────
namespace {
	constexpr float kDistance    = 6.0f;    // 평상시 거리
	constexpr float kAimDistance = 3.2f;    // 조준 시 (어깨 너머로 당김)
	constexpr float kSprintDistance = 6.8f; // 질주 시 살짝 뒤로

	constexpr float kShoulder    = 1.5f;    // 오른쪽 어깨 오프셋
	constexpr float kAimShoulder = 1.0f;

	constexpr float kFocusHeight = 1.8f;    // 타겟 기준 주시점 높이
	constexpr float kMinEyeHeight = 0.4f;   // 지면 아래로 안 내려감

	constexpr float kFov       = 1.05f;     // 약 60도
	constexpr float kAimFov    = 0.62f;     // 약 35도
	constexpr float kSprintFov = 1.17f;     // 질주 시 시야 확대

	constexpr float kPitchMin = -0.55f;     // 위로 약 31도
	constexpr float kPitchMax =  1.15f;     // 아래로 약 66도

	constexpr float kFollowLag = 14.0f;     // 위치 추종 (클수록 딱 붙음)
	constexpr float kZoomLag   = 12.0f;     // 조준/질주 전환 속도

	// 프레임률에 무관한 지수 감쇠
	float Damp(float current, float target, float rate, float dt)
	{
		return target + (current - target) * expf(-rate * dt);
	}
}

namespace swc {

	void Camera::AddLook(float yawDelta, float pitchDelta)
	{
		yaw += yawDelta;
		pitch += pitchDelta;

		if (pitch < kPitchMin) pitch = kPitchMin;
		if (pitch > kPitchMax) pitch = kPitchMax;

		if (yaw > XM_PI) yaw -= XM_2PI;
		else if (yaw < -XM_PI) yaw += XM_2PI;
	}

	XMFLOAT3 Camera::ForwardXZ() const
	{
		return { sinf(yaw), 0.0f, cosf(yaw) };
	}

	XMFLOAT3 Camera::RightXZ() const
	{
		return { cosf(yaw), 0.0f, -sinf(yaw) };
	}

	void Camera::SnapTo(const XMFLOAT3& target)
	{
		smoothTarget = target;
		distance = kDistance;
		shoulder = kShoulder;
		fov = kFov;
		Update(0.0f, target);
	}

	void Camera::Update(float dt, const XMFLOAT3& target)
	{
		smoothTarget.x = Damp(smoothTarget.x, target.x, kFollowLag, dt);
		smoothTarget.y = Damp(smoothTarget.y, target.y, kFollowLag, dt);
		smoothTarget.z = Damp(smoothTarget.z, target.z, kFollowLag, dt);

		float wantDistance = kDistance;
		float wantFov = kFov;
		if (aiming)         { wantDistance = kAimDistance;    wantFov = kAimFov; }
		else if (sprinting) { wantDistance = kSprintDistance; wantFov = kSprintFov; }

		distance = Damp(distance, wantDistance, kZoomLag, dt);
		shoulder = Damp(shoulder, aiming ? kAimShoulder : kShoulder, kZoomLag, dt);
		fov = Damp(fov, wantFov, kZoomLag, dt);

		const XMFLOAT3 right = RightXZ();
		const XMFLOAT3 focus{
			smoothTarget.x + right.x * shoulder,
			smoothTarget.y + kFocusHeight,
			smoothTarget.z + right.z * shoulder };

		// eye -> focus 방향. pitch 가 클수록 카메라가 위에서 내려다본다.
		const float cp = cosf(pitch);
		const XMFLOAT3 dir{ sinf(yaw) * cp, -sinf(pitch), cosf(yaw) * cp };

		XMFLOAT3 eye{
			focus.x - dir.x * distance,
			focus.y - dir.y * distance,
			focus.z - dir.z * distance };
		if (eye.y < kMinEyeHeight) eye.y = kMinEyeHeight;
		eyePosition = eye;

		const XMMATRIX view = XMMatrixLookAtLH(
			XMLoadFloat3(&eye), XMLoadFloat3(&focus), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
		const XMMATRIX proj = XMMatrixPerspectiveFovLH(fov, aspect, 0.1f, 1000.0f);
		XMStoreFloat4x4(&viewProj, view * proj);
	}
}
