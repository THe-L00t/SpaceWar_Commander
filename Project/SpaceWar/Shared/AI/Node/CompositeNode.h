#pragma once
#include <vector>
#include "BehaviorNode.h"

// 자식 여러 개를 거느리는 노드. 자식을 어떤 순서와 조건으로 굴릴지는 파생이 정한다.
namespace Shared {

	class CompositeNode : public BehaviorNode
	{
	public:
		~CompositeNode() override;

		// 자식을 소유하지 않는다. 노드의 수명은 트리가 관리한다.
		void AddChild(BehaviorNode* child) { children.push_back(child); }

		uint16_t ChildCount() const { return static_cast<uint16_t>(children.size()); }

	protected:
		std::vector<BehaviorNode*> children;
	};

} // namespace Shared
