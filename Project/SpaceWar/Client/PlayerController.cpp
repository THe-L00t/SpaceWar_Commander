#include "PlayerController.h"
#include "Input.h"
#include "Camera.h"
#include "Shared/Units.h"
#include <cmath>

using namespace DirectX;
using namespace Shared::units;

// ── 이동 감각 튜닝 (헬다이버즈 참고) ─────────────────────────
//   조작감이 마음에 안 들면 여기 숫자만 만지면 된다.
namespace {
	constexpr float kWalkSpeed   = 6.5_mps;
	constexpr float kSprintSpeed = 11.0_mps;
	constexpr float kAimSpeed    = 3.0_mps;

	constexpr float kStrafeScale = 0.80f;   // 좌우 이동 배율
	constexpr float kBackScale   = 0.55f;   // 후진 배율

	constexpr float kAcceleration = 26.0_mps2;  // 낮출수록 출발이 굼뜸
	constexpr float kFriction     = 16.0_mps2;  // 낮출수록 멈출 때 더 미끄러짐

	constexpr float kTurnRate    = 9.0_radps;   // 몸통 회전 각속도 상한
	constexpr float kAimTurnRate = 22.0_radps;  // 조준 시 빠르게 카메라에 정렬

	constexpr float kMoveEpsilon = 0.15_mps;    // 이 속도 아래면 방향 유지
	constexpr float kJumpSpeed   = 7.0_mps;     // 중력 18 기준 약 1.4m 도약

	// position 은 몸통 중심이다. 발이 지면에 닿으려면 그만큼 띄워야 한다.
	// (더미 큐브가 2m 이므로 1m. 실제 캐릭터가 들어오면 그 반높이로 교체)
	constexpr float kGroundOffset = 1.0_m;

	// 내리막 스텝다운 — 이 낙차까지는 땅에 붙어 내려간다. 이보다 크면(절벽) 실제로 떨어진다.
	// 없으면 g·dt²(60fps 에서 5mm)보다 빨리 낮아지는 경사(질주 시 1.6도)마다 공중 판정이 나고,
	// sprinting 이 grounded 에 묶여 있어 질주가 매 프레임 켜졌다 꺼졌다 한다(속도 6.5↔11, 카메라 거리·FOV 진동).
	// 실제 조각으로 잰 결과: 60fps 질주 중 공중 판정 24%, 초당 8회 → 보정 후 0%.
	constexpr float kStepDown = 0.5_m;
}

namespace swc {

	void PlayerController::Spawn(const Vec3d& worldPosition, const Vec3d& facingDirection)
	{
		up = planet ? planet->Up(worldPosition) : Vec3d{ 0.0, 1.0, 0.0 };

		// ★ 지형 높이 위에 놓는다.
		//   0 중심화 때문에 타일 중앙의 지형 높이가 0이라는 보장이 없다.
		//   고도를 고정값으로 두면 스폰하자마자 솟거나 떨어진다.
		altitude = (planet ? planet->SurfaceHeight(up) : 0.0) + kGroundOffset;
		position = planet ? planet->PositionAt(up, altitude) : worldPosition;

		facing = ProjectOntoPlane(facingDirection, up);
		velocity = { 0.0, 0.0, 0.0 };
		verticalSpeed = 0.0;
		grounded = true;
		speed = 0.0f;
	}

