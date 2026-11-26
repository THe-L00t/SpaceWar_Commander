#include "Model.h"
#include "GRenderer.h"
// ★ client2 의 `Scene` 은 우리 쪽에서 `LegacyScene` 이다 (09-18 `df71fc9`).
//   게임 소유 Scene 과 렌더 소유 Render World 로 나뉘기 전의 임시 클래스다.
#include "LegacyScene.h"
#include "Shared/GameLogic/PlayerMovement.h"   // 캐릭터 키 = 충돌 캡슐 키 (2026-11-14)
#include <cmath>
#include <utility>

using namespace DirectX;

namespace {
	// 이동 좌표는 몸통 중심이다. 모델의 발은 중심보다 1m 아래다(kGroundOffset).
	// ★ 캐릭터 키 = 충돌 캡슐 키 1.8 m (2026-11-14 사용자 «캐릭터 키 맞춰줘»)
	//   전에는 2.0 m 로 늘려 그려서 판정 캡슐(PlayerMovement::kCapsuleHeight 1.8 m) · 리소스 체크리스트(키 기준 1.8 m)보다 컸고,
	//   실제 크기(1:1)로 그리는 총이 손에 붙으면 10 % 작아 보였다. 캡슐 상수를 그대로 써서 둘이 다시 갈라지지 않게 한다.
	//   이 자동 키 맞춤은 표시 변환 없이 부르는 Initialize(data, renderer) — 지금은 캐릭터(OBJ)만 쓴다.
	//   맵 · 오브젝트 · 총은 표시 변환을 직접 주므로 영향이 없다.
	constexpr float kModelHeight = Shared::PlayerMovement::kCapsuleHeight;
	constexpr float kGroundOffset = 1.0f;
	// ★ 정면 보정 0 (2026-10-08 — 통합 뒤 실행해서 확인했다)
	//   client2 에서는 XM_PI 였다. 그쪽 ObjModelLoader 가 정점의 Z 를 뒤집어 올렸기 때문에
	//   모델 정면이 −Z 로 돌아가고, 그것을 180도 돌려 엔진 전방(+Z)에 맞춘 것이다.
	//   지금 파서(Shared/Model)는 좌표를 변환하지 않는다 → 캐릭터 OBJ 의 정면이 그대로 +Z 다.
	//   그래서 180도를 그대로 두면 이동 방향의 **정반대**를 바라본다(실제로 그랬다).
	//   제작 모델의 정면이 바뀌면 이 값만 조정한다(라디안).
	constexpr float kModelYaw = 0.0f;
}

namespace swc {

	bool Model::Initialize(const ModelData& data, GRenderer& renderer)
	{
		const XMFLOAT3& lo = data.boundsMin;
		const XMFLOAT3& hi = data.boundsMax;
		const float height = hi.y - lo.y;
		if (!std::isfinite(height) || height <= 1.0e-6f)
		{
			lastError = L"모델의 높이가 올바르지 않습니다.";
			return false;
		}
		const float scale = kModelHeight / height;
		const float centerX = lo.x * 0.5f + hi.x * 0.5f;
		const float centerZ = lo.z * 0.5f + hi.z * 0.5f;
		XMFLOAT4X4 placement;
		XMStoreFloat4x4(&placement,
			XMMatrixTranslation(-centerX, -lo.y, -centerZ) *
			XMMatrixScaling(scale, scale, scale) * XMMatrixRotationY(kModelYaw) *
			XMMatrixTranslation(0.0f, -kGroundOffset, 0.0f));
		return Initialize(data, renderer, placement);
	}

