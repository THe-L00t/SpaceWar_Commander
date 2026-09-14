#pragma once
#include "DecoratorNode.h"

// 자식의 성공과 실패를 뒤집는다. Running 은 그대로 올린다.
// 조건 노드 하나로 "있다" 와 "없다" 를 모두 쓰기 위한 것이다.
namespace Shared {

	class InverterNode : public DecoratorNode
	{
	public:
		~InverterNode() override;

		BehaviorStatus Tick(BehaviorContext& ctx) const override;
	};

} // namespace Shared
