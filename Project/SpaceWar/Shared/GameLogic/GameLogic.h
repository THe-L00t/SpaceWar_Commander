#pragma once

// ============================================================
//  Shared/GameLogic/GameLogic.h — 아키텍처 명세서 8절
//
//  게임의 규칙과 동작. GameObject 의 상태를 바꾸고 게임플레이 판정을 수행한다.
//  Renderer 의 DirectX 객체를 직접 만지지 않는다 (명세 18절 원칙 1).
//
//  ★ Shared 에 두되 «지금은 서버만» 쓴다 — 2026-09-18 사용자 결정
//    판정은 전부 서버 권위다 (명세 18절 원칙 4). 규칙 코드를 한 곳에 두면,
//    나중에 클라가 예측·보정을 할 때 같은 규칙을 그대로 가져다 쓸 수 있다.
//    ※ 노션 문서에는 이 배치의 근거가 없다. 「멀티스레딩 분류 명세서」는 클라이언트만 다룬다.
//  ★ 클라에서의 실행 위치 — 메인 스레드, 계산만 잡으로 나누고 반영은 메인 (멀티스레딩 5장)
//    객체끼리 읽기→계산→쓰기가 얽히므로 결과 배열을 모아 ③ 상태 커밋에서 한 번에 반영한다.
//  ★ 전투 수치는 여기 한 곳에만 둔다 (2026-09-24)
//    서버가 판정에 쓰고, 클라는 이 파일을 포함하지 않는다. 클라가 아는 체력은
//    서버가 보내준 값뿐이다 — 규칙을 양쪽에 복사해 두면 반드시 갈라진다.
// ============================================================

namespace Shared {

	// ── 전투 규칙 ───────────────────────────────────────────
	//  교수님 지시(2026-08-26 회의록): 좌클릭 사격 / 피격 시 HP 감소 /
	//  HP 0 이면 시작 위치로 텔레포트(HP 만땅) / NPC 는 10초 뒤 리스폰.
	inline constexpr float kMaxHealth = 100.0f;    // 시작·회복 체력
	inline constexpr float kShotDamage = 25.0f;    // 한 발 (4발이면 쓰러진다)
	inline constexpr float kFireRange = 300.0f;    // 사거리 (m). 지평선 125m 보다 넉넉히
	inline constexpr float kHitRadius = 1.2f;      // 히트박스 반지름 (m)
	inline constexpr float kFireInterval = 0.2f;   // 연사 제한 (s)
	inline constexpr float kNpcRespawnDelay = 10.0f;   // NPC 재등장까지 (s)

	// 발사 원점이 서버가 아는 위치에서 이만큼 벌어지면 서버 위치로 바꿔 쏜다 (m).
	// 3인칭이라 총구가 몸통 중심과 완전히 같지는 않으므로 여유를 둔다.
	inline constexpr float kMuzzleTolerance = 3.0f;

	// ★ 히트박스는 명세 6.3 의 Collision Box 자리다
	//   아직 Collision Box 가 없어 반지름 하나로 대신한다. 더미 큐브가 2m 라
	//   외접 반지름은 1.73m 지만, 맞추기 쉬우면 판정이 헐거워지므로 1.2m 로 둔다.
	class GameLogic
	{
	public:
		GameLogic();
		~GameLogic();

		// 남은 체력. 0 밑으로는 내려가지 않는다.
		static float ApplyDamage(float health, float damage);

		// 쓰러졌는가.
		static bool IsDown(float health);
	};

} // namespace Shared
