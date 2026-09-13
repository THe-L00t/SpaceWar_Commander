#pragma once

// ============================================================
//  Shared/Vec3.h — 공용 3차원 벡터
//
//  클라와 서버가 같은 좌표 타입을 써야 하므로 Shared 에 둔다.
//  DirectXMath 에 의존하지 않는다 — 서버는 DirectX 를 모른다.
//  단위는 Units.h 규약을 따른다 (길이 m).
//
//  연산자는 아직 없다. 이동·판정 로직을 붙일 때 필요한 것만 추가한다.
// ============================================================

namespace Shared {

	struct Vec3
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

} // namespace Shared