	bool Model::Initialize(const ModelData& data, GRenderer& renderer, const XMFLOAT4X4& visualTransform, bool forceBackfaceCulling)
	{
		lastError.clear();
		if (initialized)
		{
			lastError = L"이미 초기화된 모델입니다.";
			return false;
		}
		if (data.meshes.empty() || data.nodes.empty())
		{
			lastError = L"모델에 표시할 메시 또는 노드가 없습니다.";
			return false;
		}
		for (const auto& row : visualTransform.m)
		{
			for (float value : row)
			{
				if (!std::isfinite(value))
				{
					lastError = L"모델 표시 변환이 올바르지 않습니다.";
					return false;
				}
			}
		}
		const float determinant = XMVectorGetX(XMMatrixDeterminant(XMLoadFloat4x4(&visualTransform)));
		if (!std::isfinite(determinant) || std::fabs(determinant) < 1.0e-20f)
		{
			lastError = L"모델 표시 변환의 배율이 올바르지 않습니다.";
			return false;
		}

		const XMFLOAT3& lo = data.boundsMin;
		const XMFLOAT3& hi = data.boundsMax;
		const float height = hi.y - lo.y;
		if (!std::isfinite(lo.x) || !std::isfinite(lo.y) || !std::isfinite(lo.z) ||
			!std::isfinite(hi.x) || !std::isfinite(hi.y) || !std::isfinite(hi.z) ||
			!std::isfinite(height) || height <= 1.0e-6f || hi.x < lo.x || hi.z < lo.z)
		{
			lastError = L"모델의 크기 범위가 올바르지 않습니다.";
			return false;
		}

		// LegacyScene 은 배열 앞쪽의 부모부터 월드 행렬을 계산한다.
		for (size_t i = 0; i < data.nodes.size(); ++i)
		{
			const ModelNodeData& node = data.nodes[i];
			if (node.parent != kInvalidModelIndex && node.parent >= i)
			{
				lastError = L"모델 노드가 부모보다 먼저 배치되어 있습니다.";
				return false;
			}
			for (uint32_t mesh : node.meshes)
			{
				if (mesh >= data.meshes.size())
				{
					lastError = L"모델 노드의 메시 참조가 올바르지 않습니다.";
					return false;
				}
			}
		}
		for (const ModelMeshData& mesh : data.meshes)
		{
			if (mesh.material != kInvalidModelIndex && mesh.material >= data.materials.size())
			{
				lastError = L"모델 메시의 재질 참조가 올바르지 않습니다.";
				return false;
			}
		}
		for (const MaterialData& material : data.materials)
		{
			for (uint32_t texture : material.textures)
			{
				if (texture != kInvalidModelIndex && texture >= data.textures.size())
				{
					lastError = L"모델 재질의 텍스처 참조가 올바르지 않습니다.";
					return false;
				}
			}
		}

		std::vector<TextureHandle> textureHandles;
		textureHandles.reserve(data.textures.size());
		for (const TextureData& texture : data.textures)
		{
			const TextureHandle handle = renderer.CreateTexture(texture);
			if (handle == kInvalidTexture)
			{
				lastError = L"모델 텍스처 생성 실패: " + renderer.StatusText();
				return false;
			}
			textureHandles.push_back(handle);
		}

		std::vector<MaterialHandle> materialHandles;
		materialHandles.reserve(data.materials.size());
		for (const MaterialData& source : data.materials)
		{
			// CPU 메시를 복제하거나 로더 캐시를 수정하지 않고 GPU 재질의 양면 설정만 선택한다.
			MaterialData material = source;
			if (forceBackfaceCulling) material.doubleSided = false;
			std::array<TextureHandle, kMaterialTextureCount> textures;
			textures.fill(kInvalidTexture);
			for (size_t slot = 0; slot < textures.size(); ++slot)
			{
				if (material.textures[slot] != kInvalidModelIndex)
					textures[slot] = textureHandles[material.textures[slot]];
			}
			const MaterialHandle handle = renderer.CreateMaterial(material, textures);
			if (handle == kInvalidMaterial)
			{
				lastError = L"모델 재질 생성 실패: " + renderer.StatusText();
				return false;
			}
			materialHandles.push_back(handle);
		}

		std::vector<MeshHandle> uploadedMeshes;
		std::vector<MaterialHandle> uploadedMaterials;
		uploadedMeshes.reserve(data.meshes.size());
		uploadedMaterials.reserve(data.meshes.size());
		for (const ModelMeshData& mesh : data.meshes)
		{
			const MeshHandle handle = renderer.CreateMesh(
				mesh.mesh.vertices.data(), mesh.mesh.vertices.size(),
				mesh.mesh.indices.data(), mesh.mesh.indices.size());
			if (handle == kInvalidMesh)
			{
				lastError = L"모델 메시 생성 실패: " + renderer.StatusText();
				return false;
			}
			uploadedMeshes.push_back(handle);
			// 렌더러의 0번 재질은 텍스처가 없는 기본 재질이다.
			uploadedMaterials.push_back(mesh.material == kInvalidModelIndex
				? 0 : materialHandles[mesh.material]);
		}

		visualLocal = visualTransform;

		// 노드 이름/계층은 값으로 보관해 향후 애니메이션 확장에 사용할 수 있다.
		nodes = data.nodes;
		nodeLod.assign(nodes.size(), 0);   // 2026-11-20 메시 LOD — 단계 노드 표시
		for (size_t i = 0; i < nodes.size(); ++i)
			nodeLod[i] = static_cast<uint8_t>(LodLevelOfNodeName(nodes[i].name));
		meshes = std::move(uploadedMeshes);
		meshMaterials = std::move(uploadedMaterials);
		// 2026-11-17 — 놓을 때(Release) 쓰려고 이 모델이 만든 텍스처 · 재질 번호를 남긴다(meshMaterials 에는 0번 기본 재질도 섞여 있다).
		textures = std::move(textureHandles);
		materials = std::move(materialHandles);
		initialized = true;
		return true;
	}

