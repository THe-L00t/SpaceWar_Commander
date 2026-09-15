#pragma once
#include "Shared/AI/Node/ActionNode.h"

// 제자리 대기. 아무것도 결정하지 않는다.
//
// Selector 의 마지막 갈래라 «항상 되는 행동» 이어야 한다.
// 그래서 실패를 돌려주지 않는다.
namespace srv {

	class IdleAction : public Shared::ActionNode
	{
	public:
		~IdleAction() override;

		Shared::BehaviorStatus Tick(Shared::BehaviorContext& ctx) const override;
	};

} // namespace srv
