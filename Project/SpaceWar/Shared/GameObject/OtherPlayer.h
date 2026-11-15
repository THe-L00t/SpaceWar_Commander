#pragma once
#include "DynamicObject.h"
#include "Weapon.h"

// 명세 6.7 — 현재 클라이언트에서 자신을 제외한 다른 플레이어.
// 상태의 갱신 원천은 로컬 입력이 아니라 서버가 보내는 네트워크 데이터다.
// 명세 6.7.2 Weapon (2026-11-14) — 한 번에 한 자루(Weapon.h). 남이 든 총은 3인칭 모델로 그린다.
namespace Shared {

	class OtherPlayer : public DynamicObject
	{
	public:
		~OtherPlayer() override;

		Weapon weapon;   // 서버가 알려 준 종류(바뀔 때만 받는다 — 예정)
	};

} // namespace Shared
