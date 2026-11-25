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
//    · 몸     : 그 몸의 충돌체(2026-11-25). 몬스터 = glb 의 COL_ 부위 메시(CollisionWorld::LoadBodyPack)를 몸 자세로 씌운 것
//               (RaycastPosed — 정적 충돌체와 같은 BVH 판정), 캐릭터(플레이어 · 게임 맵 NPC — 충돌체 파일 없음) = 이동 캡슐(RayCapsule).
//               자세 = BodyPose(몸통 중심 · 그 자리 위) — 클라 CharacterTransform 과 같은 축, 발 = 몸통 중심 − kGroundOffset.
//               ★ 2026-11-25 사용자 «예전부터 구로 판정하라 한적 없고 충돌체 이용하라 했는데» — 09-26 부터 쓰던 몸통 중심 구(1.2 m)를 지웠다.
//    · 충돌체 : CollisionWorld::Raycast — 건물 · 바위 · 소품 · 나무 · 총 진열 상자(지금 맵의 정적 충돌체)
//    · 지형   : MoveWorld 의 지면(게임 맵 · 예시 행성 모두 — 이동 판정이 보는 땅과 같다)
//  ★ 상태를 바꾸지 않는다 — 부르는 쪽 스레드에서 그대로 부른다(클라 메인 · 서버 틱). CPU 계산이다.
// ============================================================

namespace Shared {

	struct MoveWorld;
	class CollisionWorld;

	// 몸 하나 — 몸통 중심(월드 좌표) · 그 몸의 충돌체. id 는 부르는 쪽 번호(플레이어 · NPC 번호 등) — 이 함수는 보지 않고 돌려주기만 한다.
	//  body = 몸 충돌체 묶음(CollisionWorld::LoadBodyPack — 몬스터). nullptr 이면 캐릭터 몸 = 이동 캡슐.
	//  가리키는 묶음은 부르는 동안 살아 있어야 한다(맵이 들고 있다 — 서버 ServerWorld::npcBody · 클라 PlanetDebugScene::NpcBody).
	struct ShotTarget
	{
		Vec3d                 center{};
		uint32_t              id = 0;
		const CollisionWorld* body = nullptr;
	};

	// 몸 자세 — 몸 충돌체 메시 공간(glTF · 원점 발밑) → 맵 공간 행 벡터 3 × 4(CollisionInstance::xf 규약).
	//  행 = 오른쪽(위 × 앞) · 위 · 앞(월드 +Z 를 그 자리 수평면에 눕힌 것), 이동 = 몸통 중심 − 위 × kGroundOffset − mapCenter.
	//  클라 CharacterTransform(Engine.cpp) × PlanetDebugScene::SetNpcModel 의 발 내림과 같은 식 — 화면에 그린 몬스터와 겹친다.
	//  (2026-11-25 — C 키 와이어(ColliderDebugView::AddBody)도 이 값으로 그린다)
	void BodyPoseTransform(const Vec3d& center, const Vec3d& up, const Vec3d& mapCenter, float outXf[12]);

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
		double distance = 0.0;   // 원점에서 닿은 곳까지(m). Body 면 그 몸의 충돌체에 닿은 점. None 이면 maxDistance
		size_t target = 0;       // Body 일 때 targets 번호
		// Body 일 때 맞은 부위 — 몸 묶음의 메시 번호(body->Mesh(part).name = COL_ 뒤 이름 · head · chest …). 캐릭터 캡슐이면 kNoPart.
		static constexpr uint32_t kNoPart = 0xFFFFFFFFu;
		uint32_t part = kNoPart;
		//  2026-11-25 — 예전 closest(«레이가 몸통 중심에 가장 가까워지는 거리» — 구 대역 때 클라가 겨누던 점)는 지웠다.
		//   이제 충돌체에 실제로 닿은 점(distance)을 겨눈다.
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
