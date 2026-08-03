#include "PlayerController.h"
#include "Input.h"
#include "Camera.h"

using namespace DirectX;

namespace swc {

	void PlayerController::Spawn(const Vec3d& worldPosition, const Vec3d& facingDirection)
	{
		if (!planet) return;
		Shared::SpawnMotion(state, *planet, worldPosition, facingDirection);
	}

	// ── 입력 수집 ───────────────────────────────────────────
	//
	//  ★ 이 함수가 클라와 서버의 경계다.
	//    여기까지가 "키보드와 화면을 아는 세계",
	//    여기서 나온 MoveInput 부터가 "서버도 아는 세계"다.
	//
	//  ★ aimDir 을 왜 입력에 넣나
	//    우리 이동은 카메라 상대(전진 = 카메라가 보는 쪽)다.
	//    서버엔 카메라가 없으므로 이 방향을 같이 보내야 이동 축을 잡을 수 있다.
	MoveInput PlayerController::CollectInput(const Input& input, const Camera& camera) const
	{
		MoveInput in{};

		if (input.IsDown('W')) in.moveZ += 1.0f;
		if (input.IsDown('S')) in.moveZ -= 1.0f;
		if (input.IsDown('D')) in.moveX += 1.0f;
		if (input.IsDown('A')) in.moveX -= 1.0f;

		if (input.WasPressed(VK_SPACE)) in.buttons |= Shared::Btn_Jump;
		if (input.IsDown(VK_SHIFT))     in.buttons |= Shared::Btn_Sprint;
		if (camera.Aiming())            in.buttons |= Shared::Btn_Aim;

		in.aimDir = camera.Forward();
		return in;
	}

	void PlayerController::Step(const MoveInput& in, float dt)
	{
		if (!planet) return;
		Shared::StepMotion(state, in, *planet, dt);
	}

	// ── 렌더용 월드 행렬 ────────────────────────────────────
	//  기저 벡터(오른쪽 / 위 / 앞)를 행으로 놓고 마지막 행에 위치를 넣는다.
	XMMATRIX PlayerController::MakeWorldMatrix(const Vec3d& position,
		const Vec3d& up, const Vec3d& facing)
	{
		const Vec3d right = Cross(up, facing);

		XMFLOAT4X4 m;
		m._11 = float(right.x);    m._12 = float(right.y);    m._13 = float(right.z);    m._14 = 0.0f;
		m._21 = float(up.x);       m._22 = float(up.y);       m._23 = float(up.z);       m._24 = 0.0f;
		m._31 = float(facing.x);   m._32 = float(facing.y);   m._33 = float(facing.z);   m._34 = 0.0f;
		m._41 = float(position.x); m._42 = float(position.y); m._43 = float(position.z); m._44 = 1.0f;
		return XMLoadFloat4x4(&m);
	}

	XMMATRIX PlayerController::WorldMatrix() const
	{
		return MakeWorldMatrix(state.position, state.up, state.facing);
	}
}
