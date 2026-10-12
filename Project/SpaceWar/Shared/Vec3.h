#pragma once

// ============================================================
//  Shared/Vec3.h — 공용 수학 타입 (Vec2 · Vec3 · Vec4 · Mat4)
//
//  클라와 서버가 같은 좌표 타입을 써야 하므로 Shared 에 둔다.
//  DirectXMath 에 의존하지 않는다 — 서버는 DirectX 를 모른다.
//  단위는 Units.h 규약을 따른다 (길이 m).
//
//  연산자는 아직 없다. 이동·판정 로직을 붙일 때 필요한 것만 추가한다.
//
//  ★ 2026-10-05: Vec2 · Vec4 · Mat4 추가 (모델 파서용)
//    모델 파서를 Shared 에 두기로 해서(서버가 충돌·높이를 읽는다) UV·색·행렬 타입도
//    Shared 에 있어야 한다. DirectXMath 의 XMFLOAT2/4/4X4 와 메모리 배치가 같으므로
//    Client 쪽에서 그대로 복사해 올릴 수 있다 (Resource/ModelBuilder 담당).
// ============================================================

namespace Shared {

	struct Vec2
	{
		float x = 0.0f;
		float y = 0.0f;
	};

	struct Vec3
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	struct Vec4
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		float w = 0.0f;
	};

	// 행 우선(row-major). DirectXMath 와 같은 규약이라 그대로 올려 쓸 수 있다.
	struct Mat4
	{
		float m[16] = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f };
	};

} // namespace Shared
