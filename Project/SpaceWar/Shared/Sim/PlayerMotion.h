#pragma once
#include "Shared/Sim/MoveInput.h"
#include "Shared/World/Planet.h"

// ============================================================
//  PlayerMotion — ★ 클라와 서버가 공유하는 단 하나의 이동 계산
//
//  같은 (상태, 입력, dt) 를 넣으면 양쪽에서 같은 결과가 나와야 한다.
//  이게 성립해야
//    - 클라는 입력 즉시 예측해서 지연 없이 움직이고
//    - 서버 결과가 오면 비교해서 어긋난 만큼만 보정할 수 있다
//  두 곳에 비슷한 코드를 따로 두면 매 프레임 어긋나 캐릭터가 계속 튄다.
//
//  ★ dt 는 반드시 고정 틱이어야 한다
//    프레임마다 다른 dt 를 쓰면 같은 입력에도 다른 위치가 나온다.
//    Shared::kTickSeconds 를 쓴다.
// ============================================================

namespace Shared {

	// 한 틱 전진.
	void StepMotion(MotionState& s, const MoveInput& in, const Planet& planet, float dt);

	// 표면 위에 세운다. 지형 높이를 반영하므로 스폰하자마자 솟거나 떨어지지 않는다.
	void SpawnMotion(MotionState& s, const Planet& planet,
		const Vec3d& worldPosition, const Vec3d& facingDirection);
}
