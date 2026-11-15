#include "ShotTrace.h"

#include <algorithm>
#include <cmath>
#include "GameLogic.h"            // kHitRadius — 서버 판정과 클라 겨눈 점이 같은 구를 쓴다
#include "PlayerMovement.h"       // MoveWorld
#include "../PlanetConst.h"       // kUsePlanarMap
#include "../Physics/CollisionWorld.h"

// 머리 주석은 ShotTrace.h.

namespace Shared {

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

		// ① 몸 — 레이가 구에 들어가는 거리가 가장 가까운 것. 구 중심이 레이 앞쪽(tStart 이후)에 있어야 한다.
		const double radius = double(kHitRadius);
		for (size_t i = 0; targets && i < targetCount; ++i)
		{
			const Vec3d toCenter = targets[i].center - origin;
			const double closest = Dot(toCenter, dir);
			if (closest < tStart || closest > tEnd) continue;
			const double miss2 = Dot(toCenter, toCenter) - closest * closest;
			if (miss2 > radius * radius) continue;
			const double enter = (std::max)(tStart, closest - std::sqrt((std::max)(0.0, radius * radius - miss2)));
			if (enter >= best) continue;
			best = enter;
			hit.kind = ShotHitKind::Body;
			hit.target = i;
			hit.closest = closest;
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
