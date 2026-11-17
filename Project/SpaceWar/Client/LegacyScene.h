#pragma once
#include <cstdint>
#include <vector>
#include <DirectXMath.h>
#include "Handles.h"
#include "InstanceData.h"

// ============================================================
//  ★ 임시 클래스 — 둘로 나뉜다 (2026-09-18)
//
//  지금 이 한 클래스가 두 가지를 겸하고 있다.
//    · 게임이 소유하는 Scene 노드      → Client/Scene/Scene.h        (메인 소유)
//    · 렌더가 소유하는 Render World    → Client/Render/RenderWorld.h  (렌더 소유)
//  「멀티스레딩 분류 명세서」 7장: «한 스레드일 때는 Scene 이 곧 Render World 역할을
//  했지만 이 문서부터는 둘을 나눈다». 경로는 GameObject → RenderObjectID →
//  Scene 노드 → ④ 렌더 추출 → Render World 다.
//
//  ★ 구조 자체는 유지한다
//  「렌더러 수정방향」 §5 «그대로 둘 것»: Scene 의 SoA 노드와 Extract 구조.
//  3.2 에서 prevWorld 배열이 추가되고 Extract 가 InstanceData 를 채우게 된다(2026-10-11 반영).
//  없애는 것이 아니라 소유자가 갈리는 것이다.
// ============================================================

namespace swc {
	class LegacyScene
	{
	public:
		LegacyScene();

		NodeHandle AddNode(NodeHandle, MeshHandle, MaterialHandle);

		void SetLocalTransform(NodeHandle, const DirectX::XMMATRIX&);
		void SetMesh(NodeHandle, MeshHandle);
		// 2026-10-28 지형 레이어 — 이미 올린 노드의 재질을 바꾼다(그리기 묶음이 바뀌므로 구성 버전을 올린다).
		void SetMaterial(NodeHandle, MaterialHandle);
		void SetVisible(NodeHandle, bool);
		// 2026-11-16 — true 면 이 노드는 TLAS 에 넣지 않는다(Extract 가 kInstanceNoRayTracing 을 붙인다). 자식에게 물려주지 않는다.
		//  그리기 묶음과 무관한 표시라 구성 버전을 올리지 않는다.
		void SetRayTracingExcluded(NodeHandle, bool);
		// 2026-11-17 예시 행성 해제 — root 와 그 아래 노드를 모두 «메시 없음 · 숨김» 으로 비운다(구성 버전을 올린다).
		//  노드 칸은 지우지도 다시 쓰지도 않는다 — Update/Extract 가 «부모가 자식보다 앞 번호» 를 전제로 돌아서다.
		//  그래서 비운 칸은 빈 노드로 남고, 매 프레임 건너뛰는 비용만 든다. 비운 노드 번호를 다른 곳에서 다시 쓰지 말 것.
		void ClearSubtree(NodeHandle root);

		void UpdateWorldTransforms();
		// 렌더 추출 ④ — 보이는 메시 노드의 InstanceData 와 구성 버전을 채운다. 뷰는 호출자가 채운다.
		void Extract(FramePacket&) const;

		size_t NodeCount() const { return parent.size(); }

	private:
		std::vector<NodeHandle>          parent;
		std::vector<DirectX::XMFLOAT4X4> local;
		std::vector<DirectX::XMFLOAT4X4> world;
		std::vector<DirectX::XMFLOAT4X4> prevWorld;    // 이전 프레임 world (수정방향 3.2)
		std::vector<MeshHandle>          mesh;
		std::vector<MaterialHandle>      material;
		std::vector<uint8_t>             visible;
		std::vector<uint8_t>             worldVisible;
		std::vector<uint8_t>             localDirty;
		std::vector<uint8_t>             worldDirty;   // 이번 갱신에서 부모 변경을 자식에게 전달한다.
		std::vector<uint8_t>             prevPending;  // world 가 바뀐 다음 프레임에 prevWorld 를 따라잡는다.
		std::vector<uint32_t>            flags;        // InstanceFlags — world 가 바뀔 때만 다시 정한다.
		std::vector<uint8_t>             rayTracingExcluded;   // 2026-11-16 — 1 이면 TLAS 에 넣지 않는다(SetRayTracingExcluded)

		// FramePacket::layoutVersion = (sceneId << 32) | layoutVersion.
		//  렌더러는 이 값이 바뀔 때만 그리기 묶음을 다시 만든다.
		uint32_t sceneId = 0;
		uint32_t layoutVersion = 0;
	};

}

