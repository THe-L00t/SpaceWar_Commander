#pragma once
#include <cmath>

// ============================================================
//  Shared/Vec3d.h — 구면 계산용 double 벡터
//
//  ★ 2026-10-29 Client/Vec3d.h 에서 옮겼다 (노션 「충돌 처리 ②」 3절 «벡터 → Shared»).
//    플레이어 이동(Shared::PlayerMovement)을 클라 예측과 서버 판정이 같이 쓰므로
//    같은 double 계산이 Shared 에 있어야 한다. 정의는 여기 하나다 — Client/Vec3d.h 는 이것을 swc 로 들여온다.
//  ★ DirectXMath 를 쓰지 않는다 — 서버가 끌려오지 않게(ModelSource.h 머리 주석). XMFLOAT3 변환은 Client/Vec3d.h.
//  ★ 구면 계산은 double 로 한다 (Client/Planet.h 머리 주석 — float 로는 고도가 양자화된다).
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

} // namespace Shared
