#pragma once
#include "DynamicObject.h"

// 명세 6.8 — AI 에 의해 행동하는 캐릭터.
// 행동 계산은 담당하는 쪽(서버 또는 담당 클라이언트)이, 누군가 보고 있을 때만 한다.
// 피해·체력·보상·담당 전환의 판정은 언제나 서버가 한다 (명세 6.13).
// 명세의 Behavior Tree 는 이번 범위에서 제외했다.
namespace Shared {

	class NPC : public DynamicObject
	{
	public:
		~NPC() override;
	};

} // namespace Shared
