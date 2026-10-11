#pragma once
#include <cstdint>
#include <vector>
#include <DirectXMath.h>
#include "Handles.h"

// ============================================================
//  Client/InstanceData.h — 「렌더러 수정방향」 3.2 · 3.5
//
//  ★ 이름을 노션 기준으로 맞췄다 (RenderItem → InstanceData, 2026-09-18)
//  ★ 3.2 의 목표 형태로 바꿨다 (2026-10-11)
//    InstanceData = world · prevWorld · mesh · material · flags.
//    Scene 의 Extract 가 이 배열을 채워 프레임 패킷(FramePacket)으로 넘기고,
//    렌더러는 패킷만 받는다 — Scene 을 직접 읽지 않는다(멀티스레딩 명세 M2).
//
//  ★ RenderView 는 3.5 에서 확장된다
//    view · proj · viewProj · prevViewProj · invProj · 지터 · 렌더/출력 크기 ·
//    시간 · 프레임 번호 · 카메라 원점(Vec3d) · 태양 방향까지.
//    그때 C++ 와 HLSL 이 함께 include 하는 공유 상수 헤더로 옮긴다.
// ============================================================

namespace swc {
	// InstanceData::flags — Extract 가 노드의 월드 행렬에서 정한다.
	enum InstanceFlags : uint32_t
	{
		kInstanceMirrored   = 1u << 0,   // 행렬식 < 0 — 와인딩이 뒤집혀 반전 PSO 로 그린다
		kInstanceDegenerate = 1u << 1,   // 행렬식 ≈ 0 — 그리지 않는다
	};

	struct InstanceData {
		DirectX::XMFLOAT4X4 world{ };
		DirectX::XMFLOAT4X4 prevWorld{ };   // 이전 프레임 world — 모션 벡터용(아직 읽는 셰이더 없음)
		MeshHandle mesh = kInvalidMesh;
		MaterialHandle material = kInvalidMaterial;
		uint32_t flags = 0;
	};

	struct RenderView {
		DirectX::XMFLOAT4X4 viewProj{ };
		DirectX::XMFLOAT3   eyePosition{ };   // 프레넬의 시선 벡터(V) 계산용
		bool background = false;             // 배경 패스 뒤 깊이만 초기화한다.
	};

	// 렌더 추출 ④ 의 결과 — 렌더러가 받는 유일한 입력 (수정방향 3.2, 멀티스레딩 명세 §6 ④)
	//  광원·이미터·데칼 배열은 해당 항목(3.12 등)을 할 때 붙는다.
	struct FramePacket {
		RenderView view;
		std::vector<InstanceData> instances;
		// 인스턴스의 «구성» 버전(수정방향 3.2 «버전 번호»). 노드 추가·메시 변경·가시성·반전 여부가
		// 바뀔 때만 오른다. 렌더러는 이 값이 같으면 그리기 묶음을 다시 만들지 않는다.
		//  상위 32비트 = 장면 번호, 하위 32비트 = 장면 안의 버전.
		uint64_t layoutVersion = 0;
	};
}
