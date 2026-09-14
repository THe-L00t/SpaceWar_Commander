#pragma once
#include "Shared/AI/Node/ConditionNode.h"

// 사거리 안에 플레이어가 있는가.
//
// 판정만 한다. 컨텍스트를 읽기만 하고 아무것도 바꾸지 않는다.
namespace srv {

	class HasTargetCondition : public Shared::ConditionNode
	{
	public:
		explicit HasTargetCondition(float range) : detectRange(range) {}
		~HasTargetCondition() override;

		Shared::BehaviorStatus Tick(Shared::BehaviorContext& ctx) const override;

	private:
		float detectRange = 0.0f;   // m
	};

} // namespace srv
