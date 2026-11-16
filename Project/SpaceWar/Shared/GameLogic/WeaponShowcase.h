#pragma once
// ★ Shared 안에서는 상대 경로로 include 한다 (PhysicsCalculator.h 머리 주석)
#include <cstddef>
#include <cstdint>
#include "../Vec3d.h"
#include "../GameObject/Weapon.h"

// ============================================================
//  Shared/GameLogic/WeaponShowcase.h — 총 진열 6개의 자리 · 충돌체 (2026-11-15)
//
//  게임 맵 스폰 앞 허공에 띄운 총 6개(2026-11-14 사용자 «씬 처음 허공에 1인칭 모델과 3인칭 모델 총 6개 띄우는거까지만»)의
//  자리 · 축을 클라(그리기 · 이동 예측 충돌 — Engine::BuildWeaponShowcase)와 서버(이동 판정 충돌 — main.cpp
//  SetupWeaponShowcase)가 같은 식으로 구한다. 2026-11-15 줍기 — 이 자리가 줍는 출처다(주워도 진열은 그대로, WeaponPickup.h).
//  사용자 지시(2026-11-15): «충돌체도 같이 적용은 해줘봐».
//  ★ 충돌체 = 그 총의 상자 묶음(WeaponSpec::colliderBoxes — 1인칭 메시를 덮는 상자 3 ~ 6개, Weapon.cpp). 1인칭 · 3인칭 진열 모두 같은 것.
//    2026-11-15 처음엔 3인칭 glb 의 COL_<총>(삼각형 5만)을 썼으나, 부딪히면 프레임이 10 까지 떨어져(사용자) 상자로 바꿨다.
//  ★ 이동 충돌은 서버 권위(노션 「충돌 처리 ②」 B안) — 클라에만 넣으면 예측과 판정이 갈려 총 근처에서 되감긴다.
//    그래서 자리 계산을 여기 하나에 두고 양쪽이 부른다. 좌표는 PlayerMovement::Spawn 으로 세운 스폰 상태 기준(양쪽 첫 상태와 같다).
//  ★ 임시 — 바닥 총(WeaponPickup, 예정)이 생기면 이 진열은 지운다.
// ============================================================

namespace Shared {

	struct MoveWorld;
	class CollisionWorld;

	inline constexpr double kWeaponShowcaseDistance = 5.0;            // 스폰에서 앞으로 (m)
	inline constexpr double kWeaponShowcaseSpacing = 1.4;             // 총 종류 사이 (m) — 기관총 길이 1.05 m 보다 넓게
	inline constexpr double kWeaponShowcaseHeightFirstPerson = 1.6;   // 지면 위 (m) — 윗줄 1인칭 모델
	inline constexpr double kWeaponShowcaseHeightThirdPerson = 1.0;   // 지면 위 (m) — 아랫줄 3인칭 모델
	inline constexpr size_t kWeaponShowcaseSlotCount = kWeaponTypeCount * 2;   // 종류 × (1인칭 · 3인칭)

	// 진열 한 자루 — 월드 좌표(행성 중심 포함, MoveState::position 과 같은 공간).
	//  축 = 총 glb 의 +X · +Y · +Z 가 월드에서 향하는 쪽(단위). 총 glb 는 +Y 위 · +Z 총구 · ±X 옆면이다.
	//  옆면(+X)이 스폰 쪽(−facing)을, 총구(+Z)가 오른쪽(up × facing)을 본다 — (−facing) × up = up × facing 이라 뒤집히지 않는다.
	struct WeaponShowcaseSlot
	{
		WeaponType type = WeaponType::None;
		bool       firstPerson = false;   // true = 윗줄 1인칭 모델 · false = 아랫줄 3인칭 모델
		Vec3d      position{};
		Vec3d      axisX{};
		Vec3d      axisY{};
		Vec3d      axisZ{};
	};

	// 스폰 자리 · 처음 방향(클라 controller.Spawn · 서버 PlayerMovement::Spawn 에 넘기는 값)에서 6자리를 구한다.
	//  순서: 종류(권총 · 레이저 라이플 · 기관총 — 왼쪽부터) × (1인칭 · 3인칭). 가운데 열이 레이저 라이플.
	//  높이는 그 열 자리의 지면(MoveWorld::SurfaceHeight — 착륙장 · 길 포함) 위로 잰다.
	void LayoutWeaponShowcase(const MoveWorld& world, const Vec3d& spawnPoint, const Vec3d& spawnFacing,
		WeaponShowcaseSlot (&out)[kWeaponShowcaseSlotCount]);

	// 6자리에 충돌체를 넣는다 — 종류마다 상자 묶음 메시 하나(CollisionWorld::AddBoxCollider) + 자리마다 인스턴스.
	//  맵 공간 = 월드 − world.center. 넣은 인스턴스 수를 돌려준다. ★ 부른 쪽이 collision.Finalize() 를 다시 부른다.
	size_t AddWeaponShowcaseColliders(CollisionWorld& collision, const MoveWorld& world,
		const WeaponShowcaseSlot (&slots)[kWeaponShowcaseSlotCount]);

} // namespace Shared
