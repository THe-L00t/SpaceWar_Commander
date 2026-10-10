#pragma once

// ============================================================
//  Shared/PlanetConst.h — 공통 맵 기하 상수
//
//  ★ 좌표계 규약 (Client/Planet.h 와 같은 것)
//     GLB의 미터 단위를 유지하며 구면 중심은 (0,-R,0)이다.
//
//  Shared 에 두는 이유:
//    서버의 접지·NPC·위치 판정과 클라이언트 렌더·이동이 같은
//    파일, 반지름, 중심, 시작 지점을 사용해야 한다.
// ============================================================

namespace Shared {

	// Ruins Enhanced / Separated Planet 원본의 기준 반경. 표시와 접지 배율은 1이다.
	inline constexpr double kPlanetRadius = 650.0;   // m
	inline constexpr double kPlanetModelReferenceRadius = 650.0;

	inline constexpr bool kUsePlanarMap = false;
	// 평면 모드에서 사용하는 월드 X/Z 시작 좌표.
	inline constexpr double kMapSpawnX = -102.0;
	inline constexpr double kMapSpawnZ = -63.0;
	// Landing L1 방향. GLB의 오른손 Z를 반전한 단위 벡터다.
	inline constexpr double kPlanetSpawnUpX = -0.3261576291;
	inline constexpr double kPlanetSpawnUpY = 0.9179231599;
	inline constexpr double kPlanetSpawnUpZ = -0.2259165190;

	inline constexpr const wchar_t* kPlanetModelAsset =
		L"model\\map\\future_ruins_planet_separated.glb";
	inline constexpr const char* kPlanetSurfaceNode = "COL_Expedition_Planet_Terrain";

	inline constexpr double kPlanetCenterX = 0.0;
	inline constexpr double kPlanetCenterY = kUsePlanarMap ? 0.0 : -kPlanetRadius;
	inline constexpr double kPlanetCenterZ = 0.0;

	// 몸통 중심에서 발까지의 높이. 클라이언트·서버 접지가 공유한다.
	inline constexpr double kGroundOffset = 1.0;

} // namespace Shared
