#pragma once
#include <cstdint>
#include <vector>
#include "../Vec3.h"

// ============================================================
//  Shared/AI/BehaviorContext.h
//  노드가 갖지 않는 것을 전부 여기에 둔다. NPC 하나당 하나다.
//
//  ★ 노드는 불변 공유물이고 바뀌는 것은 전부 여기 있다.
//    담당 NPC 의 행동 트리는 워커 잡으로 돌아가므로(멀티스레딩 명세 10장 M7)
//    여러 워커가 같은 노드를 동시에 실행한다. 노드에 상태를 두면 그 순간 경쟁이다.
// ============================================================

namespace Shared {

	// 노드 하나의 실행 상태. 노드 종류와 상관없이 크기가 같다.
	struct BehaviorNodeState
	{
		uint16_t runningChild = 0;   // Composite : 다음 틱에 이어갈 자식 번호
		uint16_t counter = 0;        // Decorator : 반복 횟수
		float    timer = 0.0f;       // Decorator : 쿨다운 잔여 시간 (초)
	};

	// 이번 틱에 트리가 내린 결정.
	//
	// ★ 노드는 NPC 를 직접 고치지 않는다. 여기에 적어두면 메인 스레드가
	//   상태 커밋 단계에서 반영한다 (멀티스레딩 명세 6장 3번 · 7장).
	//   피해와 체력은 서버가 판정하므로 공격은 "시작" 만 적는다 (명세서 6.13).
	struct BehaviorDecision
	{
		bool     hasMoveTarget = false;
		Vec3     moveTarget{};
		uint32_t chaseTarget = 0;      // 대상 핸들. 0 이면 없음
		bool     attackStart = false;
	};

	struct BehaviorContext
	{
		uint32_t npc = 0;      // NPC 핸들 (index + generation). 포인터를 쓰지 않는다
		float    dt = 0.0f;    // 초

		// ── 읽기용 월드 스냅샷 ──────────────────────────────
		//  이번 틱에 노드가 볼 수 있는 것. 노드는 여기서 읽기만 하고
		//  게임 객체를 직접 찾아가지 않는다.
		Vec3     npcPos{};                   // 이 NPC 의 현재 위치
		bool     hasNearestPlayer = false;
		uint32_t nearestPlayerId = 0;
		Vec3     nearestPlayerPos{};
		float    nearestPlayerDist = 0.0f;   // m

		// 노드 수만큼 트리를 만들 때 미리 잡는다.
		// Tick 중에는 늘리지 않는다 — 잡 안에서 힙 할당을 하지 않는다.
		std::vector<BehaviorNodeState> nodeStates;

		BehaviorDecision decision{};

		// 담당이 넘어올 때 여기서 다시 시작한다.
		// 진행 지점을 복원하지 않고 큰 상태부터 다시 잡는다 (명세서 6.13).
		void Reset()
		{
			for (BehaviorNodeState& state : nodeStates)
			{
				state = BehaviorNodeState{};
			}
			decision = BehaviorDecision{};
		}

		// 노드 수만큼 상태 슬롯을 잡는다. 트리를 붙일 때 한 번만 부른다.
		void Allocate(uint16_t nodeCount)
		{
			nodeStates.assign(nodeCount, BehaviorNodeState{});
		}
	};

} // namespace Shared
