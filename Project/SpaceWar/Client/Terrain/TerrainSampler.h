#pragma once
#include "../Vec3d.h"
#include "Shared/HeightmapData.h"

// ============================================================
//  TerrainSampler — ★ 단일 진실 공급원
//
//  지형 메쉬를 만들 때와 플레이어가 땅을 밟을 때가 반드시 이 함수를 거쳐야 한다.
//  갈리면 플레이어가 지형에 파묻히거나 공중에 뜬다.
//
//  지금은 조각 1장을 스폰 주변에만 적용하는 20줄짜리지만,
//  이 자리를 지금 만들어두지 않으면 나중에 메쉬 생성이 자기만의 높이 계산을 갖게 된다.
// ============================================================

namespace swc {

	struct TerrainConfig
	{
		double tileSize = 1000.0;   // 조각이 덮는 실제 크기 (m)
		double relief = 150.0;      // 표고차 (m)
		double fade = 0.25;         // 바깥 몇 %를 0으로 감쇠할지 (경계 절벽 방지)
	};

	class TerrainSampler
	{
	public:
		void Configure(const Shared::HeightmapData* heightmap, double planetRadius,
			const TerrainConfig& cfg);

		// 방향 벡터 하나로 지형 높이(m). 기준구 표면 기준.
		double Height(const Vec3d& upDirection) const;

		bool HasTerrain() const { return data != nullptr; }

	private:
		const Shared::HeightmapData* data = nullptr;
		double radius = 120000.0;
		TerrainConfig config;
	};
}
