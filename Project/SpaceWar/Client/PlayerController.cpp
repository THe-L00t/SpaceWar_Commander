#include "PlayerController.h"
#include "Input.h"
#include "Camera.h"
#include <cmath>

using namespace DirectX;

// ── 이동 감각 튜닝 (헬다이버즈 참고) ─────────────────────────
//   조작감이 마음에 안 들면 여기 숫자만 만지면 된다.
namespace {
	constexpr float kWalkSpeed   = 6.5f;    // 전진 최고속 (m/s)
	constexpr float kSprintSpeed = 11.0f;   // Shift 질주
	constexpr float kAimSpeed    = 3.0f;    // 조준 중 감속

	constexpr float kStrafeScale = 0.80f;   // 좌우 이동 배율
	constexpr float kBackScale   = 0.55f;   // 후진 배율

	constexpr float kAcceleration = 26.0f;  // 낮출수록 출발이 굼뜸 (m/s^2)
	constexpr float kFriction     = 16.0f;  // 낮출수록 멈출 때 더 미끄러짐

	constexpr float kTurnRate    = 9.0f;    // 몸통 회전 각속도 상한 (rad/s)
	constexpr float kAimTurnRate = 22.0f;   // 조준 시 빠르게 카메라에 정렬

	constexpr float kMoveEpsilon = 0.15f;   // 이 속도 아래면 방향 유지

	float WrapAngle(float a)
	{
		while (a > XM_PI) a -= XM_2PI;
		while (a < -XM_PI) a += XM_2PI;
		return a;
	}
}

namespace swc {

	void PlayerController::Update(float dt, const Input& input, const Camera& camera)
	{
		if (dt <= 0.0f) return;

		// ① 카메라 기준 로컬 입력
		float localX = 0.0f;
		float localZ = 0.0f;
		if (input.IsDown('W')) localZ += 1.0f;
		if (input.IsDown('S')) localZ -= 1.0f;
		if (input.IsDown('D')) localX += 1.0f;
		if (input.IsDown('A')) localX -= 1.0f;

		const float inputLen = sqrtf(localX * localX + localZ * localZ);
		const bool hasInput = inputLen > 0.0f;
		if (hasInput)
		{
			localX /= inputLen;
			localZ /= inputLen;
		}

		const bool aiming = camera.Aiming();
		sprinting = !aiming && hasInput && input.IsDown(VK_SHIFT);

		// ② 방향별 속도 배율 — 전진 1.0 / 좌우 0.8 / 후진 0.55 를 성분비로 보간
		float dirScale = 1.0f;
		if (hasInput)
		{
			const float fwd = localZ > 0.0f ? localZ : 0.0f;
			const float back = localZ < 0.0f ? -localZ : 0.0f;
			const float side = fabsf(localX);
			dirScale = (fwd + back * kBackScale + side * kStrafeScale) / (fwd + back + side);
		}

		float maxSpeed = kWalkSpeed;
		if (aiming) maxSpeed = kAimSpeed;
		else if (sprinting) maxSpeed = kSprintSpeed;

		const XMFLOAT3 f = camera.ForwardXZ();
		const XMFLOAT3 r = camera.RightXZ();
		const float targetX = (f.x * localZ + r.x * localX) * maxSpeed * dirScale;
		const float targetZ = (f.z * localZ + r.z * localX) * maxSpeed * dirScale;

		// ③ 가속/감속 — 목표 속도로 일정 비율씩 접근. 관성감의 핵심.
		const float rate = hasInput ? kAcceleration : kFriction;
		float dx = targetX - velocity.x;
		float dz = targetZ - velocity.z;
		const float diffLen = sqrtf(dx * dx + dz * dz);
		const float step = rate * dt;
		if (diffLen > step && diffLen > 0.0f)
		{
			dx *= step / diffLen;
			dz *= step / diffLen;
		}
		velocity.x += dx;
		velocity.z += dz;

		position.x += velocity.x * dt;
		position.z += velocity.z * dt;

		// ④ 맵 경계 — 속도까지 죽여야 벽에 붙은 채 속도가 쌓이지 않는다
		if (position.x > mapLimit) { position.x = mapLimit; velocity.x = 0.0f; }
		if (position.x < -mapLimit) { position.x = -mapLimit; velocity.x = 0.0f; }
		if (position.z > mapLimit) { position.z = mapLimit; velocity.z = 0.0f; }
		if (position.z < -mapLimit) { position.z = -mapLimit; velocity.z = 0.0f; }

		speed = sqrtf(velocity.x * velocity.x + velocity.z * velocity.z);

		// ⑤ 몸통 회전 — 조준 중엔 카메라를, 아니면 이동 방향을 향해 서서히
		float desiredYaw = bodyYaw;
		float turnRate = kTurnRate;
		if (aiming)
		{
			desiredYaw = camera.Yaw();
			turnRate = kAimTurnRate;
		}
		else if (speed > kMoveEpsilon)
		{
			desiredYaw = atan2f(velocity.x, velocity.z);
		}

		const float diff = WrapAngle(desiredYaw - bodyYaw);
		const float maxTurn = turnRate * dt;
		const float turn = fabsf(diff) <= maxTurn ? diff : (diff > 0.0f ? maxTurn : -maxTurn);
		bodyYaw = WrapAngle(bodyYaw + turn);
	}

	XMMATRIX PlayerController::WorldMatrix() const
	{
		return XMMatrixRotationY(bodyYaw) * XMMatrixTranslation(position.x, position.y, position.z);
	}
}
