#include "SelectorNode.h"
#include "../BehaviorContext.h"

namespace Shared {

	SelectorNode::~SelectorNode() = default;

	BehaviorStatus SelectorNode::Tick(BehaviorContext& ctx) const
	{
		BehaviorNodeState& state = ctx.nodeStates[index];
		const uint16_t count = ChildCount();

		// 지난 틱에 멈춘 자식부터 이어간다.
		for (uint16_t i = state.runningChild; i < count; ++i)
		{
			const BehaviorStatus status = children[i]->Tick(ctx);

			if (status == BehaviorStatus::Running)
			{
				state.runningChild = i;
				return BehaviorStatus::Running;
			}

			if (status == BehaviorStatus::Success)
			{
				state.runningChild = 0;
				return BehaviorStatus::Success;
			}
		}

		state.runningChild = 0;
		return BehaviorStatus::Failure;
	}

} // namespace Shared
