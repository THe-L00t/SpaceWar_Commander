#pragma once
#include <DirectXMath.h>
#include "Shared/Model/Vertex.h"

namespace swc {
	// ★ 렌더 정점은 Shared 의 Vertex 하나다 (2026-10-09)
	//   모델 파서가 이 배치(60B, D3D 입력 레이아웃)로 바로 만들고, Resource Manager 의 RAM 사본도
	//   GPU 정점 버퍼도 같은 타입이다 — 중간 복사가 없다. 배치는 Shared/Model/Vertex.h 의
	//   static_assert 가 지킨다. 무텍스처 지형도 tangent 가 항상 초기화돼 있다(기본값 (1,0,0,1)).
	using Vertex = Shared::Vertex;

	// DirectXMath 값을 정점 필드(Shared 타입)에 넣을 때 쓴다. 절차적 메시(DummyMesh)용.
	inline Shared::Vec3 ToVec3(const DirectX::XMFLOAT3& v) { return { v.x, v.y, v.z }; }
}
