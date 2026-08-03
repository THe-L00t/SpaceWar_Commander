#pragma once
#include "Shared/Math/Vec3d.h"
#include "Shared/World/TerrainSampler.h"

// ============================================================
//  Planet.h — 구형 행성 정의
//
//  ★ 좌표계 규약
//     월드 원점 = 플레이어 스폰 지점(행성 표면), 행성 중심 = (0, -R, 0).
//     R = 120,000 이라 float32 정밀도가 그 근처에서 1.4cm 까지 벌어진다.
//     원점을 표면에 두면 주변 좌표가 전부 작은 수라 정밀도가 넉넉하고,
//     스폰 지점에서 up 이 정확히 (0,1,0) 이 되어 검증 기준점이 생긴다.
//
//  ★ SurfaceHeight() 가 지형의 유일한 접점이다
//     이 함수 하나만 TerrainSampler 로 위임하면
//     이동·착지·카메라 최소고도·메쉬 생성이 전부 자동으로 지형을 따른다.
//
//  ★ Shared 인 이유
//     서버가 위치를 계산하려면 반지름·중력·지형을 클라와 똑같이 알아야 한다.
//     하나라도 다르면 클라 예측과 서버 결과가 매 틱 어긋난다.
// ============================================================

namespace Shared {

	struct Planet
	{
		double radius = 120000.0;    // 기준구 반지름 (m)
		double gravity = 18.0;       // m/s^2 — 물리값 0.18 은 소행성 수준이라 게임이 안 됨
		Vec3d  center{ 0.0, -120000.0, 0.0 };

		const TerrainSampler* terrain = nullptr;   // 없으면 평평한 구

		// 지표면 법선 (= 로컬 위쪽)
		Vec3d Up(const Vec3d& position) const
		{
			return Normalize(position - center);
		}

		// 기준구 위 고도
		double Altitude(const Vec3d& position) const
		{
			return Length(position - center) - radius;
		}

		// ★ 지형 높이. 메쉬 생성과 충돌 판정이 반드시 이 경로를 거친다.
		double SurfaceHeight(const Vec3d& upDirection) const
		{
			return terrain ? terrain->Height(upDirection) : 0.0;
		}

		// 방향과 고도로부터 위치를 구성
		Vec3d PositionAt(const Vec3d& upDirection, double altitude) const
		{
			return center + upDirection * (radius + altitude);
		}
	};
}
