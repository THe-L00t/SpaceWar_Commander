#pragma once
#include <DirectXMath.h>

// 3인칭 궤도 카메라. 마우스로 yaw/pitch, 타겟은 감쇠 추종(스프링).
// 조준(우클릭) 시 어깨 너머로 당겨지고 FOV 가 좁아진다.
namespace swc {
	class Camera
	{
	public:
		void SetAspect(float a) { aspect = a; }
		void SetAiming(bool a) { aiming = a; }
		void SetSprinting(bool s) { sprinting = s; }

		void AddLook(float yawDelta, float pitchDelta);
		void SnapTo(const DirectX::XMFLOAT3&);
		void Update(float, const DirectX::XMFLOAT3&);

		bool  Aiming() const { return aiming; }
		float LookScale() const { return aiming ? 0.55f : 1.0f; }   // 조준 중 감도 저하
		float Yaw() const { return yaw; }

		DirectX::XMFLOAT3 ForwardXZ() const;   // 카메라 상대 이동용 (XZ 평면)
		DirectX::XMFLOAT3 RightXZ() const;

		const DirectX::XMFLOAT4X4& ViewProj() const { return viewProj; }
		const DirectX::XMFLOAT3& EyePosition() const { return eyePosition; }   // 프레넬의 V 벡터용

	private:
		float aspect = 16.0f / 9.0f;
		float yaw = 0.0f;
		float pitch = 0.35f;

		bool aiming = false;
		bool sprinting = false;

		float distance = 6.0f;
		float shoulder = 1.5f;
		float fov = 1.05f;

		DirectX::XMFLOAT3   smoothTarget{ 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT3   eyePosition{ 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT4X4 viewProj{};
	};
}
