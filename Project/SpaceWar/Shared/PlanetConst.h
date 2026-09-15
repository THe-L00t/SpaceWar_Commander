#pragma once

// ============================================================
//  Shared/PlanetConst.h — 행성 기하 상수
//
//  ★ 좌표계 규약 (Client/Planet.h 와 같은 것)
//     월드 원점 = 플레이어 스폰 지점(행성 표면), 행성 중심 = (0, -R, 0).
//
//  Shared 에 두는 이유:
//    서버가 위치를 판정하고 NPC 를 지표면에 올리려면 반지름과 중심을
//    클라와 «똑같이» 알아야 한다. 둘이 다르면 NPC 가 땅에 묻히거나 뜬다.
//    Client/Planet.h 의 kPlanetRadius 는 이 값을 가져다 쓴다.
// ============================================================

namespace Shared {

	// 2026-08-05 회의: 120km -> 1.6km (교수님 프로토타입 1,650m 와 같은 급)
	inline constexpr double kPlanetRadius = 1600.0;   // m

	// 행성 중심. 원점이 표면이므로 중심은 반지름만큼 아래에 있다.
	inline constexpr double kPlanetCenterX = 0.0;
	inline constexpr double kPlanetCenterY = -kPlanetRadius;
	inline constexpr double kPlanetCenterZ = 0.0;

} // namespace Shared
