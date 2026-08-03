#include "Shared/Sim/PlayerMotion.h"
#include "Shared/Units.h"
#include <cmath>

using namespace Shared::units;

// ── 이동 감각 튜닝 (헬다이버즈 참고) ─────────────────────────
//   조작감이 마음에 안 들면 여기 숫자만 만지면 된다.
//
//   ★ 이 상수들이 Shared 에 있어야 하는 이유
//     서버가 위치를 검증하려면 "1/30초에 최대 얼마나 갈 수 있는가" 를 알아야 한다.
//     클라만 아는 값이면 정상 플레이어가 튕기거나 핵이 통과한다.
namespace {
	constexpr float kWalkSpeed = 6.5_mps;
	constexpr float kSprintSpeed = 11.0_mps;
	constexpr float kAimSpeed = 3.0_mps;

	constexpr float kStrafeScale = 0.80f;   // 좌우 이동 배율
	constexpr float kBackScale = 0.55f;     // 후진 배율

	constexpr float kAcceleration = 26.0_mps2;  // 낮출수록 출발이 굼뜸
	constexpr float kFriction = 16.0_mps2;      // 낮출수록 멈출 때 더 미끄러짐

	constexpr float kTurnRate = 9.0_radps;      // 몸통 회전 각속도 상한
	constexpr float kAimTurnRate = 22.0_radps;  // 조준 시 빠르게 카메라에 정렬

	constexpr float kMoveEpsilon = 0.15_mps;    // 이 속도 아래면 방향 유지
	constexpr float kJumpSpeed = 7.0_mps;       // 중력 18 기준 약 1.4m 도약

	// position 은 몸통 중심이다. 발이 지면에 닿으려면 그만큼 띄워야 한다.
	// (더미 큐브가 2m 이므로 1m. 실제 캐릭터가 들어오면 그 반높이로 교체)
	constexpr float kGroundOffset = 1.0_m;
}

namespace Shared {

	void SpawnMotion(MotionState& s, const Planet& planet,
		const Vec3d& worldPosition, const Vec3d& facingDirection)
	{
		s.up = planet.Up(worldPosition);

		// ★ 지형 높이 위에 놓는다.
		//   0 중심화 때문에 타일 중앙의 지형 높이가 0이라는 보장이 없다.
		//   고도를 고정값으로 두면 스폰하자마자 솟거나 떨어진다.
		s.altitude = planet.SurfaceHeight(s.up) + kGroundOffset;
		s.position = planet.PositionAt(s.up, s.altitude);

		s.facing = ProjectOntoPlane(facingDirection, s.up);
		s.velocity = { 0.0, 0.0, 0.0 };
		s.verticalSpeed = 0.0;
		s.grounded = true;
		s.sprinting = false;
		s.speed = 0.0f;
	}

	void StepMotion(MotionState& s, const MoveInput& in, const Planet& planet, float dt)
	{
		if (dt <= 0.0f) return;

		// ① 로컬 기준 갱신 — 걸어다니면 up 이 조금씩 바뀐다
		s.up = planet.Up(s.position);
		s.facing = ProjectOntoPlane(s.facing, s.up);
		s.velocity = s.velocity - s.up * Dot(s.velocity, s.up);   // 접평면으로 재투영

		// ② 입력
		float localX = in.moveX;
		float localZ = in.moveZ;

		const float inputLen = std::sqrt(localX * localX + localZ * localZ);
		const bool hasInput = inputLen > 0.0f;
		if (hasInput)
		{
			localX /= inputLen;
			localZ /= inputLen;
		}

		const bool aiming = in.Aiming();
		s.sprinting = !aiming && hasInput && s.grounded && in.Sprint();

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
		else if (s.sprinting) maxSpeed = kSprintSpeed;

		// 카메라의 접평면 축을 현재 up 기준으로 다시 정렬해서 쓴다.
		// (서버엔 카메라가 없으므로 클라가 보낸 aimDir 이 유일한 근거다)
		const Vec3d camForward = ProjectOntoPlane(in.aimDir, s.up);
		const Vec3d camRight = Cross(s.up, camForward);

		const Vec3d target = (camForward * double(localZ) + camRight * double(localX))
			* (double(maxSpeed) * double(dirScale));

		// ④ 가속/감속 — 목표 속도로 일정 비율씩 접근. 관성감의 핵심.
		const double rate = hasInput ? double(kAcceleration) : double(kFriction);
		Vec3d delta = target - s.velocity;
		const double deltaLen = Length(delta);
		const double step = rate * dt;
		if (deltaLen > step && deltaLen > 0.0)
			delta = delta * (step / deltaLen);
		s.velocity = s.velocity + delta;

		// ⑤ 접평면을 따라 직선 이동 (⑧에서 구면에 다시 붙인다)
		s.position = s.position + s.velocity * dt;

		// ⑥ 점프 / 중력 — 고도는 기준구 기준이라 공중에서는 순수 포물선이다
		if (s.grounded && in.Jump())
		{
			s.verticalSpeed = kJumpSpeed;
			s.grounded = false;
		}
		s.verticalSpeed -= planet.gravity * dt;
		s.altitude += s.verticalSpeed * dt;

		// ⑦ 착지 판정 — 지형 높이는 여기서만 쓴다
		const Vec3d newUp = Normalize(s.position - planet.center);
		const double groundAltitude = planet.SurfaceHeight(newUp) + kGroundOffset;
		if (s.altitude <= groundAltitude)
		{
			s.altitude = groundAltitude;
			s.verticalSpeed = 0.0;
			s.grounded = true;
		}
		else
		{
			s.grounded = false;
		}

		// ⑧ 구면 재투영 — 접선 이동으로 생긴 미세 상승도 여기서 제거된다
		s.position = planet.PositionAt(newUp, s.altitude);
		s.up = newUp;
		s.facing = ProjectOntoPlane(s.facing, s.up);
		s.velocity = s.velocity - s.up * Dot(s.velocity, s.up);

		s.speed = float(Length(s.velocity));

		// ⑨ 몸통 회전 — 조준 중엔 카메라를, 아니면 이동 방향을 향해 서서히
		Vec3d desired = s.facing;
		double turnRate = kTurnRate;
		if (aiming)
		{
			desired = ProjectOntoPlane(in.aimDir, s.up);
			turnRate = kAimTurnRate;
		}
		else if (s.speed > kMoveEpsilon)
		{
			desired = Normalize(s.velocity);
		}

		// up 축 기준 부호 있는 각도차를 구해 상한만큼만 돌린다
		const double cosA = std::fmax(-1.0, std::fmin(1.0, Dot(s.facing, desired)));
		double angle = std::acos(cosA);
		if (angle > 1e-6)
		{
			const double sign = (Dot(Cross(s.facing, desired), s.up) >= 0.0) ? 1.0 : -1.0;
			const double maxTurn = turnRate * dt;
			if (angle > maxTurn) angle = maxTurn;
			s.facing = Normalize(RotateAround(s.facing, s.up, angle * sign));
		}
	}
}
