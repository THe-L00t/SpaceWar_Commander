#pragma once
#include "BehaviorNode.h"

// 자식이 없는 노드. 트리의 끝이며 실제로 판정하거나 행동한다.
namespace Shared {

	class LeafNode : public BehaviorNode
	{
	public:
		~LeafNode() override;
	};

} // namespace Shared
