#include "IdleAction.h"
#include "Shared/AI/BehaviorContext.h"

namespace srv {

	IdleAction::~IdleAction() = default;

	Shared::BehaviorStatus IdleAction::Tick(Shared::BehaviorContext& ctx) const
	{
		ctx.decision.hasMoveTarget = false;
		ctx.decision.chaseTarget = 0;

		// 대기는 끝나지 않는다. 다음 틱에 조건이 바뀌면 Selector 가 알아서 위 갈래로 간다.
		return Shared::BehaviorStatus::Running;
	}

} // namespace srv
