#include "ShotTrace.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include "PlayerMovement.h"       // MoveWorld · 캐릭터 캡슐(kCapsuleRadius · kCapsuleHeight)
#include "../PlanetConst.h"       // kUsePlanarMap · kGroundOffset
#include "../Physics/CollisionWorld.h"
#include "../Physics/PhysicsCalculator.h"   // RayCapsule — 캐릭터 몸(2026-11-25)

// 머리 주석은 ShotTrace.h.

namespace Shared {

	void BodyPoseTransform(const Vec3d& center, const Vec3d& up, const Vec3d& mapCenter, float outXf[12])
	{
		const Vec3d u = Normalize(up);
		const Vec3d forward = ProjectOntoPlane({ 0.0, 0.0, 1.0 }, u);   // CharacterTransform 과 같다
		const Vec3d right = Cross(u, forward);
		const Vec3d t = center - u * kGroundOffset - mapCenter;        // 메시 원점(발밑) = 몸통 중심 − kGroundOffset
		const float xf[12] = {
			float(right.x), float(right.y), float(right.z),
			float(u.x), float(u.y), float(u.z),
			float(forward.x), float(forward.y), float(forward.z),
			float(t.x), float(t.y), float(t.z) };
		std::copy(std::begin(xf), std::end(xf), outXf);
	}

	ShotHit TraceShot(const MoveWorld& world, const Vec3d& origin, const Vec3d& direction, double maxDistance,
		const ShotTarget* targets, size_t targetCount, const ShotOptions& options)
	{
		ShotHit hit;
		hit.distance = maxDistance;

		const double length = Length(direction);
		if (!(length > 1.0e-9) || !(maxDistance > 0.0)) return hit;
		const Vec3d dir = direction * (1.0 / length);
		const double tStart = (std::max)(0.0, options.minDistance);
		const double tEnd = (std::max)(tStart, maxDistance);
		double best = tEnd;   // 지금까지 가장 가까운 닿은 거리

		// ① 몸 — 그 몸의 충돌체에 가장 먼저 닿는 것(2026-11-25 — 예전 구 kHitRadius 대신).
		//  몬스터 = 몸 묶음(COL_ 부위 메시)을 몸 자세로 씌워 RaycastPosed(정적 충돌체와 같은 BVH 판정, 맵 공간).
		//  캐릭터 = 이동 캡슐(아래 구 중심 = 몸통 중심 − (kGroundOffset − r), 위 구 중심 = 몸통 중심 + (키 − kGroundOffset − r)).
		const double capsuleRadius = double(PlayerMovement::kCapsuleRadius);
		const double capsuleBottom = kGroundOffset - capsuleRadius;
		const double capsuleTop = double(PlayerMovement::kCapsuleHeight) - kGroundOffset - capsuleRadius;
		for (size_t i = 0; targets && i < targetCount; ++i)
		{
			const ShotTarget& target = targets[i];
			const Vec3d up = world.Up(target.center);
			if (target.body && target.body->BodyPack())
			{
				float xf[12];
				BodyPoseTransform(target.center, up, world.center, xf);
				const Vec3d from = origin + dir * tStart - world.center;   // 맵 공간
				float distance = 0.0f;
				uint32_t part = ShotHit::kNoPart;
				if (!target.body->RaycastPosed(xf, Vec3{ float(from.x), float(from.y), float(from.z) },
					Vec3{ float(dir.x), float(dir.y), float(dir.z) }, float(best - tStart), distance, part))
					continue;
				const double enter = tStart + double(distance);
				if (enter >= best) continue;
				best = enter;
				hit.kind = ShotHitKind::Body;
				hit.target = i;
				hit.part = part;
				continue;
			}
			double enter = 0.0;
			if (!PhysicsCalculator::RayCapsule(origin, dir, target.center - up * capsuleBottom, target.center + up * capsuleTop,
				capsuleRadius, tStart, best, enter))
				continue;
			if (enter >= best) continue;
			best = enter;
			hit.kind = ShotHitKind::Body;
			hit.target = i;
			hit.part = ShotHit::kNoPart;
		}

		// ② 충돌체 — 몸보다 앞에서 맞으면 막힌다. 충돌체는 맵 공간(행성 중심 기준).
		if (world.collision && world.collision->Loaded() && best > tStart)
		{
			const Vec3d from = origin + dir * tStart - world.center;
			float distance = 0.0f;
			if (world.collision->Raycast(Vec3{ float(from.x), float(from.y), float(from.z) },
				Vec3{ float(dir.x), float(dir.y), float(dir.z) }, float(best - tStart), distance))
			{
				best = tStart + double(distance);
				hit.kind = ShotHitKind::Collider;
			}
		}

		// ③ 지형 — 몸 · 충돌체보다 앞에서 지면 아래(허용 깊이 넘게)로 들어가면 막힌다. 원점이 이미 땅속이면 보지 않는다.
		const auto underground = [&](double t)
		{
			const Vec3d p = origin + dir * t;
			const Vec3d surfacePoint = kUsePlanarMap ? p : world.Up(p);
			return world.Altitude(p) < world.SurfaceHeight(surfacePoint) - options.terrainTolerance;
		};
		if (!underground(tStart))
		{
			double previous = tStart;
			for (double t = tStart + kShotTerrainStep; previous < best; t += kShotTerrainStep)
			{
				const double sample = (std::min)(t, best);
				if (underground(sample))
				{
					double lo = previous, hi = sample;
					for (int i = 0; i < kShotTerrainRefine; ++i)
					{
						const double mid = 0.5 * (lo + hi);
						if (underground(mid)) hi = mid; else lo = mid;
					}
					best = hi;
					hit.kind = ShotHitKind::Terrain;
					break;
				}
				previous = sample;
			}
		}

		hit.distance = best;
		return hit;
	}

} // namespace Shared
