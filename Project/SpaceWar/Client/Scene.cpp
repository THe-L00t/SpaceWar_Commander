#include "Scene.h"

using namespace DirectX;

namespace swc {
	NodeHandle Scene::AddNode(NodeHandle parent, MeshHandle mesh, MaterialHandle material)
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

	void Scene::SetLocalTransform(NodeHandle n, const XMMATRIX& local)
	{
		XMStoreFloat4x4(&this->local[n], local);
	}

	void Scene::SetMesh(NodeHandle n, MeshHandle mesh)
	{
		this->mesh[n] = mesh;
	}

	void Scene::UpdateWorldTransforms()
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

	void Scene::Extract(std::vector<RenderItem>& out) const
	{
		out.clear();
		const size_t count = parent.size();
		for (size_t i = 0; i < count; ++i)
		{
			if (mesh[i] == kInvalidMesh) continue;
			RenderItem item;
			item.node = static_cast<NodeHandle>(i);
			item.mesh = mesh[i];
			item.material = material[i];
			out.push_back(item);
		}
	}
}
