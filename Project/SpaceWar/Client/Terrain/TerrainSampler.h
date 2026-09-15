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

	// 반지름 1.6km 기준 배율 (2026-09-05 조정). 120km 시절엔 relief 150m / fade 25% 였다.
	//   relief 60m = 반지름의 3.75%. 09-04 의 30m(프로토타입 진폭 비율 1.8%)는 실기에서 너무 평평했다.
	//   로더가 조각을 min-max 정규화하므로 relief 는 조각의 «실제» 봉우리-골 높이다.
	//   fade 10% = 가장자리 100m 만 감쇠. 25% 는 조각 면적의 75% 를 눌러 스폰 밖이 금방 평평해졌다.
	//   tileSize 는 1024px 조각이 약 1m/px 가 되는 1km 를 유지한다.
	struct TerrainConfig
	{
		double tileSize = 1000.0;   // 조각이 덮는 실제 크기 (m)
		double relief = 60.0;       // 표고차 (m) — 조각의 봉우리-골 높이
		double fade = 0.10;         // 바깥 몇 %를 0으로 감쇠할지 (경계 절벽 방지)
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
		double radius = 1600.0;     // Configure() 가 Planet 의 값으로 덮어쓴다
		TerrainConfig config;
	};
}
