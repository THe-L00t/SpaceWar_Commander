#pragma once

// ============================================================
//  Shared/PlanetConst.h — 공통 맵 기하 상수
//
//  ★ 좌표계 규약 (Client/Planet.h 와 같은 것)
//     GLB의 미터 단위를 유지하며 구면 중심은 (0,-R,0)이다.
//     ★ 파서(Shared/Model)는 glTF 좌표를 변환하지 않는다(Readers/GltfReader.cpp 머리 주석).
//       client2 의 GlbDocument 는 Z 를 반전했으므로, 그쪽에서 잰 좌표의 Z 는 부호를 뒤집어 옮겼다.
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
	// 평면 모드에서 사용하는 월드 X/Z 시작 좌표. (client2 값 Z=-63 의 부호를 뒤집었다)
	inline constexpr double kMapSpawnX = -102.0;
	inline constexpr double kMapSpawnZ = 63.0;
	// Landing L1 방향. GLB 원본 좌표 그대로의 단위 벡터다.
	// (client2 는 Z 를 반전해 -0.2259165190 — 우리 파서는 반전하지 않으므로 +)
	inline constexpr double kPlanetSpawnUpX = -0.3261576291;
	inline constexpr double kPlanetSpawnUpY = 0.9179231599;
	inline constexpr double kPlanetSpawnUpZ = 0.2259165190;

	inline constexpr const wchar_t* kPlanetModelAsset =
		L"model\\map\\future_ruins_planet_separated.glb";
	inline constexpr const char* kPlanetSurfaceNode = "COL_Expedition_Planet_Terrain";

	// 정적 오브젝트 충돌체 묶음 (2026-10-29, Shared/Physics/CollisionWorld.h) — 맵 glb 옆.
	//  Resource_Work/_tools/collision/col_build.py 가 만든다. ★ Client.vcxproj CopyModelAssets 에도 적혀 있다 — 바꿀 때 두 곳.
	inline constexpr const wchar_t* kPlanetCollisionAsset =
		L"model\\map\\future_ruins_planet_separated_Collision.glb";
	inline constexpr const wchar_t* kPlanetCollisionManifestAsset =
		L"model\\map\\future_ruins_planet_separated_Collision.json";

	inline constexpr double kPlanetCenterX = 0.0;
	inline constexpr double kPlanetCenterY = kUsePlanarMap ? 0.0 : -kPlanetRadius;
	inline constexpr double kPlanetCenterZ = 0.0;

	// 몸통 중심에서 발까지의 높이. 클라이언트·서버 접지가 공유한다.
	inline constexpr double kGroundOffset = 1.0;

	// 플레이어 중력 (m/s²) — 물리값 0.18 은 소행성 수준이라 게임이 안 됨.
	//  2026-10-29 Client/Planet.h 에서 옮겼다 — 이동을 서버도 같이 계산한다(Shared/GameLogic/PlayerMovement.h).
	inline constexpr double kPlanetGravity = 18.0;

} // namespace Shared
