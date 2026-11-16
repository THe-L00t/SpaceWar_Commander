#pragma once
#include <DirectXMath.h>
#include "Shared/Vec3d.h"

// 구면 계산 전용 double 벡터. DirectXMath 에는 double 타입이 없다.
//
// ★ 2026-10-29 — 정의를 Shared/Vec3d.h 로 옮겼다(플레이어 이동을 서버와 같이 쓰려고, 노션 「충돌 처리 ②」 3절).
//   클라 코드는 지금처럼 swc::Vec3d 로 쓴다. 여기 남은 것은 DirectXMath 변환뿐이다 — Shared 는 DirectX 를 모른다.
//   예전 멤버 함수 v.ToFloat3() 는 자유 함수 ToFloat3(v) 가 됐다.
namespace swc {

	using Shared::Vec3d;
	using Shared::operator+;
	using Shared::operator-;
	using Shared::operator*;
	using Shared::Dot;
	using Shared::Length;
	using Shared::Cross;
	using Shared::Normalize;
	using Shared::ProjectOntoPlane;
	using Shared::RotateAround;

	inline DirectX::XMFLOAT3 ToFloat3(const Vec3d& v)
	{
		return { float(v.x), float(v.y), float(v.z) };
	}
}
