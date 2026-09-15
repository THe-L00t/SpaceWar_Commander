#pragma once
#include "BehaviorNode.h"

// 자식 하나를 거느리고 그 결과나 실행 조건을 변형하는 노드.
// 자식이 하나뿐이라는 것을 타입으로 못박는다 — Composite 와 섞지 않는 이유다.
namespace Shared {

	class DecoratorNode : public BehaviorNode
	{
	public:
		~DecoratorNode() override;

		// 자식을 소유하지 않는다. 노드의 수명은 트리가 관리한다.
		void SetChild(BehaviorNode* node) { child = node; }

		const BehaviorNode* Child() const { return child; }

	protected:
		BehaviorNode* child = nullptr;
	};

} // namespace Shared
