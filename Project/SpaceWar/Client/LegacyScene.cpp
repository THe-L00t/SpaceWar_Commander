#include "LegacyScene.h"
#include <atomic>
#include <cmath>

using namespace DirectX;

namespace {
	// 장면마다 다른 번호 — 하늘 행성 장면과 게임 장면의 구성 버전이 섞이지 않게 한다.
	std::atomic<uint32_t> nextSceneId{ 1 };

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
	LegacyScene::LegacyScene()
		: sceneId(nextSceneId.fetch_add(1))
	{
	}

	NodeHandle LegacyScene::AddNode(NodeHandle parent, MeshHandle mesh, MaterialHandle material)
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
		this->visible.push_back(1);
		this->worldVisible.push_back(1);
		this->localDirty.push_back(1);
		this->worldDirty.push_back(1);
		this->prevPending.push_back(0);
		this->flags.push_back(0);
		++layoutVersion;
		return idx;
	}

	void LegacyScene::SetLocalTransform(NodeHandle n, const XMMATRIX& local)
	{
		XMStoreFloat4x4(&this->local[n], local);
		this->localDirty[n] = 1;
	}

	void LegacyScene::SetMesh(NodeHandle n, MeshHandle mesh)
	{
		if (this->mesh[n] == mesh) return;
		this->mesh[n] = mesh;
		++layoutVersion;
	}

	void LegacyScene::SetMaterial(NodeHandle n, MaterialHandle material)
	{
		if (n >= this->material.size() || this->material[n] == material) return;
		this->material[n] = material;
		++layoutVersion;   // 렌더러는 구성 버전이 바뀔 때만 (PSO, 재질, 메시) 묶음을 다시 만든다
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
		bool layoutChanged = false;
		for (size_t i = 0; i < count; ++i)
		{
			// 부모가 먼저 추가되는 기존 SoA 순서로 변경을 전달한다.
			// 정적 맵은 첫 갱신 뒤 행렬·가시성을 그대로 사용한다.
			const NodeHandle p = parent[i];
			worldDirty[i] = (localDirty[i] || (p != kInvalidNode && worldDirty[p])) ? 1 : 0;
			localDirty[i] = 0;
			if (!worldDirty[i])
			{
				// 지난 프레임에 움직였다면 이번 프레임에 prevWorld 가 world 를 따라잡는다.
				if (prevPending[i])
				{
					prevWorld[i] = world[i];
					prevPending[i] = 0;
				}
				continue;
			}
			prevWorld[i] = world[i];
			prevPending[i] = 1;

			const uint8_t wasVisible = worldVisible[i];
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
			const uint32_t nodeFlags = FlagsFor(world[i]);
			// 보이는지·반전·퇴화가 바뀌면 그리기 묶음 구성이 바뀐다.
			if (mesh[i] != kInvalidMesh && (wasVisible != worldVisible[i] || nodeFlags != flags[i]))
				layoutChanged = true;
			flags[i] = nodeFlags;
		}
		if (layoutChanged) ++layoutVersion;
	}

	void LegacyScene::Extract(FramePacket& out) const
	{
		out.instances.clear();
		out.layoutVersion = (uint64_t(sceneId) << 32) | layoutVersion;
		const size_t count = parent.size();
		for (size_t i = 0; i < count; ++i)
		{
			if (mesh[i] == kInvalidMesh || !worldVisible[i]) continue;
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
