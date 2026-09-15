#pragma once
#include "LeafNode.h"

// 판정만 하는 노드. 즉시 Success 나 Failure 를 돌려주고 Running 을 돌려주지 않는다.
//
// 게임별 판정(적이 있는가·사거리 안인가·체력이 얼마인가)은 이 클래스를 상속해 만든다.
namespace Shared {

	class ConditionNode : public LeafNode
	{
	public:
		~ConditionNode() override;
	};

} // namespace Shared
