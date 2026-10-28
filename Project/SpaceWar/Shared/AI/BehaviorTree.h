#pragma once

// ============================================================
//  Shared/AI/BehaviorTree.h
//  아키텍처 명세서 6.8.2 — NPC 의 행동을 결정하는 객체.
//
//  Shared 에 두는 이유 (명세 6.13)
//    담당이 클라이언트에서 서버로 넘어가도 이어서 돌아야 하므로
//    서버와 클라이언트가 «같은» Behavior Tree 를 함께 쓴다.
//
//  여기서 정하는 것 : 이동 · 추격 대상 · 공격 시작
//  서버만 정하는 것 : 피해 · 체력 · 무력화 · 보상 · 담당 전환
//
//  ★ 지금은 틀뿐이다. 노드 종류(Sequence·Selector·Action 등)와
//    실행(Tick)은 아직 없다.
// ============================================================

namespace Shared {

	class BehaviorTree
	{
	public:
		BehaviorTree() = default;
		virtual ~BehaviorTree();
	};

} // namespace Shared
