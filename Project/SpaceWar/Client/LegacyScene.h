#pragma once
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
//  3.2 에서 prevWorld 배열이 추가되고 Extract 가 InstanceData 를 채우게 된다.
//  없애는 것이 아니라 소유자가 갈리는 것이다.
// ============================================================

namespace swc {
	class LegacyScene
	{
	public:
		NodeHandle AddNode(NodeHandle, MeshHandle, MaterialHandle);

		void SetLocalTransform(NodeHandle, const DirectX::XMMATRIX&);
		void SetMesh(NodeHandle, MeshHandle);

		void UpdateWorldTransforms();
		void Extract(std::vector<InstanceData>&) const;

		size_t NodeCount() const { return parent.size(); }
		const DirectX::XMFLOAT4X4* WorldData() const { return world.data(); }

	private:
		std::vector<NodeHandle>          parent;
		std::vector<DirectX::XMFLOAT4X4> local;
		std::vector<DirectX::XMFLOAT4X4> world;
		std::vector<MeshHandle>          mesh;
		std::vector<MaterialHandle>      material;
	};

}

