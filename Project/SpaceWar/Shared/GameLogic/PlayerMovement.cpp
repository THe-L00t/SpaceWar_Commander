#include "PlayerMovement.h"

#include <algorithm>
#include <cmath>

#include "../Units.h"
#include "../Physics/CollisionWorld.h"
#include "../Terrain/TerrainSampler.h"

// 머리 주석은 PlayerMovement.h. ①~⑨ 는 2026-10-29 이전 Client/PlayerController.cpp 와 같은 번호다.

using namespace Shared::units;

namespace Shared {

	namespace {

		// ── 이동 감각 튜닝 (헬다이버즈 참고) — Client/PlayerController.cpp 에서 옮겼다 ──
		//   조작감이 마음에 안 들면 여기 숫자만 만지면 된다. 클라 · 서버가 같은 값을 쓴다.
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

		// 내리막 스텝다운 — 이 낙차까지는 땅에 붙어 내려간다. 이보다 크면(절벽) 실제로 떨어진다.
		//  (PlayerController.cpp 의 측정: 없으면 60fps 질주 중 공중 판정 24 %, 초당 8 회 → 보정 후 0 %)
		constexpr double kStepDown = 0.5;

		// ── 충돌 (2026-10-29) ──
		constexpr int    kCollisionIterations = 4;      // 가장 깊은 겹침부터 이만큼 밀어낸다(모서리 · 구석)
		constexpr double kSkin = 0.001;                 // 밀어낸 뒤 1 mm 더 떼어 다음 틱에 같은 면을 다시 덜 잡게
		constexpr double kSupportNormal = 0.7;          // 위쪽 성분이 이보다 크면(약 45° 안) 디디고 설 수 있는 면
	}

	// ── 행성 기하 (Client/Planet.h 와 같은 식) ──
	Vec3d MoveWorld::Up(const Vec3d& position) const
	{
		if constexpr (kUsePlanarMap) return { 0.0, 1.0, 0.0 };
		return Normalize(position - center);
	}

	double MoveWorld::Altitude(const Vec3d& position) const
	{
		if constexpr (kUsePlanarMap) return position.y;
		return Length(position - center) - radius;
	}

	double MoveWorld::SurfaceHeight(const Vec3d& surfacePoint) const
	{
		return terrain ? terrain->Height(surfacePoint.x, surfacePoint.y, surfacePoint.z) : 0.0;
	}

	Vec3d MoveWorld::PositionAt(const Vec3d& surfacePoint, double altitude) const
	{
		if constexpr (kUsePlanarMap) return { surfacePoint.x, altitude, surfacePoint.z };
		return center + surfacePoint * (radius + altitude);
	}

	namespace {

		// 캡슐을 충돌체에서 밀어낸다. 디디고 설 면에 닿았으면 true.
		//  벽(옆면)에 닿으면 그 면으로 들어가는 속도만 지운다 — 벽을 따라 미끄러진다.
		//  천장에 닿으면 올라가던 속도를 지운다.
		bool ResolveCollisions(MoveState& s, const MoveWorld& world)
		{
			if (!world.collision) return false;
			const double r = PlayerMovement::kCapsuleRadius;
			const double bottom = kGroundOffset - r;                               // 몸통 중심 → 아래 구 중심
			const double top = double(PlayerMovement::kCapsuleHeight) - kGroundOffset - r;   // 몸통 중심 → 위 구 중심
			bool supported = false;
			for (int iteration = 0; iteration < kCollisionIterations; ++iteration)
			{
				const Vec3d up = world.Up(s.position);
				const Vec3d local = s.position - world.center;   // 맵 공간
				const Vec3d a = local - up * bottom;
				const Vec3d b = local + up * top;
				CapsuleContact contact;
				if (!world.collision->DeepestCapsuleContact(
						Vec3{ float(a.x), float(a.y), float(a.z) }, Vec3{ float(b.x), float(b.y), float(b.z) },
						PlayerMovement::kCapsuleRadius, contact))
					break;

				const Vec3d n{ contact.normal.x, contact.normal.y, contact.normal.z };
				s.position = s.position + n * (double(contact.depth) + kSkin);

				const double nUp = Dot(n, up);
				if (nUp > kSupportNormal)
				{
					supported = true;
					if (s.verticalSpeed < 0.0) s.verticalSpeed = 0.0;
				}
				else if (nUp < -kSupportNormal)
				{
					if (s.verticalSpeed > 0.0) s.verticalSpeed = 0.0;   // 천장
				}

				// 접평면 쪽 법선으로 미끄러짐 — 들어가는 성분만 지운다
				Vec3d tangentNormal = n - up * nUp;
				const double length = Length(tangentNormal);
				if (length > 1.0e-6)
				{
					tangentNormal = tangentNormal * (1.0 / length);
					const double into = Dot(s.velocity, tangentNormal);
					if (into < 0.0) s.velocity = s.velocity - tangentNormal * into;
				}
			}
			return supported;
		}
	}

