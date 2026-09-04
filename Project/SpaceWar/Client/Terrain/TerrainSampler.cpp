#include "TerrainSampler.h"
#include <algorithm>

namespace {

	// 가장자리에서 0, 안쪽에서 1 로 부드럽게 (smoothstep)
	double EdgeFade1D(double t, double fadeFraction)
	{
		if (fadeFraction <= 0.0) return 1.0;
		const double d = std::min(t, 1.0 - t);      // 가까운 변까지의 거리 [0, 0.5]
		if (d >= fadeFraction) return 1.0;
		const double s = d / fadeFraction;          // [0,1]
		return s * s * (3.0 - 2.0 * s);
	}
}

namespace swc {

	void TerrainSampler::Configure(const Shared::HeightmapData* heightmap,
		double planetRadius, const TerrainConfig& cfg)
	{
		data = (heightmap && heightmap->Valid()) ? heightmap : nullptr;
		radius = planetRadius;
		config = cfg;
	}

	double TerrainSampler::Height(const Vec3d& upDirection) const
	{
		if (!data) return 0.0;

		// 지면이 구 전체 메시라 반대편 반구도 그려진다. x,z 만 보면 대척점에
		// 같은 조각이 거울상으로 다시 나타나므로 스폰 반구(up.y > 0)에만 적용한다.
		if (upDirection.y <= 0.0) return 0.0;

		// 구 표면 위치는 P = center + up*R, center = (0,-R,0) 이므로
		// 스폰 기준 접평면 좌표는 (up.x*R, up.z*R) 이다.
		const double x = upDirection.x * radius;
		const double z = upDirection.z * radius;

		const double u = x / config.tileSize + 0.5;
		const double v = z / config.tileSize + 0.5;
		if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0)
			return 0.0;                                  // 조각 바깥은 평지

		// 경계 절벽 방지
		const double fade = EdgeFade1D(u, config.fade) * EdgeFade1D(v, config.fade);

		// 0 중심화 — 그냥 더하면 조각 평균만큼 지형 전체가 들린다
		const double h = double(data->Sample(float(u), float(v))) - double(data->mean);
		return h * config.relief * fade;
	}
}
