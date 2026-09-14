#include "ChaseTargetAction.h"
#include "Shared/AI/BehaviorContext.h"

namespace srv {

	ChaseTargetAction::~ChaseTargetAction() = default;

	Shared::BehaviorStatus ChaseTargetAction::Tick(Shared::BehaviorContext& ctx) const
	{
		if (!ctx.hasNearestPlayer)
		{
			return Shared::BehaviorStatus::Failure;
		}

		ctx.decision.chaseTarget = ctx.nearestPlayerId;
		ctx.decision.moveTarget = ctx.nearestPlayerPos;
		ctx.decision.hasMoveTarget = true;

		// 추격은 끝나는 행동이 아니다. 다음 틱에도 이어서 한다.
		return Shared::BehaviorStatus::Running;
	}

} // namespace srv
