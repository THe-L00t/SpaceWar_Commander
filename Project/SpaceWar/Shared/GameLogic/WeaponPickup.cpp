#include "WeaponPickup.h"

#include <cmath>
#include "PlayerMovement.h"
#include "../PlanetConst.h"

// 머리 주석은 WeaponPickup.h.

namespace Shared {

	WeaponPose HeldWeaponPose(const Vec3d& body, const Vec3d& up, const Vec3d& facing)
	{
		// 몸 축 — PlayerController::WorldMatrix · Client CharacterTransform 과 같은 배치(오른쪽 = up × facing).
		const Vec3d right = Cross(up, facing);
		WeaponPose pose;
		pose.position = body + right * kHeldWeaponRight + up * kHeldWeaponUp + facing * kHeldWeaponForward;
		pose.axisX = right;    // 총 옆
		pose.axisY = up;       // 총 위
		pose.axisZ = facing;   // 총구 = 몸 앞 (right × up = facing — Z = X × Y)
		return pose;
	}

	Vec3d MuzzlePosition(const WeaponPose& pose, WeaponType type)
	{
		const WeaponSpec& spec = SpecOf(type);
		return pose.position + pose.axisX * double(spec.muzzle[0]) + pose.axisY * double(spec.muzzle[1]) +
			pose.axisZ * double(spec.muzzle[2]);
	}

	WeaponDrop SettleDroppedWeapon(const MoveWorld& world, WeaponType type,
		const Vec3d& body, const Vec3d& up, const Vec3d& facing)
	{
		WeaponDrop drop;
		drop.start = HeldWeaponPose(body, up, facing);

		// 손 자리 바로 아래 지면
		const Vec3d hand = drop.start.position;
		const Vec3d groundUp = world.Up(hand);
		const Vec3d surfacePoint = kUsePlanarMap ? hand : groundUp;
		const double ground = world.SurfaceHeight(surfacePoint);
		const double restAltitude = ground + double(SpecOf(type).restHeight);

		// 옆면(+X)이 위 · 총구는 몸 앞쪽을 지면에 투영한 쪽. Z = X × Y 가 되게 Y = Z × X.
		const Vec3d muzzleDir = ProjectOntoPlane(facing, groundUp);
		drop.rest.position = world.PositionAt(surfacePoint, restAltitude);
		drop.rest.axisX = groundUp;
		drop.rest.axisZ = muzzleDir;
		drop.rest.axisY = Cross(muzzleDir, groundUp);

		// 떨어지는 시간 — 손 높이에서 누운 높이까지 자유 낙하. 손이 더 낮으면(비탈 아래쪽) 0.
		const double height = world.Altitude(hand) - restAltitude;   // Altitude 는 평면 맵 = y, 구면 맵 = 기준구 위 고도 — restAltitude 와 같은 잣대
		const double gravity = world.gravity > 1.0e-6 ? world.gravity : 9.8;
		drop.fallSeconds = height > 0.0 ? float(std::sqrt(2.0 * height / gravity)) : 0.0f;
		return drop;
	}

} // namespace Shared
