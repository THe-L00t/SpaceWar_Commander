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
		this->visible.push_back(1);
		this->worldVisible.push_back(1);
		this->localDirty.push_back(1);
		this->worldDirty.push_back(1);
		return idx;
	}

	void LegacyScene::SetLocalTransform(NodeHandle n, const XMMATRIX& local)
	{
		XMStoreFloat4x4(&this->local[n], local);
		this->localDirty[n] = 1;
	}

	void LegacyScene::SetMesh(NodeHandle n, MeshHandle mesh)
	{
		this->mesh[n] = mesh;
	}

	void LegacyScene::SetVisible(NodeHandle n, bool visible)
	{
		if (n >= this->visible.size()) return;
		const uint8_t value = visible ? 1 : 0;
		if (this->visible[n] == value) return;
		this->visible[n] = value;
		this->localDirty[n] = 1;
	}

	void LegacyScene::UpdateWorldTransforms()
	{
		const size_t count = parent.size();
		for (size_t i = 0; i < count; ++i)
		{
			// 부모가 먼저 추가되는 기존 SoA 순서로 변경을 전달한다.
			// 정적 맵은 첫 갱신 뒤 행렬·가시성을 그대로 사용한다.
			const NodeHandle p = parent[i];
			worldDirty[i] = (localDirty[i] || (p != kInvalidNode && worldDirty[p])) ? 1 : 0;
			localDirty[i] = 0;
			if (!worldDirty[i]) continue;
			const XMMATRIX localMat = XMLoadFloat4x4(&local[i]);
			if (p == kInvalidNode)
			{
				XMStoreFloat4x4(&world[i], localMat);
				worldVisible[i] = visible[i];
			}
			else
			{
				XMStoreFloat4x4(&world[i], localMat * XMLoadFloat4x4(&world[p]));
				worldVisible[i] = (visible[i] && worldVisible[p]) ? 1 : 0;
			}
		}
	}

	void LegacyScene::Extract(std::vector<InstanceData>& out) const
	{
		out.clear();
		const size_t count = parent.size();
		for (size_t i = 0; i < count; ++i)
		{
			if (mesh[i] == kInvalidMesh || !worldVisible[i]) continue;
			InstanceData item;
			item.node = static_cast<NodeHandle>(i);
			item.mesh = mesh[i];
			item.material = material[i];
			out.push_back(item);
		}
	}
}
