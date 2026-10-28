#pragma once

// ============================================================
//  Shared/AI/BehaviorStatus.h
//  행동 트리 노드가 한 틱에 돌려주는 결과. 이 규약이 트리 전체를 굴린다.
// ============================================================

namespace Shared {

	enum class BehaviorStatus
	{
		Success,   // 끝났고 성공
		Failure,   // 끝났고 실패
		Running    // 아직 하는 중. 다음 틱에 이어서 한다
	};

} // namespace Shared
