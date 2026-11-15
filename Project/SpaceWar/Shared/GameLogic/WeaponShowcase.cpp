#include "WeaponShowcase.h"

#include "PlayerMovement.h"
#include "../PlanetConst.h"
#include "../Physics/CollisionWorld.h"

// 머리 주석은 WeaponShowcase.h. 2026-11-14 Client/Engine/Engine.cpp BuildWeaponShowcase 의 자리 계산을 그대로 옮겼다.

namespace Shared {

	void LayoutWeaponShowcase(const MoveWorld& world, const Vec3d& spawnPoint, const Vec3d& spawnFacing,
		WeaponShowcaseSlot (&out)[kWeaponShowcaseSlotCount])
	{
		// 클라 · 서버의 스폰과 같은 식으로 세운다 — 위치 · 위 · 앞이 양쪽 첫 상태와 같아진다.
		MoveState spawned;
		PlayerMovement::Spawn(spawned, world, spawnPoint, spawnFacing);
		const Vec3d origin = spawned.position;
		const Vec3d up = spawned.up;
		const Vec3d facing = spawned.facing;
		const Vec3d right = Cross(up, facing);

		size_t slot = 0;
		for (size_t t = 0; t < kWeaponTypeCount; ++t)
		{
			// 총 종류 사이를 오른쪽으로 띄운다 — 가운데가 1번(레이저 라이플).
			const Vec3d column = origin + facing * kWeaponShowcaseDistance +
				right * ((double(t) - double(kWeaponTypeCount - 1) * 0.5) * kWeaponShowcaseSpacing);
			const Vec3d columnUp = world.Up(column);
			const Vec3d surfacePoint = kUsePlanarMap ? column : columnUp;
			const double ground = world.SurfaceHeight(surfacePoint);
			const Vec3d columnFacing = ProjectOntoPlane(facing, columnUp);

			for (int view = 0; view < 2; ++view)
			{
				WeaponShowcaseSlot& s = out[slot++];
				s.type = static_cast<WeaponType>(t);
				s.firstPerson = (view == 0);
				const double height = s.firstPerson ? kWeaponShowcaseHeightFirstPerson : kWeaponShowcaseHeightThirdPerson;
				s.position = world.PositionAt(surfacePoint, ground + height);
				s.axisX = columnFacing * -1.0;               // 옆면 → 스폰 쪽
				s.axisY = columnUp;                          // 위
				s.axisZ = Cross(columnUp, columnFacing);     // 총구 → 오른쪽
			}
		}
	}

	size_t AddWeaponShowcaseColliders(CollisionWorld& collision, const MoveWorld& world,
		const WeaponShowcaseSlot (&slots)[kWeaponShowcaseSlotCount])
	{
		// 종류마다 상자 묶음 메시 하나(2026-11-15 — 1인칭 메시를 덮는 상자, Weapon.cpp kPistolBoxes 등)
		uint32_t meshOfType[kWeaponTypeCount];
		for (size_t t = 0; t < kWeaponTypeCount; ++t)
		{
			const WeaponSpec& spec = SpecOf(static_cast<WeaponType>(t));
			meshOfType[t] = collision.AddBoxCollider(std::string("Weapon_") + spec.name,
				spec.colliderBoxes, spec.colliderBoxCount, CollisionClass::Prop);
		}

		size_t added = 0;
		for (const WeaponShowcaseSlot& s : slots)
		{
			const size_t t = static_cast<size_t>(s.type);
			if (t >= kWeaponTypeCount || meshOfType[t] == CollisionWorld::kNoMesh) continue;

			// 메시 공간(총 glb 루트) → 맵 공간. 행 벡터 3 × 4: p' = x · axisX + y · axisY + z · axisZ + (위치 − 행성 중심)
			const Vec3d p = s.position - world.center;
			const float xf[12] = {
				float(s.axisX.x), float(s.axisX.y), float(s.axisX.z),
				float(s.axisY.x), float(s.axisY.y), float(s.axisY.z),
				float(s.axisZ.x), float(s.axisZ.y), float(s.axisZ.z),
				float(p.x),       float(p.y),       float(p.z) };
			if (collision.AddInstance(meshOfType[t], xf)) ++added;
		}
		return added;
	}

} // namespace Shared
