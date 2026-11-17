#pragma once
// ★ Shared 안에서는 상대 경로로 include 한다 (PhysicsCalculator.h 머리 주석)
#include <cstddef>
#include <cstdint>
#include "../Vec3d.h"

// ============================================================
//  Shared/GameLogic/ShotTrace.h — 히트스캔 한 발의 레이 (2026-11-15)
//
//  사용자 지시(2026-11-15): «레이 하나 쏘는거잖아 역으로, 그거 충돌체만 더 포함해서 처리하면 되는거 아니야?» ·
//    «규칙 벗어난거는 게임 로직을 shared에 뺄 수 있을지 확인해줘봐» · «벽에 막히는건 당연히 막혀야지. 진행하고 노션에 정리해줘»
//
//  ★ 클라 · 서버가 «같은 계산» 을 쓴다(명세 13절 — 클라 예측 · 서버 권위가 같은 계열의 계산. 이동 B안과 같은 원칙).
//    · 클라 Engine::AimDirection — 카메라 눈에서 조준선(화면 가운데)으로. 처음 닿는 곳 = 겨눈 점(크로스헤어 맞추기)
//    · 서버 ResolveFire          — 총구에서 겨눈 방향으로. 처음 닿는 것이 몸이면 명중, 충돌체 · 지형이면 막힘
//  ★ 닿는 것 세 가지 — 가장 가까운 것 하나를 돌려준다.
//    · 몸     : 몸통 중심의 구(반지름 Shared::kHitRadius — GameLogic.h). 명세 6.3 Collision Box 가 생기기 전까지의 히트박스.
//    · 충돌체 : CollisionWorld::Raycast — 건물 · 바위 · 소품 · 나무 · 총 진열 상자(지금 맵의 정적 충돌체)
//    · 지형   : MoveWorld 의 지면(게임 맵 · 예시 행성 모두 — 이동 판정이 보는 땅과 같다)
//  ★ 상태를 바꾸지 않는다 — 부르는 쪽 스레드에서 그대로 부른다(클라 메인 · 서버 틱). CPU 계산이다.
// ============================================================

namespace Shared {

	struct MoveWorld;

	// 몸 하나 — 몸통 중심(월드 좌표). id 는 부르는 쪽 번호(플레이어 · NPC 번호 등) — 이 함수는 보지 않고 돌려주기만 한다.
	struct ShotTarget
	{
		Vec3d    center{};
		uint32_t id = 0;
	};

	enum class ShotHitKind : uint8_t
	{
		None,       // 사거리 안에 아무것도 없다
		Body,       // 몸(targets 중 하나)
		Collider,   // 정적 충돌체
		Terrain,    // 지면
	};

	struct ShotHit
	{
		ShotHitKind kind = ShotHitKind::None;
		double distance = 0.0;   // 원점에서 닿은 곳까지(m). Body 면 그 구에 들어가는 거리. None 이면 maxDistance
		size_t target = 0;       // Body 일 때 targets 번호
		double closest = 0.0;    // Body 일 때 — 레이가 그 몸통 중심에 가장 가까워지는 거리(클라가 이 점을 겨눈다)
	};

	struct ShotOptions
	{
		// 이보다 앞은 보지 않는다(m). 클라: 카메라와 캐릭터 사이(어깨 뒤)를 겨누지 않게 «조준선이 총구 옆을 지나는 거리».
		double minDistance = 0.0;
		// 지면 아래로 이만큼 들어가야 지형에 막힌 것으로 본다(m). 서버: 0.5 — 땅을 스치듯 지나는 발을 봐준다(예전 TerrainBlocks 와 같은 값).
		//  클라 겨눈 점은 0 — 땅을 겨누면 실제 지면을 겨눈다.
		double terrainTolerance = 0.0;
	};

	inline constexpr double kShotTerrainStep = 1.0;          // m — 지면 아래로 들어갔는지 보는 간격
	inline constexpr int    kShotTerrainRefine = 10;         // 처음 들어간 칸을 반씩 10 번(1 m → 약 1 mm)
	inline constexpr double kShotServerTerrainTolerance = 0.5;   // 서버 판정이 쓰는 ShotOptions::terrainTolerance

	// origin 에서 direction(길이 상관없음)으로 maxDistance 까지 — 가장 먼저 닿는 몸 · 충돌체 · 지형.
	ShotHit TraceShot(const MoveWorld& world, const Vec3d& origin, const Vec3d& direction, double maxDistance,
		const ShotTarget* targets, size_t targetCount, const ShotOptions& options = {});

} // namespace Shared
