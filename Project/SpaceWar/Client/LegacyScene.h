#pragma once
#include <vector>
#include <DirectXMath.h>
#include "Handles.h"
#include "RenderItem.h"

// ============================================================
//  ★ 없앨 클래스 (2026-09-18)
//
//  아키텍처 명세서를 적용할 때 Render World(Client/Render/RenderWorld.h)로 대체된다.
//  구조가 많이 달라질 예정이라 지금은 그대로 두고 이름만 바꿔 «임시» 임을 표시한다.
//  새 코드는 이 클래스에 기대지 말 것.
// ============================================================

namespace swc {
	class LegacyScene
	{
	public:
		NodeHandle AddNode(NodeHandle, MeshHandle, MaterialHandle);

		void SetLocalTransform(NodeHandle, const DirectX::XMMATRIX&);
		void SetMesh(NodeHandle, MeshHandle);

		void UpdateWorldTransforms();
		void Extract(std::vector<RenderItem>&) const;

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

