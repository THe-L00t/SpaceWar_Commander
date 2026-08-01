#pragma once
#include <DirectXMath.h>
#include "Planet.h"

// 구면 위 3인칭 이동. 헬다이버즈식 관성·회전 지연은 그대로 유지하고
// 축만 월드 XZ 평면에서 접평면으로 바꾼다.
//
// ★ 각도(yaw) 대신 방향 벡터를 들고 다닌다.
//   구면에서 각도로 방향을 표현하려면 기준 방향이 필요한데
//   어떤 기준을 잡아도 극점에서 무너진다.
//
// ★ 상태는 "지형 위 고도"가 아니라 "기준구 위 고도"다.
//   지형 위 고도로 잡으면 점프 중에도 지형을 따라가서
//   포물선이 아니라 언덕에 들러붙는 점프가 된다.
namespace swc {
	class Input;
	class Camera;

	class PlayerController
	{
	public:
		void SetPlanet(const Planet* p) { planet = p; }
		void Spawn(const Vec3d& worldPosition, const Vec3d& facingDirection);

		void Update(float, const Input&, const Camera&);

		const Vec3d& Position() const { return position; }
		const Vec3d& Facing() const { return facing; }
		const Vec3d& Up() const { return up; }

		float  Speed() const { return speed; }
		double Altitude() const { return altitude; }
		bool   IsGrounded() const { return grounded; }
		bool   IsSprinting() const { return sprinting; }

		DirectX::XMMATRIX WorldMatrix() const;

	private:
		const Planet* planet = nullptr;

		Vec3d position{ 0.0, 0.0, 0.0 };
		Vec3d velocity{ 0.0, 0.0, 0.0 };   // 접평면 속도 (항상 up 과 수직)
		Vec3d facing{ 0.0, 0.0, 1.0 };     // 몸이 향한 방향 (접평면 단위벡터)
		Vec3d up{ 0.0, 1.0, 0.0 };

		double altitude = 0.0;             // 기준구 위 고도
		double verticalSpeed = 0.0;

		bool  grounded = true;
		bool  sprinting = false;
		float speed = 0.0f;
	};
}
