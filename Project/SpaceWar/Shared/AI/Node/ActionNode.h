#pragma once
#include "LeafNode.h"

// 실제 행동. 여러 틱이 걸리면 Running 을 돌려준다.
//
// ★ NPC 를 직접 고치지 않는다. 이번 틱의 결정을 ctx.decision 에 적어두면
//   메인 스레드가 상태 커밋에서 반영한다. 공격은 "시작" 만 적고 피해는 서버가 판정한다.
//
// 게임별 행동(이동·추격·공격 시작·대기)은 이 클래스를 상속해 만든다.
namespace Shared {

	class ActionNode : public LeafNode
	{
	public:
		~ActionNode() override;
	};

} // namespace Shared
