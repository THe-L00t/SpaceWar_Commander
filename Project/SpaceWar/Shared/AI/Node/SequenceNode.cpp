#include "SequenceNode.h"
#include "../BehaviorContext.h"

namespace Shared {

	SequenceNode::~SequenceNode() = default;

	BehaviorStatus SequenceNode::Tick(BehaviorContext& ctx) const
	{
		BehaviorNodeState& state = ctx.nodeStates[index];
		const uint16_t count = ChildCount();

		// ★ 매 틱 «맨 앞부터» 다시 본다 (반응형 Sequence)
		//   Running 이던 자식부터 이어가면 앞에 있는 조건을 건너뛴다.
		//   [적이 사거리 안] -> [추격] 에서 추격이 Running 이라고 조건을 안 보면,
		//   플레이어가 멀리 달아나도 계속 쫓는다. 조건은 매번 다시 확인해야 한다.
		//
		//   여러 틱에 걸쳐 «중단되면 안 되는» 행동이 생기면 그때
		//   기억하는 변형(SequenceMemory)을 형제 클래스로 따로 만든다.
		for (uint16_t i = 0; i < count; ++i)
		{
			const BehaviorStatus status = children[i]->Tick(ctx);

			if (status == BehaviorStatus::Running)
			{
				state.runningChild = i;   // 이번 틱에 어디까지 갔는지 기록만 한다
				return BehaviorStatus::Running;
			}

			if (status == BehaviorStatus::Failure)
			{
				state.runningChild = i;
				return BehaviorStatus::Failure;
			}
		}

		state.runningChild = 0;
		return BehaviorStatus::Success;
	}

} // namespace Shared