	// 2026-11-17 예시 행성 해제 — 이 모델이 만든 GPU 메시 · 텍스처 · 재질을 놓고 빈 모델로 돌아간다.
	//  이 모델로 펼친 장면 노드는 호출자가 먼저 비워야 한다(LegacyScene::ClearSubtree).
	bool Model::Release(GRenderer& renderer)
	{
		if (!renderer.Release(meshes, textures, materials))
		{
			lastError = L"모델 GPU 자원 놓기 실패: " + renderer.StatusText();
			return false;
		}
		nodes.clear();
		nodeLod.clear();
		meshes.clear();
		meshMaterials.clear();
		textures.clear();
		materials.clear();
		initialized = false;
		return true;
	}

	NodeHandle Model::Instantiate(LegacyScene& scene, NodeHandle parent,
		std::vector<std::pair<uint32_t, NodeHandle>>* meshNodes) const
	{
		if (!initialized) return kInvalidNode;

		// 이동 루트와 모델 표시 보정을 분리한다. 이동 행렬을 덮어써도 보정은 유지된다.
		const NodeHandle root = scene.AddNode(parent, kInvalidMesh, kInvalidMaterial);
		const NodeHandle visual = scene.AddNode(root, kInvalidMesh, kInvalidMaterial);
		scene.SetLocalTransform(visual, XMLoadFloat4x4(&visualLocal));

		std::vector<NodeHandle> nodeHandles;
		nodeHandles.reserve(nodes.size());
		for (size_t n = 0; n < nodes.size(); ++n)
		{
			const ModelNodeData& node = nodes[n];
			const NodeHandle nodeParent = node.parent == kInvalidModelIndex
				? visual : nodeHandles[node.parent];
			const NodeHandle handle = scene.AddNode(nodeParent, kInvalidMesh, kInvalidMaterial);
			scene.SetLocalTransform(handle, XMLoadFloat4x4(&node.local));
			nodeHandles.push_back(handle);
			if (n < nodeLod.size() && nodeLod[n] != 0) continue;   // 2026-11-20 LOD 단계 노드는 그리지 않는다(렌더러가 고른다)

			// 한 모델 노드가 여러 재질 부분을 가지면 같은 부모 아래 별도 draw 노드를 둔다.
			for (uint32_t mesh : node.meshes)
			{
				const NodeHandle draw = scene.AddNode(handle, meshes[mesh], meshMaterials[mesh]);
				if (meshNodes) meshNodes->emplace_back(mesh, draw);
			}
		}
		return root;
	}

