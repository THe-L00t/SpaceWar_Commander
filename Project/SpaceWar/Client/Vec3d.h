#pragma once
#include <cmath>
#include <DirectXMath.h>

// 구면 계산 전용 double 벡터. DirectXMath 에는 double 타입이 없다.
//
// Planet 에서 분리한 이유: Planet 이 TerrainSampler 를 들고,
// TerrainSampler 는 Vec3d 를 받으므로 한 헤더에 두면 순환 참조가 된다.
namespace swc {

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
