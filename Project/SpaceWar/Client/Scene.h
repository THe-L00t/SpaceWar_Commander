#pragma once
#include <vector>
#include <DirectXMath.h>
#include "Handles.h"
#include "RenderItem.h"

namespace swc {
	class Scene
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