	void PlayerMovement::Spawn(MoveState& s, const MoveWorld& world, const Vec3d& worldPosition, const Vec3d& facingDirection)
	{
		s.up = world.Up(worldPosition);

		// ★ 지형 높이 위에 놓는다. 고도를 고정값으로 두면 스폰하자마자 솟거나 떨어진다.
		const Vec3d surfacePoint = kUsePlanarMap ? worldPosition : s.up;
		s.altitude = world.SurfaceHeight(surfacePoint) + kGroundOffset;
		s.position = world.PositionAt(surfacePoint, s.altitude);

		s.facing = ProjectOntoPlane(facingDirection, s.up);
		s.velocity = { 0.0, 0.0, 0.0 };
		s.verticalSpeed = 0.0;
		s.grounded = true;
		s.onObject = false;
		s.sprinting = false;
	}

	void PlayerMovement::Step(MoveState& s, const MoveCommand& command, double dt, const MoveWorld& world)
	{
		if (dt <= 0.0) return;

		// ① 로컬 기준 갱신 — 걸어다니면 up 이 조금씩 바뀐다
		s.up = world.Up(s.position);
		s.facing = ProjectOntoPlane(s.facing, s.up);
		s.velocity = s.velocity - s.up * Dot(s.velocity, s.up);   // 접평면으로 재투영

		// ② 입력 — 축은 −1 ~ 1 로 자르고 대각선은 길이 1 로
		double localX = std::clamp(double(command.moveX), -1.0, 1.0);
		double localZ = std::clamp(double(command.moveZ), -1.0, 1.0);
		const double inputLen = std::sqrt(localX * localX + localZ * localZ);
		const bool hasInput = inputLen > 0.0;
		if (hasInput)
		{
			localX /= inputLen;
			localZ /= inputLen;
		}

		const bool aiming = (command.buttons & kMoveButtonAim) != 0;
		s.sprinting = !aiming && hasInput && s.grounded && (command.buttons & kMoveButtonSprint) != 0;

		// ③ 방향별 속도 배율 — 전진 1.0 / 좌우 0.8 / 후진 0.55 를 성분비로 보간
		double dirScale = 1.0;
		if (hasInput)
		{
			const double fwd = localZ > 0.0 ? localZ : 0.0;
			const double back = localZ < 0.0 ? -localZ : 0.0;
			const double side = std::fabs(localX);
			dirScale = (fwd + back * kBackScale + side * kStrafeScale) / (fwd + back + side);
		}

		double maxSpeed = kWalkSpeed;
		if (aiming) maxSpeed = kAimSpeed;
		else if (s.sprinting) maxSpeed = kSprintSpeed;

		// 카메라 방향(입력에 실려 온다)의 접평면 축을 현재 up 기준으로 다시 정렬해서 쓴다
		Vec3d look{ command.look[0], command.look[1], command.look[2] };
		if (!std::isfinite(look.x) || !std::isfinite(look.y) || !std::isfinite(look.z) || Length(look) < 1.0e-6)
			look = s.facing;
		const Vec3d camForward = ProjectOntoPlane(look, s.up);
		const Vec3d camRight = Cross(s.up, camForward);

		const Vec3d target = (camForward * localZ + camRight * localX) * (maxSpeed * dirScale);

		// ④ 가속/감속 — 목표 속도로 일정 비율씩 접근. 관성감의 핵심.
		const double rate = hasInput ? double(kAcceleration) : double(kFriction);
		Vec3d delta = target - s.velocity;
		const double deltaLen = Length(delta);
		const double step = rate * dt;
		if (deltaLen > step && deltaLen > 0.0)
			delta = delta * (step / deltaLen);
		s.velocity = s.velocity + delta;

		// ⑤ 이동 평면을 따라 직선 이동 (⑧에서 지면 좌표와 고도를 결합한다)
		s.position = s.position + s.velocity * dt;

		// ⑥ 점프 / 중력 — 지형 높이와 고도를 분리해 공중에서는 포물선을 유지한다
		if (s.grounded && (command.buttons & kMoveButtonJump) != 0)
		{
			s.verticalSpeed = kJumpSpeed;
			s.grounded = false;
			s.onObject = false;
		}
		s.verticalSpeed -= world.gravity * dt;
		s.altitude += s.verticalSpeed * dt;

		// ⑦ 착지 판정(지형) — 지형 높이는 여기서만 쓴다
		const Vec3d newUp = world.Up(s.position);
		const Vec3d surfacePoint = kUsePlanarMap ? s.position : newUp;
		const double groundAltitude = world.SurfaceHeight(surfacePoint) + kGroundOffset;
		const bool wasGrounded = s.grounded;   // 점프한 프레임은 이미 false 라 스텝다운에 안 걸린다
		const bool wasOnObject = s.onObject;
		if (s.altitude <= groundAltitude)
		{
			s.altitude = groundAltitude;
			s.verticalSpeed = 0.0;
			s.grounded = true;
			s.onObject = false;
		}
		else if (wasGrounded && !wasOnObject && s.verticalSpeed <= 0.0 && s.altitude - groundAltitude <= kStepDown)
		{
			// ★ 오브젝트 위에 서 있던 틱은 지형으로 붙이지 않는다 — 윗면 아래(오브젝트 속)로 끌려 들어가 옆으로 밀려난다
			s.altitude = groundAltitude;
			s.verticalSpeed = 0.0;
			s.grounded = true;
		}
		else
		{
			s.grounded = false;
			s.onObject = false;
		}

		// ⑧ 지면 방향과 고도 결합 — 현재 구면 또는 평면 규약으로 재투영한다.
		s.position = world.PositionAt(surfacePoint, s.altitude);
		s.up = newUp;

		// ⑧-1 충돌체 (2026-10-29) — 밀어내기 · 미끄러짐 · 오브젝트 윗면에 서기
		if (world.collision)
		{
			bool supported = ResolveCollisions(s, world);
			if (!supported && wasOnObject && wasGrounded && s.verticalSpeed <= 0.0)
			{
				// 오브젝트 윗면을 따라 내려가기 — kStepDown 만큼(지형 밑으로는 안 간다) 내려 보고 받쳐 주면 그 자리
				MoveState probe = s;
				const Vec3d probeUp = world.Up(probe.position);
				const Vec3d probePoint = kUsePlanarMap ? probe.position : probeUp;
				const double probeGround = world.SurfaceHeight(probePoint) + kGroundOffset;
				probe.altitude = (std::max)(world.Altitude(probe.position) - kStepDown, probeGround);
				probe.position = world.PositionAt(probePoint, probe.altitude);
				if (ResolveCollisions(probe, world))
				{
					s = probe;
					supported = true;
				}
				else if (probe.altitude <= probeGround + 1.0e-9)
				{
					s = probe;            // 지형까지 내려왔다
					s.grounded = true;
					s.onObject = false;
					s.verticalSpeed = 0.0;
				}
			}
			if (supported)
			{
				s.grounded = true;
				s.onObject = true;
				if (s.verticalSpeed < 0.0) s.verticalSpeed = 0.0;
			}
			// 밀려난 자리에서 고도 · 위쪽을 다시 잰다
			s.up = world.Up(s.position);
			s.altitude = world.Altitude(s.position);
		}

		s.facing = ProjectOntoPlane(s.facing, s.up);
		s.velocity = s.velocity - s.up * Dot(s.velocity, s.up);
		const double speed = Length(s.velocity);

		// ⑨ 몸통 회전 — 조준 중엔 카메라를, 아니면 이동 방향을 향해 서서히
		Vec3d desired = s.facing;
		double turnRate = kTurnRate;
		if (aiming)
		{
			desired = ProjectOntoPlane(look, s.up);
			turnRate = kAimTurnRate;
		}
		else if (speed > kMoveEpsilon)
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

} // namespace Shared
