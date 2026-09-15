#pragma once
#include <cstdint>
#include "../BehaviorStatus.h"

// ============================================================
//  Shared/AI/Node/BehaviorNode.h
//  모든 행동 트리 노드의 뿌리. 자식 수로 세 갈래가 갈린다.
//
//      BehaviorNode
//      ├── CompositeNode   자식 N개 — 흐름을 분기한다
//      ├── DecoratorNode   자식 1개 — 결과를 변형한다
//      └── LeafNode        자식 없음 — 판정하거나 행동한다
//
//  설계 근거는 노션 「행동 트리 노드 설계」 3장.
// ============================================================

namespace Shared {

	struct BehaviorContext;

	class BehaviorNode
	{
	public:
		virtual ~BehaviorNode();

		// ★ const 다. 노드는 자기 상태를 바꾸지 않는다.
		//   바뀌는 것은 전부 ctx 안에 있다 — 여러 워커가 같은 노드를 동시에 실행하기 때문이다.
		virtual BehaviorStatus Tick(BehaviorContext& ctx) const = 0;

		// 자기 실행 상태 슬롯의 번호. 트리를 만들 때 부여한다.
		void SetIndex(uint16_t i) { index = i; }
		uint16_t Index() const { return index; }

	protected:
		uint16_t index = 0;
	};

} // namespace Shared
