#include "HasTargetCondition.h"
#include "Shared/AI/BehaviorContext.h"

namespace srv {

	HasTargetCondition::~HasTargetCondition() = default;

	Shared::BehaviorStatus HasTargetCondition::Tick(Shared::BehaviorContext& ctx) const
	{
		if (!ctx.hasNearestPlayer)
		{
			return Shared::BehaviorStatus::Failure;
		}

		return ctx.nearestPlayerDist <= detectRange
			? Shared::BehaviorStatus::Success
			: Shared::BehaviorStatus::Failure;
	}

} // namespace srv
