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
	// 조준 시 — 2026-11-14 사용자 «우클릭 조준시 … 좀 더 시점 낮춰주고 왼쪽으로 이동할것»: 1.0 → 0.6 m (화면 가운데 쪽으로)
	constexpr float kAimShoulder = 0.6_m;

	constexpr float kFocusHeight    = 1.8_m;    // 타겟(몸통 중심) 기준 주시점 높이
	// 조준 시 주시점 높이 — 같은 지시로 1.8 → 1.1 m. 몸통 중심이 지면 위 1 m 라 지면 위 2.1 m(캐릭터 키 1.8 m 의 머리보다 0.3 m 위)를 본다.
	//  눈(eye)은 주시점에서 거리만큼 뒤에 있으므로 같이 낮아진다. 조준 전환 속도는 kZoomLag 그대로.
	constexpr float kAimFocusHeight = 1.1_m;
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

	Vec3d Camera::LookDirection() const
	{
		return Normalize(forward * std::cos(pitch) - up * std::sin(pitch));
	}

	void Camera::SnapTo(const Vec3d& target, const Vec3d& upDirection, const Vec3d& forwardHint)
	{
		smoothTarget = target;
		up = Normalize(upDirection);
		forward = ProjectOntoPlane(forwardHint, up);
		distance = kDistance;
		shoulder = kShoulder;
		focusHeight = kFocusHeight;
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
		focusHeight = Damp(focusHeight, aiming ? kAimFocusHeight : kFocusHeight, kZoomLag, dt);
		fov = Damp(fov, wantFov, kZoomLag, dt);

		const Vec3d right = Cross(up, forward);
		const Vec3d focus = smoothTarget + up * double(focusHeight) + right * double(shoulder);

		// eye -> focus 방향. pitch 가 클수록 카메라가 위에서 내려다본다.
		const double cp = std::cos(pitch);
		const Vec3d dir = forward * cp - up * std::sin(pitch);

		Vec3d eye = focus - dir * double(distance);

		// 지표면 위 최소 고도 유지. 평면 맵은 해당 XZ의 지면 Y로 보정한다.
		const Vec3d eyeUp = planet.Up(eye);
		const Vec3d surfacePoint = Shared::kUsePlanarMap ? eye : eyeUp;
		const double eyeAlt = planet.Altitude(eye);
		const double minAlt = planet.SurfaceHeight(surfacePoint) + double(kMinEyeAlt);
		if (eyeAlt < minAlt)
			eye = planet.PositionAt(surfacePoint, minAlt);

		eyePosition = ToFloat3(eye);

		const XMFLOAT3 focusF = ToFloat3(focus);
		const XMFLOAT3 upF = ToFloat3(up);
		const XMMATRIX view = XMMatrixLookAtLH(
			XMLoadFloat3(&eyePosition), XMLoadFloat3(&focusF), XMLoadFloat3(&upF));
		const XMMATRIX proj = XMMatrixPerspectiveFovLH(fov, aspect, 0.1f, 20000.0f);
		XMStoreFloat4x4(&viewProj, view * proj);
		// 원거리 배경만 별도 near 평면을 사용하고 같은 FOV·카메라 방향을 유지한다.
		const XMMATRIX backgroundProj = XMMatrixPerspectiveFovLH(fov, aspect, 10.0f, 20000.0f);
		XMStoreFloat4x4(&backgroundViewProj, view * backgroundProj);
	}
}
