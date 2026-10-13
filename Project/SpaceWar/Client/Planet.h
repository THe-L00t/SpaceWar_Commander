#pragma once
#include "Vec3d.h"
#include "Shared/Terrain/TerrainSampler.h"
#include "Shared/PlanetConst.h"

// ============================================================
//  Planet.h — 맵의 이동 좌표와 중력 정의
//
//  현재 GLB는 구면 맵이며 행성 중심으로부터의 방향이 위쪽이다.
//  평면 모드는 Shared::kUsePlanarMap으로 같은 인터페이스에서 선택한다.
//
//  ★ 좌표계 규약
//     행성 중심 = Shared::kPlanetCenter (0,-R,0).
//     기준구 북극을 원점에 두어 주변 좌표를 작게 유지한다.
//     시작 지점은 Landing L1 방향의 실제 지면 위다.
//
//  ★ 구면 계산은 double 로 한다
//     (R + 고도) 를 float 로 다루면 고도가 양자화된다. R 가 클수록 심해서
//     120km 에서는 7.8mm 단위였고, 카메라가 그만큼 떨리면 화면이 2픽셀 흔들렸다.
//
//  ★ SurfaceHeight() 가 지형의 유일한 접점이다
//     이 함수 하나만 TerrainSampler 로 위임하면
//     이동·착지·카메라 최소고도·메쉬 생성이 전부 자동으로 지형을 따른다.
//
//  ★ 반지름은 kPlanetRadius 한 곳에서만 정한다
//     center 가 radius 와 따로 놀면 고도가 통째로 어긋나 이동·착지가 전부 무너진다.
// ============================================================

namespace swc {

	// 현재 모델의 기준 반경은 Shared에서 정한다.
	// ★ 실제 값은 Shared 에 있다. 서버도 같은 값을 봐야 NPC 가 지표면에 놓인다.
	inline constexpr double kPlanetRadius = Shared::kPlanetRadius;

	struct Planet
	{
		double radius = kPlanetRadius;   // 기준구 반지름 (m)
		double gravity = 18.0;           // m/s^2 — 물리값 0.18 은 소행성 수준이라 게임이 안 됨
		Vec3d  center{ Shared::kPlanetCenterX, Shared::kPlanetCenterY, Shared::kPlanetCenterZ };

		const Shared::TerrainSampler* terrain = nullptr;   // 없으면 높이 0인 지면

		// 지표면 법선 (= 로컬 위쪽)
		Vec3d Up(const Vec3d& position) const
		{
			if constexpr (Shared::kUsePlanarMap) return { 0.0, 1.0, 0.0 };
			return Normalize(position - center);
		}

		// 평면 맵의 월드 Y 또는 기준구 위 고도
		double Altitude(const Vec3d& position) const
		{
			if constexpr (Shared::kUsePlanarMap) return position.y;
			return Length(position - center) - radius;
		}

		// 평면 맵은 월드 위치, 구면 맵은 단위 방향으로 같은 지형 조회를 사용한다.
		double SurfaceHeight(const Vec3d& surfacePoint) const
		{
			return terrain ? terrain->Height(surfacePoint.x, surfacePoint.y, surfacePoint.z) : 0.0;
		}

		// 평면 맵은 XZ를 보존하고 Y만 설정한다. 구면 맵은 방향과 고도로 구성한다.
		Vec3d PositionAt(const Vec3d& surfacePoint, double altitude) const
		{
			if constexpr (Shared::kUsePlanarMap) return { surfacePoint.x, altitude, surfacePoint.z };
			return center + surfacePoint * (radius + altitude);
		}
	};
}
