#include "PhysicsCalculator.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

#include "../PlanetConst.h"
#include "../Terrain/TerrainSampler.h"

namespace Shared {

	namespace {

		// 지형 행진 간격과 여유. 사거리 300m 면 표본 150개다.
		constexpr float kMarchStep = 2.0f;        // m
		constexpr float kMarchStart = 2.0f;       // 총구 바로 앞은 건너뛴다 (m)

		// ★ 여유를 두는 이유
		//   발사 원점이 «지면 + kGroundOffset» 이라, 내리막에서는 한 걸음만 나가도
		//   표본이 지면 아래로 들어간다. 여유가 없으면 평지에서도 자기 발밑에 막힌다.
		constexpr double kMarchTolerance = 0.5;   // m

		double Dot(const Vec3& a, const Vec3& b)
		{
			return double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z;
		}

	} // namespace

	PhysicsCalculator::PhysicsCalculator() = default;
	PhysicsCalculator::~PhysicsCalculator() = default;

	bool PhysicsCalculator::RaySphere(const Vec3& origin, const Vec3& direction,
		const Vec3& center, float radius, float maxDistance, float& outDistance)
	{
		// origin -> center 로 가는 벡터를 레이에 사영해 최근접점을 찾는다.
		const Vec3 toCenter{ center.x - origin.x, center.y - origin.y, center.z - origin.z };

		const double along = Dot(toCenter, direction);   // direction 은 단위벡터
		const double distSq = Dot(toCenter, toCenter);
		const double perpSq = distSq - along * along;    // 중심에서 레이까지의 수직거리 제곱

		const double radiusSq = double(radius) * radius;
		if (perpSq > radiusSq)
			return false;								// 빗나갔다

		const double half = std::sqrt(radiusSq - perpSq);
		double hit = along - half;						// 앞면

		if (hit < 0.0)
		{
			// 원점이 구 안에 있으면 0 거리로 맞은 것으로 본다. 뒤쪽은 맞지 않는다.
			if (distSq > radiusSq) return false;
			hit = 0.0;
		}

		if (hit > double(maxDistance))
			return false;

		outDistance = float(hit);
		return true;
	}

	bool PhysicsCalculator::RayCapsule(const Vec3d& origin, const Vec3d& direction, const Vec3d& a, const Vec3d& b,
		double radius, double tMin, double tMax, double& outDistance)
	{
		constexpr double kTiny = 1.0e-12;
		double enter = 1.0e300, exit = -1.0e300;
		auto take = [&](double t0, double t1) { enter = (std::min)(enter, t0); exit = (std::max)(exit, t1); };

		// 두 끝 구
		for (const Vec3d& c : { a, b })
		{
			const Vec3d oc = origin - c;
			const double half = Shared::Dot(oc, direction);
			const double h = half * half - (Shared::Dot(oc, oc) - radius * radius);
			if (h >= 0.0) take(-half - std::sqrt(h), -half + std::sqrt(h));
		}
		// 몸통 — 끝없는 원기둥 ∩ 두 끝 면 사이
		const Vec3d axis = b - a;
		const double length = Length(axis);
		if (length > kTiny)
		{
			const Vec3d u = axis * (1.0 / length);
			const Vec3d oa = origin - a;
			const double du = Shared::Dot(direction, u), ou = Shared::Dot(oa, u);
			const Vec3d dp = direction - u * du, op = oa - u * ou;
			const double qa = Shared::Dot(dp, dp), qb = Shared::Dot(op, dp), qc = Shared::Dot(op, op) - radius * radius;
			const double h = qb * qb - qa * qc;
			if (qa > kTiny && h >= 0.0 && std::fabs(du) > kTiny)
			{
				double c0 = (-qb - std::sqrt(h)) / qa, c1 = (-qb + std::sqrt(h)) / qa;
				double s0 = -ou / du, s1 = (length - ou) / du;
				if (s0 > s1) std::swap(s0, s1);
				const double t0 = (std::max)(c0, s0), t1 = (std::min)(c1, s1);
				if (t0 <= t1) take(t0, t1);
			}
			else if (qa > kTiny && h >= 0.0 && ou >= 0.0 && ou <= length)
				take((-qb - std::sqrt(h)) / qa, (-qb + std::sqrt(h)) / qa);   // 축에 수직으로 지나간다
			// 축과 나란한 레이(qa ≈ 0)는 두 끝 구 구간이 이미 몸통을 덮는다
		}
		if (!(enter <= exit) || exit < tMin || enter > tMax) return false;
		outDistance = (std::max)(enter, tMin);
		return true;
	}

	bool PhysicsCalculator::TerrainBlocks(const TerrainSampler* terrain,
		const Vec3& origin, const Vec3& direction, float distance)
	{
		for (float t = kMarchStart; t < distance; t += kMarchStep)
		{
			if constexpr (kUsePlanarMap)
			{
				const double x = double(origin.x) + double(direction.x) * t;
				const double y = double(origin.y) + double(direction.y) * t;
				const double z = double(origin.z) + double(direction.z) * t;
				const double height = terrain ? terrain->Height(x, y, z) : 0.0;
				if (y < height - kMarchTolerance) return true;
				continue;
			}
			const double px = double(origin.x) + double(direction.x) * t - kPlanetCenterX;
			const double py = double(origin.y) + double(direction.y) * t - kPlanetCenterY;
			const double pz = double(origin.z) + double(direction.z) * t - kPlanetCenterZ;

			const double len = std::sqrt(px * px + py * py + pz * pz);
			if (len < 1.0e-6)
				return true;							// 행성 중심을 지난다 = 통과 불가

			const double height = terrain
				? terrain->Height(px / len, py / len, pz / len)
				: 0.0;

			if (len < kPlanetRadius + height - kMarchTolerance)
				return true;
		}

		return false;
	}

} // namespace Shared
