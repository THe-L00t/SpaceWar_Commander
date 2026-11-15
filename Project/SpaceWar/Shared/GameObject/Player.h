#pragma once
#include "DynamicObject.h"
#include "Weapon.h"

// 명세 6.6 — 현재 클라이언트가 직접 조작하는 플레이어. 클라이언트당 하나.
// 입력은 Player 가 직접 받지 않는다: Input Manager → Class Bridge → Game Logic → Player.
// 명세 6.6.2 Weapon (2026-11-14) — 한 번에 한 자루(Weapon.h). 자기가 든 총은 1인칭 모델로 그린다.
namespace Shared {

	class Player : public DynamicObject
	{
	public:
		~Player() override;

		Weapon weapon;   // 빈손이면 weapon.Held() == false
	};

} // namespace Shared