	// TLAS 목록 분리 (2026-11-17) — Instantiate 가 만드는 노드 사슬과 같은 곱(행 벡터: 월드 = local × 부모 월드).
	//  루트(rootWorld) → 표시 보정(visualLocal) → 모델 노드(부모가 늘 앞 번호 — Initialize 가 검사) → 그리기 노드(단위 행렬).
	void Model::AppendInstances(const XMFLOAT4X4& rootWorld, std::vector<InstanceData>& out) const
	{
		if (!initialized) return;
		const XMMATRIX visualWorld = XMLoadFloat4x4(&visualLocal) * XMLoadFloat4x4(&rootWorld);
		std::vector<XMFLOAT4X4> worlds(nodes.size());
		for (size_t i = 0; i < nodes.size(); ++i)
		{
			const ModelNodeData& node = nodes[i];
			const XMMATRIX parentWorld = node.parent == kInvalidModelIndex ? visualWorld : XMLoadFloat4x4(&worlds[node.parent]);
			const XMMATRIX world = XMLoadFloat4x4(&node.local) * parentWorld;
			XMStoreFloat4x4(&worlds[i], world);
			if (node.meshes.empty()) continue;
			if (i < nodeLod.size() && nodeLod[i] != 0) continue;   // 2026-11-20 LOD 단계 노드는 넣지 않는다(래스터는 렌더러가 고르고, TLAS 는 LOD0)
			const float det = XMVectorGetX(XMMatrixDeterminant(world));
			const uint32_t flags = std::fabs(det) < 1.0e-12f ? kInstanceDegenerate : (det < 0.0f ? kInstanceMirrored : 0u);
			for (const uint32_t mesh : node.meshes)
			{
				InstanceData item;
				item.world = worlds[i];
				item.prevWorld = worlds[i];
				item.mesh = meshes[mesh];
				item.material = meshMaterials[mesh];
				item.flags = flags;
				out.push_back(item);
			}
		}
	}

	// ── 메시 LOD (2026-11-20 render/tlas-select 시험) ──
	int Model::LodLevelOfNodeName(const std::string& name, std::string* base)
	{
		const size_t n = name.size();
		if (n > 5 && name.compare(n - 5, 4, "_LOD") == 0 && name[n - 1] >= '1' && name[n - 1] <= '3')
		{
			if (base) *base = name.substr(0, n - 5);
			return name[n - 1] - '0';
		}
		if (base) *base = name;
		return 0;
	}

	uint32_t Model::BaseMeshCount() const
	{
		uint32_t count = 0;
		for (size_t i = 0; i < nodes.size(); ++i)
			if (i >= nodeLod.size() || nodeLod[i] == 0)
				count += static_cast<uint32_t>(nodes[i].meshes.size());
		return count;
	}

	uint32_t Model::RegisterLods(GRenderer& renderer, const XMFLOAT3& objectCenter, const XMFLOAT3& objectExtents) const
	{
		if (!initialized) return 0;
		auto isIdentity = [](const XMFLOAT4X4& m)
		{
			const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
			const float* p = &m._11;
			for (int k = 0; k < 16; ++k)
				if (std::fabs(p[k] - id[k]) > 1.0e-6f) return false;
			return true;
		};
		uint32_t registered = 0;
		for (size_t i = 0; i < nodes.size(); ++i)
		{
			if (nodeLod[i] != 0 || nodes[i].meshes.empty()) continue;
			// 단계 노드 찾기 — «이름_LOD1» ~ «_LOD3», 같은 부모 · 단위 변환 · 같은 메시 수 · 같은 재질 순서
			int found[3] = { -1, -1, -1 };
			for (size_t j = 0; j < nodes.size(); ++j)
			{
				std::string base;
				const int level = LodLevelOfNodeName(nodes[j].name, &base);
				if (level == 0 || base != nodes[i].name) continue;
				if (nodes[j].parent != nodes[i].parent || nodes[j].meshes.size() != nodes[i].meshes.size()) continue;
				if (!isIdentity(nodes[j].local) || !isIdentity(nodes[i].local)) continue;
				bool sameMaterials = true;
				for (size_t p = 0; p < nodes[i].meshes.size(); ++p)
					sameMaterials = sameMaterials && meshMaterials[nodes[j].meshes[p]] == meshMaterials[nodes[i].meshes[p]];
				if (sameMaterials) found[level - 1] = int(j);
			}
			uint32_t levels = 0;
			while (levels < 3 && found[levels] >= 0) ++levels;   // LOD1 부터 끊김 없이 있는 데까지
			if (levels == 0) continue;
			for (size_t p = 0; p < nodes[i].meshes.size(); ++p)
			{
				MeshHandle lods[3] = { kInvalidMesh, kInvalidMesh, kInvalidMesh };
				for (uint32_t k = 0; k < levels; ++k)
					lods[k] = meshes[nodes[size_t(found[k])].meshes[p]];
				renderer.SetMeshLods(meshes[nodes[i].meshes[p]], lods, levels, objectCenter, objectExtents);
				++registered;
			}
		}
		return registered;
	}
}
