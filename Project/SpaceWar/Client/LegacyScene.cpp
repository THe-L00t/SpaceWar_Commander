#include "LegacyScene.h"

using namespace DirectX;

namespace swc {
	NodeHandle LegacyScene::AddNode(NodeHandle parent, MeshHandle mesh, MaterialHandle material)
	{
		const NodeHandle idx = static_cast<NodeHandle>(this->parent.size());

		XMFLOAT4X4 identity;
		XMStoreFloat4x4(&identity, XMMatrixIdentity());

		this->parent.push_back(parent);
		this->local.push_back(identity);
		this->world.push_back(identity);
		this->mesh.push_back(mesh);
		this->material.push_back(material);
		return idx;
	}

	void LegacyScene::SetLocalTransform(NodeHandle n, const XMMATRIX& local)
	{
		XMStoreFloat4x4(&this->local[n], local);
	}

	void LegacyScene::SetMesh(NodeHandle n, MeshHandle mesh)
	{
		this->mesh[n] = mesh;
	}

	void LegacyScene::UpdateWorldTransforms()
	{
		const size_t count = parent.size();
		for (size_t i = 0; i < count; ++i)
		{
			const XMMATRIX localMat = XMLoadFloat4x4(&local[i]);
			const NodeHandle p = parent[i];
			if (p == kInvalidNode)
			{
				XMStoreFloat4x4(&world[i], localMat);
			}
			else
			{
				XMStoreFloat4x4(&world[i], localMat * XMLoadFloat4x4(&world[p]));
			}
		}
	}

	void LegacyScene::Extract(std::vector<InstanceData>& out) const
	{
		out.clear();
		const size_t count = parent.size();
		for (size_t i = 0; i < count; ++i)
		{
			if (mesh[i] == kInvalidMesh) continue;
			InstanceData item;
			item.node = static_cast<NodeHandle>(i);
			item.mesh = mesh[i];
			item.material = material[i];
			out.push_back(item);
		}
	}
}
