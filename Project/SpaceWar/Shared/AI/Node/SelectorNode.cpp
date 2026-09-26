#include "SelectorNode.h"
#include "../BehaviorContext.h"

namespace Shared {

	SelectorNode::~SelectorNode() = default;

	BehaviorStatus SelectorNode::Tick(BehaviorContext& ctx) const
	{
		BehaviorNodeState& state = ctx.nodeStates[index];
		const uint16_t count = ChildCount();

		// ★ 매 틱 «맨 앞부터» 다시 본다 (반응형 Selector)
		//   Running 이던 자식부터 이어가면 우선순위가 굳어버린다.
		//   실제로 그렇게 만들었다가, 대기(Running)에 한 번 걸린 NPC 가
		//   플레이어가 코앞에 와도 영영 추격 갈래를 다시 보지 않는 버그가 났다.
		//   우선순위 분기는 앞 갈래의 조건을 매번 다시 확인해야 한다.
		for (uint16_t i = 0; i < count; ++i)
		{
			const BehaviorStatus status = children[i]->Tick(ctx);

			if (status == BehaviorStatus::Running)
			{
				state.runningChild = i;   // 이번 틱에 어느 갈래였는지 기록만 한다
				return BehaviorStatus::Running;
			}

			if (status == BehaviorStatus::Success)
			{
				state.runningChild = i;
				return BehaviorStatus::Success;
			}
		}

		state.runningChild = 0;
		return BehaviorStatus::Failure;
	}

} // namespace Shared
