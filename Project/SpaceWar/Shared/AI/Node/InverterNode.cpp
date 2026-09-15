#include "InverterNode.h"
#include "../BehaviorContext.h"

namespace Shared {

	InverterNode::~InverterNode() = default;

	BehaviorStatus InverterNode::Tick(BehaviorContext& ctx) const
	{
		const BehaviorStatus status = child->Tick(ctx);

		if (status == BehaviorStatus::Success)
		{
			return BehaviorStatus::Failure;
		}

		if (status == BehaviorStatus::Failure)
		{
			return BehaviorStatus::Success;
		}

		return BehaviorStatus::Running;
	}

} // namespace Shared
