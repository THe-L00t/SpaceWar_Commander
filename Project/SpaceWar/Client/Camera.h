#pragma once
#include <DirectXMath.h>
#include "Planet.h"

// 구면 위 3인칭 궤도 카메라.
//
// ★ 월드 Y축 기준 yaw 를 버리고 접평면 전방 벡터를 들고 다닌다.
//   플레이어의 facing 과는 독립이다 (몸이 돌아도 시점은 그대로).
//   pitch 만 스칼라로 남는다 — 로컬 지평선 기준 각도.
namespace swc {
	class Camera
	{
	public:
		void SetAspect(float a) { aspect = a; }
		void SetAiming(bool a) { aiming = a; }
		void SetSprinting(bool s) { sprinting = s; }

		void AddLook(float yawDelta, float pitchDelta);
		void SnapTo(const Vec3d& target, const Vec3d& up, const Vec3d& forwardHint);
		void Update(float, const Vec3d& target, const Vec3d& up, const Planet&);

		bool  Aiming() const { return aiming; }
		float LookScale() const { return aiming ? 0.55f : 1.0f; }   // 조준 중 감도 저하

		const Vec3d& Forward() const { return forward; }   // 접평면 전방 (이동 기준축)
		Vec3d Right() const { return Cross(up, forward); }

		const DirectX::XMFLOAT4X4& ViewProj() const { return viewProj; }
		const DirectX::XMFLOAT3& EyePosition() const { return eyePosition; }

	private:
		float aspect = 16.0f / 9.0f;
		float pitch = 0.35f;      // 로컬 지평선 기준. + 면 내려다봄

		bool aiming = false;
		bool sprinting = false;

		float distance = 6.0f;
		float shoulder = 1.5f;
		float fov = 1.05f;

		Vec3d forward{ 0.0, 0.0, 1.0 };   // 접평면 단위벡터
		Vec3d up{ 0.0, 1.0, 0.0 };
		Vec3d smoothTarget{ 0.0, 0.0, 0.0 };

		DirectX::XMFLOAT3   eyePosition{ 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT4X4 viewProj{};
	};
}
