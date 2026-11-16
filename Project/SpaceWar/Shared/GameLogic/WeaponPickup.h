#pragma once
// ★ Shared 안에서는 상대 경로로 include 한다 (PhysicsCalculator.h 머리 주석)
#include <cstddef>
#include <cstdint>
#include "../Vec3d.h"
#include "../GameObject/Weapon.h"

// ============================================================
//  Shared/GameLogic/WeaponPickup.h — 총 줍기 · 떨구기 · 손에 든 총 자리 (2026-11-15)
//
//  사용자 지시(2026-11-15): «주우면 애니메이션 시에 적절한 위치에 총을 고정시키고, 쏠 수 있도록 해줘. 줍기, 떨구기 둘 다
//    구현하면 되는거야. 이때 공중에 있는것을 줍고, 떨구면 땅바닥에 떨어지도록. 공중에 있는 총은 사라지지 않게»
//  앞선 결정(2026-11-14): 한 번에 한 자루 · 버리기 키만(죽어도 안 떨군다) · 자기 총 1인칭 · 남의 총 3인칭 ·
//    바닥 총은 떨어져 눕기만(튕기고 구르기는 나중, 물리 엔진).
//
//  ★ 서버 권위(명세 18절 원칙 4) — 누가 무엇을 들었는지 · 바닥 총 목록은 서버가 정한다(Server/main.cpp).
//    클라는 줍기 · 떨구기를 «요청» 하고, 서버가 보낸 WeaponEquip · GroundWeaponSpawn · GroundWeaponRemove 로 그린다.
//    서버 없이(오프라인) 켜면 클라가 같은 함수로 혼자 처리한다.
//  ★ 이 파일의 계산은 서버(떨군 자리 · 줍기 거리 검사)와 클라(손 자리 그리기 · 총구 · 오프라인)가 같이 쓴다.
//  ★ 공중 진열 6개(WeaponShowcase.h)는 줍는 «출처» 다 — 주워도 사라지지 않는다(같은 종류를 하나 더 받는다).
// ============================================================

namespace Shared {

	struct MoveWorld;

	// 손에 든 총 자리 — 캐릭터 몸 공간(+X 오른쪽 · +Y 위 · +Z 앞, 원점 = 몸통 중심 = 발 위 kGroundOffset 1 m).
	//  ★ 지금 캐릭터는 뼈대 없는 OBJ(팔을 내린 자세, 손은 지면 위 0.8 ~ 0.9 m · 옆 ±0.34 m — 2026-11-15 정점에서 잼)라
	//    손 뼈에 붙일 수 없다. 총 원점(손잡이)을 오른손 자리에 두고 총구를 몸 앞(+Z)으로 향하게 고정한다.
	//    스켈레탈 애니메이션이 들어오면(명세 §14 Animator) 이 자리 대신 오른손 뼈 행렬을 쓴다.
	inline constexpr double kHeldWeaponRight = 0.34;     // m — 오른손 옆 자리
	inline constexpr double kHeldWeaponUp = -0.14;       // m — 몸통 중심 아래(지면 위 0.86 m)
	inline constexpr double kHeldWeaponForward = 0.10;   // m — 몸 앞으로 조금

	inline constexpr double kWeaponPickupRange = 2.0;    // m — 몸통 중심에서 총 원점까지 이 안이면 줍는다(클라가 고르는 거리)
	inline constexpr double kWeaponPickupSlack = 1.0;    // m — 서버는 이만큼 더 봐준다(클라 예측 · 지연으로 위치가 조금 다르다)
	inline constexpr size_t kMaxGroundWeapons = 32;      // 맵마다 바닥 총 상한 — 넘으면 가장 오래된 것부터 지운다(진열은 무한히 주울 수 있어서)

	// 총 하나의 자세 — 월드 좌표(행성 중심 포함). 축 = 총 glb 의 +X(옆) · +Y(위) · +Z(총구)가 월드에서 향하는 쪽, Z = X × Y.
	struct WeaponPose
	{
		Vec3d position{};
		Vec3d axisX{ 1.0, 0.0, 0.0 };
		Vec3d axisY{ 0.0, 1.0, 0.0 };
		Vec3d axisZ{ 0.0, 0.0, 1.0 };
	};

	// 떨군 총 — 손에서 떨어지기 시작하는 자세 · 땅에 누운 자세 · 떨어지는 시간(s).
	struct WeaponDrop
	{
		WeaponPose start;
		WeaponPose rest;
		float      fallSeconds = 0.0f;
	};

	// 손에 든 총의 자세. body = 몸통 중심(MoveState::position), up · facing = 몸의 위 · 앞(단위, 서로 수직).
	WeaponPose HeldWeaponPose(const Vec3d& body, const Vec3d& up, const Vec3d& facing);

	// 그 자세에서 총구 자리(월드) — WeaponSpec::muzzle.
	Vec3d MuzzlePosition(const WeaponPose& pose, WeaponType type);

	// 손에서 떨군 총이 떨어져 눕는 자리.
	//  · 떨어지는 곳 = 손 자리 바로 아래의 지면(지형 높이, MoveWorld::SurfaceHeight) — 충돌체(바위 · 건물) 윗면은 아직 안 본다.
	//  · 누운 자세 = 옆면(+X)이 위, 총구가 몸 앞쪽(접평면), 원점이 지면 위 WeaponSpec::restHeight(X 반폭).
	//  · 떨어지는 시간 = √(2 × 높이 차 ÷ 중력). 그동안 클라가 손 자세 → 누운 자세로 옮겨 그린다.
	//  ★ 긴 총(라이플 · 기관총)의 비탈 기울이기 · 튕기기 · 구르기는 아직 없다.
	WeaponDrop SettleDroppedWeapon(const MoveWorld& world, WeaponType type,
		const Vec3d& body, const Vec3d& up, const Vec3d& facing);

} // namespace Shared
