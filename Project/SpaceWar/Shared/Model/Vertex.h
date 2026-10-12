#pragma once
#include <cstddef>
#include <cstdint>
#include "../Vec3.h"
#include "ModelFormat.h"

// ============================================================
//  Shared/Model/Vertex.h — 렌더 정점 (파서가 만들고, 그대로 GPU 까지 간다)
//
//  ★ 파서가 처음부터 D3D 입력 배치로 만든다 (2026-10-09)
//    예전에는 파서가 SourceVertex(84B, 스킨 포함)를 만들고 Client 의 ModelBuilder 가
//    swc::Vertex(60B)로 한 개씩 복사했다. 행성 OBJ(정점 약 1천만)에서 두 벌이 동시에 떠
//    수백 MB 가 겹쳤다. 이제 이 구조체 하나가 파서 출력이자 Resource Manager 의 RAM 사본이자
//    GPU 정점 버퍼의 한 원소다. 중간 복사 없이 std::move 로 넘어간다.
//  ★ 자료형은 우리 것(Vec2·Vec3·Vec4)이다 — Shared 는 DirectX 를 모른다(서버도 이 파서를 쓴다).
//    대신 바이트 배치를 D3D 입력 레이아웃과 똑같이 고정한다. 아래 static_assert 가 지킨다.
//    GRenderer 의 입력 레이아웃은 offsetof(Vertex, …) 로 잡혀 있어 이 배치를 그대로 읽는다.
//  ★ 스킨은 정점에 넣지 않는다 — 정적 메시(행성·지형)가 24B 씩 더 무거워진다.
//    스킨 메시일 때만 메시 옆 SkinVertex 배열에 둔다(GPU 두 번째 정점 스트림 자리).
// ============================================================

namespace Shared {

	struct Vertex
	{
		Vec3 position{};                          // POSITION  R32G32B32_FLOAT      offset 0
		Vec3 normal{};                            // NORMAL    R32G32B32_FLOAT      offset 12
		Vec3 color{ 1.0f, 1.0f, 1.0f };           // COLOR     R32G32B32_FLOAT      offset 24
		Vec2 uv{};                                // TEXCOORD  R32G32_FLOAT         offset 36
		Vec4 tangent{ 1.0f, 0.0f, 0.0f, 1.0f };   // TANGENT   R32G32B32A32_FLOAT   offset 44 (w = 종법선 부호)
	};

	static_assert(sizeof(Vertex) == 60, "Vertex 는 D3D 입력 배치 그대로 60바이트여야 한다");
	static_assert(offsetof(Vertex, position) == 0, "POSITION 오프셋");
	static_assert(offsetof(Vertex, normal) == 12, "NORMAL 오프셋");
	static_assert(offsetof(Vertex, color) == 24, "COLOR 오프셋");
	static_assert(offsetof(Vertex, uv) == 36, "TEXCOORD 오프셋");
	static_assert(offsetof(Vertex, tangent) == 44, "TANGENT 오프셋");

	// 스키닝 — glTF JOINTS_0 / WEIGHTS_0. weights 합은 리더에서 1 로 정규화한다.
	struct SkinVertex
	{
		uint16_t joints[kJointsPerVertex]{};
		float    weights[kJointsPerVertex]{};
	};

} // namespace Shared