	void PlayerController::Update(float dt, const Input& input, const Camera& camera)
	{
		if (dt <= 0.0f || !planet) return;

		// ① 로컬 기준 갱신 — 걸어다니면 up 이 조금씩 바뀐다
		up = planet->Up(position);
		facing = ProjectOntoPlane(facing, up);
		velocity = velocity - up * Dot(velocity, up);   // 접평면으로 재투영

		// ② 카메라 기준 입력
		float localX = 0.0f;
		float localZ = 0.0f;
		if (input.IsDown('W')) localZ += 1.0f;
		if (input.IsDown('S')) localZ -= 1.0f;
		if (input.IsDown('D')) localX += 1.0f;
		if (input.IsDown('A')) localX -= 1.0f;

		const float inputLen = std::sqrt(localX * localX + localZ * localZ);
		const bool hasInput = inputLen > 0.0f;
		if (hasInput)
		{
			localX /= inputLen;
			localZ /= inputLen;
		}

		const bool aiming = camera.Aiming();
		sprinting = !aiming && hasInput && grounded && input.IsDown(VK_SHIFT);

		// ③ 방향별 속도 배율 — 전진 1.0 / 좌우 0.8 / 후진 0.55 를 성분비로 보간
		float dirScale = 1.0f;
		if (hasInput)
		{
			const float fwd = localZ > 0.0f ? localZ : 0.0f;
			const float back = localZ < 0.0f ? -localZ : 0.0f;
			const float side = std::fabs(localX);
			dirScale = (fwd + back * kBackScale + side * kStrafeScale) / (fwd + back + side);
		}

		float maxSpeed = kWalkSpeed;
		if (aiming) maxSpeed = kAimSpeed;
		else if (sprinting) maxSpeed = kSprintSpeed;

		// 카메라의 접평면 축을 현재 up 기준으로 다시 정렬해서 쓴다
		const Vec3d camForward = ProjectOntoPlane(camera.Forward(), up);
		const Vec3d camRight = Cross(up, camForward);

		const Vec3d target = (camForward * double(localZ) + camRight * double(localX))
			* (double(maxSpeed) * double(dirScale));

		// ④ 가속/감속 — 목표 속도로 일정 비율씩 접근. 관성감의 핵심.
		const double rate = hasInput ? double(kAcceleration) : double(kFriction);
		Vec3d delta = target - velocity;
		const double deltaLen = Length(delta);
		const double step = rate * dt;
		if (deltaLen > step && deltaLen > 0.0)
			delta = delta * (step / deltaLen);
		velocity = velocity + delta;

		// ⑤ 접평면을 따라 직선 이동 (⑧에서 구면에 다시 붙인다)
		position = position + velocity * dt;

		// ⑥ 점프 / 중력 — 고도는 기준구 기준이라 공중에서는 순수 포물선이다
		if (grounded && input.WasPressed(VK_SPACE))
		{
			verticalSpeed = kJumpSpeed;
			grounded = false;
		}
		verticalSpeed -= planet->gravity * dt;
		altitude += verticalSpeed * dt;

		// ⑦ 착지 판정 — 지형 높이는 여기서만 쓴다
		const Vec3d newUp = Normalize(position - planet->center);
		const double groundAltitude = planet->SurfaceHeight(newUp) + kGroundOffset;
		const bool wasGrounded = grounded;   // 점프한 프레임은 이미 false 라 스텝다운에 안 걸린다
		if (altitude <= groundAltitude)
		{
			altitude = groundAltitude;
			verticalSpeed = 0.0;
			grounded = true;
		}
		else if (wasGrounded && verticalSpeed <= 0.0 && altitude - groundAltitude <= double(kStepDown))
		{
			altitude = groundAltitude;
			verticalSpeed = 0.0;
			grounded = true;
		}
		else
		{
			grounded = false;
		}

		// ⑧ 구면 재투영 — 접선 이동으로 생긴 미세 상승도 여기서 제거된다
		position = planet->PositionAt(newUp, altitude);
		up = newUp;
		facing = ProjectOntoPlane(facing, up);
		velocity = velocity - up * Dot(velocity, up);

		speed = float(Length(velocity));

		// ⑨ 몸통 회전 — 조준 중엔 카메라를, 아니면 이동 방향을 향해 서서히
		Vec3d desired = facing;
		double turnRate = kTurnRate;
		if (aiming)
		{
			desired = ProjectOntoPlane(camera.Forward(), up);
			turnRate = kAimTurnRate;
		}
		else if (speed > kMoveEpsilon)
		{
			desired = Normalize(velocity);
		}

		// up 축 기준 부호 있는 각도차를 구해 상한만큼만 돌린다
		const double cosA = std::fmax(-1.0, std::fmin(1.0, Dot(facing, desired)));
		double angle = std::acos(cosA);
		if (angle > 1e-6)
		{
			const double sign = (Dot(Cross(facing, desired), up) >= 0.0) ? 1.0 : -1.0;
			const double maxTurn = turnRate * dt;
			if (angle > maxTurn) angle = maxTurn;
			facing = Normalize(RotateAround(facing, up, angle * sign));
		}
	}

	XMMATRIX PlayerController::WorldMatrix() const
	{
		const Vec3d right = Cross(up, facing);

		XMFLOAT4X4 m;
		m._11 = float(right.x);    m._12 = float(right.y);    m._13 = float(right.z);    m._14 = 0.0f;
		m._21 = float(up.x);       m._22 = float(up.y);       m._23 = float(up.z);       m._24 = 0.0f;
		m._31 = float(facing.x);   m._32 = float(facing.y);   m._33 = float(facing.z);   m._34 = 0.0f;
		m._41 = float(position.x); m._42 = float(position.y); m._43 = float(position.z); m._44 = 1.0f;
		return XMLoadFloat4x4(&m);
	}
}
