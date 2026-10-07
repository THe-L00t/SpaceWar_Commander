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

	// 클라이언트 렌더 모델과 서버 접지 계산이 같은 파일·배율을 사용한다.
	// 모델의 원점은 행성 중심이다. Planet_Core 정점의 평균 반지름을 기준구에 맞춘다.
	inline constexpr const wchar_t* kPlanetModelAsset =
		L"model\\planet\\future_ruins_realistic.obj";
	// 접지 보정 OBJ는 미터 단위이며 Planet_Core의 평균 반경이 1,600m다.
	// 표시 배율을 1로 유지해 건물·차량·엄폐물의 FPS 크기를 보존한다.
	inline constexpr double kPlanetModelReferenceRadius = 1600.0;

	// 행성 중심. 원점이 표면이므로 중심은 반지름만큼 아래에 있다.
	inline constexpr double kPlanetCenterX = 0.0;
	inline constexpr double kPlanetCenterY = -kPlanetRadius;
	inline constexpr double kPlanetCenterZ = 0.0;

	// 몸통 중심이 지면에서 떠 있는 높이 (m). 더미 큐브 2m 의 반높이.
	// 플레이어 착지(클라)·NPC 고도(서버)·위치 검사(서버)가 같은 값을 써야 발이 땅에 닿는다.
	inline constexpr double kGroundOffset = 1.0;

} // namespace Shared
