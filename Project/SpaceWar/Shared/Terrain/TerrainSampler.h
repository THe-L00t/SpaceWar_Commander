#pragma once
#include "../HeightmapData.h"

// ============================================================
//  Shared/Terrain/TerrainSampler.h — ★ 단일 진실 공급원
//
//  지형 메쉬를 만들 때, 플레이어가 땅을 밟을 때, 서버가 NPC 를 세우고
//  플레이어 위치를 검사할 때 반드시 이 함수를 거쳐야 한다.
//  갈리면 파묻히거나 공중에 뜨고, 서버 판정이 화면과 어긋난다.
//
//  ★ Shared 에 있는 이유 — 서버 권위
//    서버가 지형을 모르면 NPC 고도와 사격 판정이 화면과 최대 relief 만큼 어긋난다.
//    그래서 클라와 서버가 같은 조각 파일을 같은 설정으로 읽는다.
//
//  ★ Height() 가 double 세 개를 받는 이유
//    Vec3d 를 받게 하면 Vec3d(DirectXMath 의존)가 Shared 로 끌려온다.
//    HeightmapData::Sample() 이 float 만 받는 것과 같은 이유다.
// ============================================================

namespace Shared {

	// 지형 조각 파일. exe 옆 assets\ 기준 상대 경로. 클라와 서버가 이 한 곳을 본다.
	// 바꾸면 Client·Server .vcxproj 의 빌드 후 복사(xcopy) 파일명도 같이 바꿔야 한다.
	inline constexpr const wchar_t* kTerrainTileAsset =
		L"terrain\\Realistic_Mountain_v00__Realistic_Mountain_v00_Out.png";

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
		void Configure(const HeightmapData* heightmap, double planetRadius,
			const TerrainConfig& cfg);

		// 단위 방향 벡터 하나로 지형 높이(m). 기준구 표면 기준.
		double Height(double upX, double upY, double upZ) const;

		bool HasTerrain() const { return data != nullptr; }

	private:
		const HeightmapData* data = nullptr;
		double radius = 1600.0;     // Configure() 가 kPlanetRadius 로 덮어쓴다
		TerrainConfig config;
	};
}
