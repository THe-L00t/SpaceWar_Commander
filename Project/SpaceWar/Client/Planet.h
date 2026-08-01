#pragma once
#include <cmath>
#include <DirectXMath.h>
#include "Shared/Units.h"

// ============================================================
//  Planet.h — 구형 행성 정의와 구면 좌표 헬퍼
//
//  DX 타입 없음(DirectXMath 는 순수 수학 라이브러리). 게임 로직이 봐도 된다.
//
//  ★ 좌표계 규약
//     월드 원점 = 플레이어 스폰 지점(행성 표면), 행성 중심 = (0, -R, 0).
//     R = 120,000 이라 float32 정밀도가 그 근처에서 1.4cm 까지 벌어진다.
//     원점을 표면에 두면 주변 좌표가 전부 작은 수라 정밀도가 넉넉하고,
//     스폰 지점에서 up 이 정확히 (0,1,0) 이 되어 검증 기준점이 생긴다.
//
//  ★ 구면 계산은 double 로 한다
//     (R + 고도) 를 float 로 다루면 고도가 7.8mm 단위로 양자화되고
//     최종 위치 오차가 약 1.2cm 남는다. 카메라가 그만큼 떨리면
//     5m 앞 물체 기준으로 화면이 2픽셀 흔들려 눈에 띈다.
// ============================================================

namespace swc {

	// 구면 계산 전용 최소 double 벡터. DirectXMath 에는 double 타입이 없다.
	struct Vec3d
	{
		double x = 0.0, y = 0.0, z = 0.0;

		Vec3d() = default;
		Vec3d(double px, double py, double pz) : x(px), y(py), z(pz) {}
		explicit Vec3d(const DirectX::XMFLOAT3& v) : x(v.x), y(v.y), z(v.z) {}

		DirectX::XMFLOAT3 ToFloat3() const
		{
			return { float(x), float(y), float(z) };
		}
	};

	inline Vec3d operator+(const Vec3d& a, const Vec3d& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	inline Vec3d operator-(const Vec3d& a, const Vec3d& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	inline Vec3d operator*(const Vec3d& a, double s) { return { a.x * s, a.y * s, a.z * s }; }

	inline double Dot(const Vec3d& a, const Vec3d& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline double Length(const Vec3d& a) { return std::sqrt(Dot(a, a)); }

	inline Vec3d Cross(const Vec3d& a, const Vec3d& b)
	{
		return { a.y * b.z - a.z * b.y,
				 a.z * b.x - a.x * b.z,
				 a.x * b.y - a.y * b.x };
	}

	inline Vec3d Normalize(const Vec3d& a)
	{
		const double len = Length(a);
		return len > 1e-12 ? a * (1.0 / len) : Vec3d{ 0.0, 1.0, 0.0 };
	}

	// 벡터를 평면(법선 n, 단위)에 투영하고 정규화. 퇴화 시 대체 벡터를 만든다.
	inline Vec3d ProjectOntoPlane(const Vec3d& v, const Vec3d& n)
	{
		Vec3d p = v - n * Dot(v, n);
		if (Length(p) < 1e-9)
		{
			// v 가 n 과 거의 평행 → 아무 수직 벡터나 하나 만든다
			const Vec3d ref = (std::fabs(n.y) < 0.9) ? Vec3d{ 0.0, 1.0, 0.0 } : Vec3d{ 1.0, 0.0, 0.0 };
			p = Cross(n, ref);
		}
		return Normalize(p);
	}

	// up 축 기준으로 v 를 angle(rad) 만큼 회전 (로드리게스)
	inline Vec3d RotateAround(const Vec3d& v, const Vec3d& axis, double angle)
	{
		const double c = std::cos(angle);
		const double s = std::sin(angle);
		return v * c + Cross(axis, v) * s + axis * (Dot(axis, v) * (1.0 - c));
	}


	struct Planet
	{
		double radius = 120000.0;    // 기준구 반지름 (m)
		double gravity = 18.0;       // m/s^2 — 물리값 0.18 은 소행성 수준이라 게임이 안 됨
		Vec3d  center{ 0.0, -120000.0, 0.0 };

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

		// ★ 하이트맵이 꽂힐 지점. 지금은 평평한 구.
		//    나중에 조각 하이트맵을 읽어 방향별 지형 높이를 반환한다.
		double SurfaceHeight(const Vec3d& /*upDirection*/) const
		{
			return 0.0;
		}

		// 방향과 고도로부터 위치를 구성
		Vec3d PositionAt(const Vec3d& upDirection, double altitude) const
		{
			return center + upDirection * (radius + altitude);
		}
	};

}
