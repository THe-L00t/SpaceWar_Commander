#pragma once
#include "Shared/Math/Vec3d.h"
#include "Shared/HeightmapData.h"

// ============================================================
//  TerrainSampler — ★ 단일 진실 공급원
//
//  지형 메쉬를 만들 때와 플레이어가 땅을 밟을 때가 반드시 이 함수를 거쳐야 한다.
//  갈리면 플레이어가 지형에 파묻히거나 공중에 뜬다.
//
//  ★ Shared 로 옮긴 이유
//    서버가 이동을 계산하려면 착지 높이를 알아야 한다.
//    클라만 지형을 알면 서버는 평지로 판단해 위치가 계속 어긋난다.
// ============================================================

namespace Shared {

	struct TerrainConfig
	{
		double tileSize = 1000.0;   // 조각이 덮는 실제 크기 (m)
		double relief = 150.0;      // 표고차 (m)
		double fade = 0.25;         // 바깥 몇 %를 0으로 감쇠할지 (경계 절벽 방지)
	};

	class TerrainSampler
	{
	public:
		void Configure(const HeightmapData* heightmap, double planetRadius,
			const TerrainConfig& cfg);

		// 방향 벡터 하나로 지형 높이(m). 기준구 표면 기준.
		double Height(const Vec3d& upDirection) const;

		bool HasTerrain() const { return data != nullptr; }

	private:
		const HeightmapData* data = nullptr;
		double radius = 120000.0;
		TerrainConfig config;
	};
}
