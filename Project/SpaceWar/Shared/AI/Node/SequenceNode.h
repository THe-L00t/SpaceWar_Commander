#pragma once
#include "CompositeNode.h"

// AND. 앞에서부터 실행하다가 하나라도 실패하면 거기서 멈춘다.
//   예) [적이 있다] -> [사거리 안이다] -> 공격 시작
namespace Shared {

	class SequenceNode : public CompositeNode
	{
	public:
		~SequenceNode() override;

		BehaviorStatus Tick(BehaviorContext& ctx) const override;
	};

} // namespace Shared
