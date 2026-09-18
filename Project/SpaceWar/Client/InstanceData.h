#pragma once
#include <cstdint>
#include <DirectXMath.h>
#include "Handles.h"

// ============================================================
//  Client/InstanceData.h — 「렌더러 수정방향」 3.2 · 3.5
//
//  ★ 이름을 노션 기준으로 맞췄다 (RenderItem → InstanceData, 2026-09-18)
//    3.2 의 목표 형태는 world · prevWorld · mesh · material · flags 이고,
//    Scene 의 Extract 가 이 배열을 채워 프레임 패킷(FramePacket)으로 넘긴다.
//    아래는 아직 확장 전 모습이다 — 필드는 3.2 를 할 때 바꾼다.
//
//  ★ RenderView 는 3.5 에서 확장된다
//    view · proj · viewProj · prevViewProj · invProj · 지터 · 렌더/출력 크기 ·
//    시간 · 프레임 번호 · 카메라 원점(Vec3d) · 태양 방향까지.
//    그때 C++ 와 HLSL 이 함께 include 하는 공유 상수 헤더로 옮긴다.
// ============================================================

namespace swc {
	struct InstanceData {
		uint32_t sortKey = 0;
		NodeHandle node = kInvalidNode;
		MeshHandle mesh = kInvalidMesh;
		MaterialHandle material = kInvalidMaterial;
	};

	struct RenderView {
		DirectX::XMFLOAT4X4 viewProj{ };
		DirectX::XMFLOAT3   eyePosition{ };   // 프레넬의 시선 벡터(V) 계산용
	};
}
