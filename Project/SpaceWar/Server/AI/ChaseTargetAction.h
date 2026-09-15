#pragma once
#include "Shared/AI/Node/ActionNode.h"

// 가장 가까운 플레이어를 쫓겠다고 «결정» 한다.
//
// ★ NPC 를 직접 움직이지 않는다. 결정을 ctx.decision 에 적을 뿐이고,
//   실제 이동은 NpcWorld 가 그 결정을 보고 적용한다.
//   (아키텍처 명세서 6.13 — 행동은 담당이 정하고 판정은 서버가 한다.
//    지금은 담당도 서버라 둘 다 서버에서 일어나지만 경계는 그대로 둔다)
namespace srv {

	class ChaseTargetAction : public Shared::ActionNode
	{
	public:
		~ChaseTargetAction() override;

		Shared::BehaviorStatus Tick(Shared::BehaviorContext& ctx) const override;
	};

} // namespace srv
