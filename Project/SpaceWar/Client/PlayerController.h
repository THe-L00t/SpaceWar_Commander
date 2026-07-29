#pragma once
#include <DirectXMath.h>

// 헬다이버즈식 3인칭 이동. 카메라 상대 입력 → 가속/감속 → 몸통 회전 지연.
// 나중에 서버 권위로 바뀌면 이 클래스만 갈아끼우면 된다(입력 전송 지점).
namespace swc {
	class Input;
	class Camera;

	class PlayerController
	{
	public:
		void SetPosition(const DirectX::XMFLOAT3& p) { position = p; }
		void SetMapLimit(float l) { mapLimit = l; }

		void Update(float, const Input&, const Camera&);

		const DirectX::XMFLOAT3& Position() const { return position; }
		float BodyYaw() const { return bodyYaw; }
		float Speed() const { return speed; }
		bool  IsSprinting() const { return sprinting; }

		DirectX::XMMATRIX WorldMatrix() const;

	private:
		DirectX::XMFLOAT3 position{ 0.0f, 1.0f, 0.0f };
		DirectX::XMFLOAT3 velocity{ 0.0f, 0.0f, 0.0f };

		float bodyYaw = 0.0f;
		float speed = 0.0f;
		bool  sprinting = false;
		float mapLimit = 49.0f;
	};
}
