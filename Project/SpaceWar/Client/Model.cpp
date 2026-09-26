#include "Model.h"
#include "GRenderer.h"
#include "Scene.h"
#include <cmath>
#include <utility>

using namespace DirectX;

namespace {
	// 이동 좌표는 기존 2m 큐브의 중심을 유지한다. 모델의 발은 중심보다 1m 아래다.
	constexpr float kModelHeight = 2.0f;
	constexpr float kGroundOffset = 1.0f;
	// 엔진의 정면은 +Z. 제작 모델의 정면이 다르면 이 값만 조정한다(라디안).
	constexpr float kModelYaw = 0.0f;
}

namespace swc {

	bool Model::Initialize(const ModelData& data, GRenderer& renderer)
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

		// Scene은 배열 앞쪽의 부모부터 월드 행렬을 계산한다.
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
		for (const MaterialData& material : data.materials)
		{
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

		const float scale = kModelHeight / height;
		const float centerX = lo.x * 0.5f + hi.x * 0.5f;
		const float centerZ = lo.z * 0.5f + hi.z * 0.5f;
		XMStoreFloat4x4(&visualLocal,
			XMMatrixTranslation(-centerX, -lo.y, -centerZ) *
			XMMatrixScaling(scale, scale, scale) * XMMatrixRotationY(kModelYaw) *
			XMMatrixTranslation(0.0f, -kGroundOffset, 0.0f));

		// 노드 이름/계층은 값으로 보관해 향후 애니메이션 확장에 사용할 수 있다.
		nodes = data.nodes;
		meshes = std::move(uploadedMeshes);
		meshMaterials = std::move(uploadedMaterials);
		initialized = true;
		return true;
	}

	NodeHandle Model::Instantiate(Scene& scene, NodeHandle parent) const
	{
		if (!initialized) return kInvalidNode;

		// 이동 루트와 모델 표시 보정을 분리한다. 이동 행렬을 덮어써도 보정은 유지된다.
		const NodeHandle root = scene.AddNode(parent, kInvalidMesh, kInvalidMaterial);
		const NodeHandle visual = scene.AddNode(root, kInvalidMesh, kInvalidMaterial);
		scene.SetLocalTransform(visual, XMLoadFloat4x4(&visualLocal));

		std::vector<NodeHandle> nodeHandles;
		nodeHandles.reserve(nodes.size());
		for (const ModelNodeData& node : nodes)
		{
			const NodeHandle nodeParent = node.parent == kInvalidModelIndex
				? visual : nodeHandles[node.parent];
			const NodeHandle handle = scene.AddNode(nodeParent, kInvalidMesh, kInvalidMaterial);
			scene.SetLocalTransform(handle, XMLoadFloat4x4(&node.local));
			nodeHandles.push_back(handle);

			// 한 FBX 노드가 여러 재질 부분을 가지면 같은 부모 아래 별도 draw 노드를 둔다.
			for (uint32_t mesh : node.meshes)
				scene.AddNode(handle, meshes[mesh], meshMaterials[mesh]);
		}
		return root;
	}
}
