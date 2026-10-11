#pragma once
#include <cstdint>
#include <vector>
#include <DirectXMath.h>
#include "Handles.h"

// ============================================================
//  렌더러 입력 — 「렌더러 수정방향」 3.2 (2026-10-11, integrate-client2 5b17e73 이식)
//    InstanceData = world · prevWorld · mesh · material · flags.
//    Scene 의 Extract 가 이 배열을 채워 프레임 패킷(FramePacket)으로 넘기고,
//    렌더러는 패킷만 받는다 — Scene 을 직접 읽지 않는다(멀티스레딩 명세 M2).
//  ※ integrate-client2 에서는 이 파일 이름이 InstanceData.h 다(09-18 이름 정리).
// ============================================================

namespace swc {
	// InstanceData::flags — Extract 가 노드의 월드 행렬에서 정한다.
	enum InstanceFlags : uint32_t
	{
		kInstanceMirrored   = 1u << 0,   // 행렬식 < 0 — 와인딩이 뒤집힌다(이 브랜치는 PSO 가 하나라 아직 쓰지 않는다)
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
	};

	// 렌더 추출 ④ 의 결과 — 렌더러가 받는 유일한 입력 (수정방향 3.2, 멀티스레딩 명세 §6 ④)
	struct FramePacket {
		RenderView view;
		std::vector<InstanceData> instances;
		// 인스턴스의 «구성» 버전(수정방향 3.2 «버전 번호»). 노드 추가·메시 변경·반전/퇴화 여부가
		// 바뀔 때만 오른다. 렌더러는 이 값이 같으면 그리기 묶음을 다시 만들지 않는다.
		uint64_t layoutVersion = 0;
	};
}
