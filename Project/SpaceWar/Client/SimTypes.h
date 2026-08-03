#pragma once
#include <DirectXMath.h>
#include "Shared/Sim/PlayerMotion.h"

// ============================================================
//  SimTypes.h — Shared 의 시뮬레이션 타입을 클라 네임스페이스로 끌어온다
//
//  Vec3d / Planet / TerrainSampler 는 원래 Client 에 있었지만
//  서버가 같은 계산을 해야 해서 Shared 로 옮겼다.
//  기존 클라 코드가 swc::Vec3d 로 쓰고 있으므로 여기서 별칭만 걸어준다.
//
//  ※ Dot / Cross / Normalize 같은 자유 함수는 별칭이 필요 없다.
//    인자가 Shared::Vec3d 라서 ADL(인자 기반 탐색)로 자동으로 찾아진다.
//
//  ★ XMFLOAT3 변환이 여기 있는 이유
//    Shared 는 DirectX 를 몰라야 한다(서버는 그래픽이 없다).
//    그래서 변환은 클라 전용인 이 파일에 둔다.
// ============================================================

namespace swc {

	using Shared::Vec3d;
	using Shared::Planet;
	using Shared::TerrainSampler;
	using Shared::TerrainConfig;
	using Shared::MoveInput;
	using Shared::MotionState;

	inline DirectX::XMFLOAT3 ToFloat3(const Vec3d& v)
	{
		return { float(v.x), float(v.y), float(v.z) };
	}
	inline Vec3d FromFloat3(const DirectX::XMFLOAT3& v)
	{
		return { double(v.x), double(v.y), double(v.z) };
	}
	inline Vec3d FromArray3(const float* p)
	{
		return { double(p[0]), double(p[1]), double(p[2]) };
	}
	inline void ToArray3(const Vec3d& v, float* p)
	{
		p[0] = float(v.x); p[1] = float(v.y); p[2] = float(v.z);
	}
}
