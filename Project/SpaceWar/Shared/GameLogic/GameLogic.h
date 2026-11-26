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
//    ★ 2026-11-25 — 몸은 그 몸의 충돌체(몬스터 COL_ · 캐릭터 이동 캡슐, ShotTrace.h)로 판정한다. 히트박스 반지름 kHitRadius(1.2 m 구)는 지웠다.
//    ★ 2026-11-15 — 클라는 여전히 이 파일을 포함하지 않는다. 한 발의 레이(몸 · 충돌체 · 지형)는
//      Shared/GameLogic/ShotTrace(TraceShot)가 계산하고, 클라 겨눈 점(Engine::AimDirection)과 서버 판정(ResolveFire)이 그 함수를 같이 부른다.
// ============================================================

namespace Shared {

	// ── 전투 규칙 ───────────────────────────────────────────
	//  교수님 지시(2026-08-26 회의록): 좌클릭 사격 / 피격 시 HP 감소 /
	//  HP 0 이면 시작 위치로 텔레포트(HP 만땅) / NPC 는 10초 뒤 리스폰.
	//  ★ 2026-11-15 — 사격 판정은 든 총의 표(Shared/GameObject/Weapon.cpp kSpecs: 발 간격 · 피해 · 사거리)를 쓴다.
	//    아래 kShotDamage · kFireRange · kFireInterval 은 더 이상 판정에 쓰지 않는다(11-15 까지 권총 줄과 같던 값 — 기록으로 남김.
	//    사거리는 11-16 부터 표가 130 m).
	inline constexpr float kMaxHealth = 100.0f;    // 플레이어 시작·회복 체력 (Shared::Player 기본값 — Player.cpp)
	// 2026-11-27 몬스터(NPC) 시작·재등장 체력 (사용자 «몬스터는 200정도로 해줘») — Shared::NPC 기본값(NPC.cpp), 서버 NpcWorld 가 이 값으로 세운다.
	//  이 날 전까지 NPC 도 kMaxHealth(100)였다. 권총 25 기준 4발 → 8발.
	inline constexpr float kNpcMaxHealth = 200.0f;
	inline constexpr float kShotDamage = 25.0f;    // 한 발 (4발이면 쓰러진다)
	inline constexpr float kFireRange = 300.0f;    // 사거리 (m). 지평선 125m 보다 넉넉히
	inline constexpr float kFireInterval = 0.2f;   // 연사 제한 (s)
	inline constexpr float kNpcRespawnDelay = 10.0f;   // NPC 재등장까지 (s)

	// 발사 원점이 서버가 아는 위치에서 이만큼 벌어지면 서버 위치로 바꿔 쏜다 (m).
	// 3인칭이라 총구가 몸통 중심과 완전히 같지는 않으므로 여유를 둔다.
	inline constexpr float kMuzzleTolerance = 3.0f;

	// ★ 몸 히트박스 = 그 몸의 충돌체(명세 6.3 Collision Box 자리) — 2026-11-25 부터 몬스터 COL_ 부위 · 캐릭터 이동 캡슐.
	//   (09-26 ~ 11-25 는 «반지름 1.2 m 구» 대역이었다 — 사용자 지시가 아니었다. 사용자 «충돌체 이용하라 했는데»)
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
