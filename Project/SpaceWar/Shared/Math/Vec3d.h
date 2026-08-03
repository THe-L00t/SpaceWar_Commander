#pragma once
#include <cmath>

// ============================================================
//  Shared/Math/Vec3d.h — 구면 계산 전용 double 벡터
//
//  ★ 왜 Shared 인가
//    서버가 권위를 가지므로 클라와 서버가 "똑같은 계산"을 해야 한다.
//    이동 계산이 Shared 로 오면 그 재료인 벡터도 따라와야 한다.
//
//  ★ 왜 DirectXMath 를 쓰지 않는가
//    서버는 그래픽을 모른다. 그리고 DirectXMath 에는 double 타입이 아예 없다.
//    XMFLOAT3 로 바꾸는 변환은 클라 전용이므로 Client/Vec3dInterop.h 에 둔다.
//
//  ★ 왜 double 인가
//    행성 반지름이 120,000m 라 그 근처에서 float 정밀도가 1.4cm 까지 벌어진다.
//    (R + 고도) 를 float 로 다루면 고도가 7.8mm 단위로 양자화되고
//    카메라가 프레임마다 약 2픽셀 떨린다.
// ============================================================

namespace Shared {

	struct Vec3d
	{
		double x = 0.0, y = 0.0, z = 0.0;

		Vec3d() = default;
		Vec3d(double px, double py, double pz) : x(px), y(py), z(pz) {}
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
			const Vec3d ref = (std::fabs(n.y) < 0.9) ? Vec3d{ 0.0, 1.0, 0.0 } : Vec3d{ 1.0, 0.0, 0.0 };
			p = Cross(n, ref);
		}
		return Normalize(p);
	}

	// axis 축 기준으로 v 를 angle(rad) 만큼 회전 (로드리게스)
	inline Vec3d RotateAround(const Vec3d& v, const Vec3d& axis, double angle)
	{
		const double c = std::cos(angle);
		const double s = std::sin(angle);
		return v * c + Cross(axis, v) * s + axis * (Dot(axis, v) * (1.0 - c));
	}
}
