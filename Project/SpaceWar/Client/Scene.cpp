#include "Scene.h"
#include <cmath>

using namespace DirectX;

namespace {
	uint32_t FlagsFor(const XMFLOAT4X4& m)
	{
		// 3x3 행렬식만 본다 — 이동 성분은 와인딩·퇴화와 무관하다.
		const float det =
			m._11 * (m._22 * m._33 - m._23 * m._32) -
			m._12 * (m._21 * m._33 - m._23 * m._31) +
			m._13 * (m._21 * m._32 - m._22 * m._31);
		if (!std::isfinite(det) || std::fabs(det) < 1e-12f) return swc::kInstanceDegenerate;
		return det < 0.0f ? swc::kInstanceMirrored : 0u;
	}
}

namespace swc {
	NodeHandle Scene::AddNode(NodeHandle parent, MeshHandle mesh, MaterialHandle material)
	{
		const NodeHandle idx = static_cast<NodeHandle>(this->parent.size());

		XMFLOAT4X4 identity;
		XMStoreFloat4x4(&identity, XMMatrixIdentity());

		this->parent.push_back(parent);
		this->local.push_back(identity);
		this->world.push_back(identity);
		this->prevWorld.push_back(identity);
		this->mesh.push_back(mesh);
		this->material.push_back(material);
		this->flags.push_back(0);
		++layoutVersion;
		return idx;
	}

	void Scene::SetLocalTransform(NodeHandle n, const XMMATRIX& local)
	{
		XMStoreFloat4x4(&this->local[n], local);
	}

	void Scene::SetMesh(NodeHandle n, MeshHandle mesh)
	{
		if (this->mesh[n] == mesh) return;
		this->mesh[n] = mesh;
		++layoutVersion;
	}

	void Scene::UpdateWorldTransforms()
	{
		const size_t count = parent.size();
		bool layoutChanged = false;
		for (size_t i = 0; i < count; ++i)
		{
			// 이 장면은 매 프레임 전체를 다시 계산한다 — 직전 값이 곧 이전 프레임 world 다.
			prevWorld[i] = world[i];
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
			const uint32_t nodeFlags = FlagsFor(world[i]);
			if (mesh[i] != kInvalidMesh && nodeFlags != flags[i])
				layoutChanged = true;
			flags[i] = nodeFlags;
		}
		if (layoutChanged) ++layoutVersion;
	}

	void Scene::Extract(FramePacket& out) const
	{
		out.instances.clear();
		out.layoutVersion = layoutVersion;
		const size_t count = parent.size();
		for (size_t i = 0; i < count; ++i)
		{
			if (mesh[i] == kInvalidMesh) continue;
			InstanceData item;
			item.world = world[i];
			item.prevWorld = prevWorld[i];
			item.mesh = mesh[i];
			item.material = material[i];
			item.flags = flags[i];
			out.instances.push_back(item);
		}
	}
}
