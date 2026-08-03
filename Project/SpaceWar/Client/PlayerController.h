#pragma once
#include <DirectXMath.h>
#include "SimTypes.h"

// ============================================================
//  PlayerController — 클라 쪽 껍데기
//
//  이동 계산 자체는 Shared/Sim/PlayerMotion 이 한다. 서버와 똑같은 코드다.
//  여기 남은 책임은 셋뿐이다.
//    ① 키보드·카메라를 MoveInput 으로 번역   (서버는 못 하는 일)
//    ② 그 입력으로 한 틱 전진 (= 클라 예측)
//    ③ 렌더용 월드 행렬 만들기               (서버는 할 필요가 없는 일)
// ============================================================

namespace swc {
	class Input;
	class Camera;

	class PlayerController
	{
	public:
		void SetPlanet(const Planet* p) { planet = p; }
		void Spawn(const Vec3d& worldPosition, const Vec3d& facingDirection);

		// ① 입력 수집 — 여기서만 Input/Camera 를 안다
		MoveInput CollectInput(const Input&, const Camera&) const;

		// ② 한 틱 전진. dt 는 반드시 Shared::kTickSeconds 여야 한다.
		void Step(const MoveInput&, float dt);

		// 서버가 보낸 권위 상태로 덮어쓴다 (보정)
		void ApplyState(const MotionState& s) { state = s; }
		const MotionState& State() const { return state; }
		MotionState& MutableState() { return state; }

		const Vec3d& Position() const { return state.position; }
		const Vec3d& Facing()   const { return state.facing; }
		const Vec3d& Up()       const { return state.up; }

		float  Speed()       const { return state.speed; }
		double Altitude()    const { return state.altitude; }
		bool   IsGrounded()  const { return state.grounded; }
		bool   IsSprinting() const { return state.sprinting; }

		// ③ 렌더 전용. 서버로 절대 옮기지 않는다.
		DirectX::XMMATRIX WorldMatrix() const;

		// 원격 플레이어를 그릴 때도 쓰는 공용 버전
		static DirectX::XMMATRIX MakeWorldMatrix(const Vec3d& position,
			const Vec3d& up, const Vec3d& facing);

	private:
		const Planet* planet = nullptr;
		MotionState   state;
	};
}
