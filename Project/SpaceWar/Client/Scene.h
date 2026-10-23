#pragma once
#include <vector>
#include <DirectXMath.h>
#include "Handles.h"
#include "InstanceData.h"

namespace swc {
	class Scene
	{
	public:
		NodeHandle AddNode(NodeHandle, MeshHandle, MaterialHandle);

		void SetLocalTransform(NodeHandle, const DirectX::XMMATRIX&);
		void SetMesh(NodeHandle, MeshHandle);

		void UpdateWorldTransforms();
		// 렌더 추출 ④ — 메시 노드의 InstanceData 와 구성 버전을 채운다. 뷰는 호출자가 채운다.
		void Extract(FramePacket&) const;

		size_t NodeCount() const { return parent.size(); }

	private:
		std::vector<NodeHandle>          parent;
		std::vector<DirectX::XMFLOAT4X4> local;
		std::vector<DirectX::XMFLOAT4X4> world;
		std::vector<DirectX::XMFLOAT4X4> prevWorld;   // 이전 프레임 world (수정방향 3.2)
		std::vector<MeshHandle>          mesh;
		std::vector<MaterialHandle>      material;
		std::vector<uint32_t>            flags;       // InstanceFlags

		uint64_t layoutVersion = 0;   // 렌더러는 이 값이 바뀔 때만 그리기 묶음을 다시 만든다
	};

}

