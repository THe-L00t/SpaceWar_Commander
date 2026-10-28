#pragma once
#include "DynamicObject.h"

// 명세 6.6 — 현재 클라이언트가 직접 조작하는 플레이어. 클라이언트당 하나.
// 입력은 Player 가 직접 받지 않는다: Input Manager → Class Bridge → Game Logic → Player.
// 명세의 Weapon 은 이번 범위에서 제외했다.
namespace Shared {

	class Player : public DynamicObject
	{
	public:
		~Player() override;
	};

} // namespace Shared
