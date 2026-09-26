#pragma once
#include "CompositeNode.h"

// OR. 앞에서부터 시도하다가 하나라도 성공하면 거기서 멈춘다. 우선순위 분기다.
//   예) 전투 -> 안 되면 경계 -> 안 되면 대기
namespace Shared {

	class SelectorNode : public CompositeNode
	{
	public:
		~SelectorNode() override;

		BehaviorStatus Tick(BehaviorContext& ctx) const override;
	};

} // namespace Shared
