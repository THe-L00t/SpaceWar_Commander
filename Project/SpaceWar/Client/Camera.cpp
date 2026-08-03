#include "Camera.h"
#include "Shared/Units.h"
#include <cmath>

using namespace DirectX;
using namespace Shared::units;

// ── 카메라 감각 튜닝 ─────────────────────────────────────────
namespace {
	constexpr float kDistance       = 6.0_m;    // 평상시 거리
	constexpr float kAimDistance    = 3.2_m;    // 조준 시 (어깨 너머로 당김)
	constexpr float kSprintDistance = 6.8_m;    // 질주 시 살짝 뒤로

	constexpr float kShoulder    = 1.5_m;       // 오른쪽 어깨 오프셋
	constexpr float kAimShoulder = 1.0_m;

	constexpr float kFocusHeight = 1.8_m;       // 타겟 기준 주시점 높이
	constexpr float kMinEyeAlt   = 0.4_m;       // 지표면 위 최소 고도

	constexpr float kFov       = 60.0_deg;
	constexpr float kAimFov    = 35.0_deg;
	constexpr float kSprintFov = 67.0_deg;

	constexpr float kPitchMin = -31.0_deg;      // 위로
	constexpr float kPitchMax = 66.0_deg;       // 아래로

	constexpr float kFollowLag = 14.0f;         // 위치 추종 (클수록 딱 붙음)
	constexpr float kZoomLag   = 12.0f;         // 조준/질주 전환 속도

	// 프레임률에 무관한 지수 감쇠
	float Damp(float current, float target, float rate, float dt)
	{
		return target + (current - target) * std::exp(-rate * dt);
	}

	double DampD(double current, double target, double rate, double dt)
	{
		return target + (current - target) * std::exp(-rate * dt);
	}
}

namespace swc {

	void Camera::AddLook(float yawDelta, float pitchDelta)
	{
		// yaw 는 로컬 up 축 기준 회전이다 (월드 Y축이 아니다)
		if (std::fabs(yawDelta) > 0.0f)
			forward = Normalize(RotateAround(forward, up, double(yawDelta)));

		pitch += pitchDelta;
		if (pitch < kPitchMin) pitch = kPitchMin;
		if (pitch > kPitchMax) pitch = kPitchMax;
	}

	void Camera::SnapTo(const Vec3d& target, const Vec3d& upDirection, const Vec3d& forwardHint)
	{
		smoothTarget = target;
		up = Normalize(upDirection);
		forward = ProjectOntoPlane(forwardHint, up);
		distance = kDistance;
		shoulder = kShoulder;
		fov = kFov;
	}

	void Camera::Update(float dt, const Vec3d& target, const Vec3d& upDirection, const Planet& planet)
	{
		up = Normalize(upDirection);
		forward = ProjectOntoPlane(forward, up);   // 걸으면 up 이 바뀌므로 매번 재투영

		smoothTarget.x = DampD(smoothTarget.x, target.x, kFollowLag, dt);
		smoothTarget.y = DampD(smoothTarget.y, target.y, kFollowLag, dt);
		smoothTarget.z = DampD(smoothTarget.z, target.z, kFollowLag, dt);

		float wantDistance = kDistance;
		float wantFov = kFov;
		if (aiming)         { wantDistance = kAimDistance;    wantFov = kAimFov; }
		else if (sprinting) { wantDistance = kSprintDistance; wantFov = kSprintFov; }

		distance = Damp(distance, wantDistance, kZoomLag, dt);
		shoulder = Damp(shoulder, aiming ? kAimShoulder : kShoulder, kZoomLag, dt);
		fov = Damp(fov, wantFov, kZoomLag, dt);

		const Vec3d right = Cross(up, forward);
		const Vec3d focus = smoothTarget + up * double(kFocusHeight) + right * double(shoulder);

		// eye -> focus 방향. pitch 가 클수록 카메라가 위에서 내려다본다.
		const double cp = std::cos(pitch);
		const Vec3d dir = forward * cp - up * std::sin(pitch);

		Vec3d eye = focus - dir * double(distance);

		// 지표면 위 최소 고도 유지 (월드 Y 가 아니라 고도 기준)
		const Vec3d eyeUp = Normalize(eye - planet.center);
		const double eyeAlt = planet.Altitude(eye);
		const double minAlt = planet.SurfaceHeight(eyeUp) + double(kMinEyeAlt);
		if (eyeAlt < minAlt)
			eye = planet.PositionAt(eyeUp, minAlt);

		eyePosition = ToFloat3(eye);

		const XMFLOAT3 focusF = ToFloat3(focus);
		const XMFLOAT3 upF = ToFloat3(up);
		const XMMATRIX view = XMMatrixLookAtLH(
			XMLoadFloat3(&eyePosition), XMLoadFloat3(&focusF), XMLoadFloat3(&upF));
		const XMMATRIX proj = XMMatrixPerspectiveFovLH(fov, aspect, 0.1f, 20000.0f);
		XMStoreFloat4x4(&viewProj, view * proj);
	}
}
