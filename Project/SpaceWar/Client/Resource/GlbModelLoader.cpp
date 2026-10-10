#include "GlbModelLoader.h"
#include "TextureLoader.h"
#include "Shared/Resource/GlbDocument.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

using namespace DirectX;

namespace {

	bool HiddenNode(const std::string& name)
	{
		// 충돌 프록시와 제작 안내·주석은 계층만 유지하고 화면에 표시하지 않는다.
		constexpr const char* prefixes[] = {
			"COL_", "Guide_", "Future_", "Landing_Label_", "Landing_Region_Label_",
			"Objective_Future_", "Planned_Risk_", "Reserved_Objective_" };
		for (const char* prefix : prefixes)
			if (name.rfind(prefix, 0) == 0) return true;
		return name == "Character_Scale_1_8m";
	}

	struct MeshBounds
	{
		XMFLOAT3 lo{};
		XMFLOAT3 hi{};
	};

	XMFLOAT4X4 Matrix(const std::array<float, 16>& values)
	{
		XMFLOAT4X4 result;
		for (size_t row = 0; row < 4; ++row)
			for (size_t column = 0; column < 4; ++column)
				result.m[row][column] = values[row * 4 + column];
		return result;
	}
}

namespace swc {

	bool GlbModelLoader::Load(const wchar_t* path, ModelData& out, std::wstring& error)
	{
		error.clear();
		try
		{
			Shared::GlbDocument document;
			if (!document.Load(path, error)) return false;
			ModelData loaded;
			for (const Shared::GlbMaterial& source : document.Materials())
			{
				MaterialData material;
				material.name = source.name;
				material.baseColor = { source.baseColor[0], source.baseColor[1], source.baseColor[2], source.baseColor[3] };
				material.emissive = { source.emissive[0], source.emissive[1], source.emissive[2] };
				material.roughness = source.roughness;
				material.metallic = source.metallic;
				material.doubleSided = source.doubleSided;
				loaded.materials.push_back(std::move(material));
			}

			// 나무 등 반복 객체는 노드만 늘리고 메시/정점/GPU 버퍼는 한 번만 만든다.
			std::vector<bool> used(document.Meshes().size(), false);
			for (const Shared::GlbNode& node : document.Nodes())
			{
				if (HiddenNode(node.name) || node.mesh == Shared::kInvalidGlbIndex) continue;
				used[node.mesh] = true;
				if (node.name == "Terrain_Expedition_Planet_Closed")
				{
					// 닫힌 지표면만 뒷면을 제거한다. 건물·나무의 양면 재질은 유지한다.
					for (const Shared::GlbPrimitiveInfo& primitive : document.Meshes()[node.mesh].primitives)
						if (primitive.material != Shared::kInvalidGlbIndex)
							loaded.materials[primitive.material].doubleSided = false;
				}
			}
			std::vector<std::vector<uint32_t>> meshReferences(document.Meshes().size());
			std::vector<MeshBounds> meshBounds;
			for (uint32_t meshIndex = 0; meshIndex < document.Meshes().size(); ++meshIndex)
			{
				if (!used[meshIndex]) continue;
				const auto& sourceMesh = document.Meshes()[meshIndex];
				for (uint32_t primitiveIndex = 0; primitiveIndex < sourceMesh.primitives.size(); ++primitiveIndex)
				{
					Shared::GlbPrimitive source;
					if (!document.ReadPrimitive(meshIndex, primitiveIndex, source, error)) return false;
					if (loaded.meshes.size() >= kInvalidModelIndex)
					{
						error = L"GLB 메시 개수가 지원 범위를 넘었습니다.";
						return false;
					}
					ModelMeshData mesh;
					mesh.material = source.material;
					mesh.mesh.vertices.resize(source.vertices.size());
					MeshBounds bounds;
					for (size_t i = 0; i < source.vertices.size(); ++i)
					{
						const Shared::GlbVertex& input = source.vertices[i];
						Vertex& vertex = mesh.mesh.vertices[i];
						vertex.position = { input.position[0], input.position[1], input.position[2] };
						// 고유 메시를 옮길 때 한 번만 범위를 구한다. 인스턴스마다 정점을 다시 읽지 않는다.
						if (i == 0) bounds.lo = bounds.hi = vertex.position;
						else
						{
							bounds.lo.x = std::min(bounds.lo.x, vertex.position.x);
							bounds.lo.y = std::min(bounds.lo.y, vertex.position.y);
							bounds.lo.z = std::min(bounds.lo.z, vertex.position.z);
							bounds.hi.x = std::max(bounds.hi.x, vertex.position.x);
							bounds.hi.y = std::max(bounds.hi.y, vertex.position.y);
							bounds.hi.z = std::max(bounds.hi.z, vertex.position.z);
						}
						vertex.normal = { input.normal[0], input.normal[1], input.normal[2] };
						vertex.uv = { input.uv[0], input.uv[1] };
						vertex.color = { input.color[0], input.color[1], input.color[2] };
						// 현재 GLB에는 노멀맵이 없으므로 유효한 직교 접선만 보관한다.
						XMFLOAT3 tangent;
						XMStoreFloat3(&tangent, XMVector3Normalize(XMVector3Orthogonal(XMLoadFloat3(&vertex.normal))));
						vertex.tangent = { tangent.x, tangent.y, tangent.z, 1.0f };
					}
					mesh.mesh.indices = std::move(source.indices);
					meshReferences[meshIndex].push_back(uint32_t(loaded.meshes.size()));
					loaded.meshes.push_back(std::move(mesh));
					meshBounds.push_back(bounds);
				}
			}

			// 숨긴 충돌 프록시의 재질은 디코딩하지 않고, 렌더 메시에서 사용하는 이미지만 읽는다.
			std::vector<bool> usedMaterials(loaded.materials.size(), false);
			for (const ModelMeshData& mesh : loaded.meshes)
			{
				if (mesh.material == kInvalidModelIndex) continue;
				if (mesh.material >= usedMaterials.size())
				{
					error = L"GLB 메시의 재질 인덱스가 올바르지 않습니다.";
					return false;
				}
				usedMaterials[mesh.material] = true;
			}
			struct DecodedImage
			{
				uint32_t image;
				bool srgb;
				uint32_t texture;
			};
			std::vector<DecodedImage> decodedImages;
			auto loadTexture = [&](uint32_t textureIndex, TextureSlot slot, bool srgb,
				MaterialData& material)
			{
				if (textureIndex == Shared::kInvalidGlbIndex) return true;
				if (textureIndex >= document.Textures().size())
				{
					error = L"GLB 재질의 텍스처 인덱스가 올바르지 않습니다.";
					return false;
				}
				const uint32_t imageIndex = document.Textures()[textureIndex].source;
				for (const DecodedImage& image : decodedImages)
				{
					if (image.image == imageIndex && image.srgb == srgb)
					{
						material.textures[static_cast<size_t>(slot)] = image.texture;
						return true;
					}
				}
				if (loaded.textures.size() >= kInvalidModelIndex)
				{
					error = L"GLB 텍스처 개수가 지원 범위를 넘었습니다.";
					return false;
				}
				const uint8_t* bytes = nullptr;
				size_t byteCount = 0;
				if (!document.ReadImage(imageIndex, bytes, byteCount, error)) return false;
				const std::wstring label = std::wstring(path) + L"#image/" + std::to_wstring(imageIndex);
				TextureData texture;
				if (!LoadTextureImageFromMemory(bytes, byteCount, label.c_str(), srgb, texture, error)) return false;
				const uint32_t decodedIndex = uint32_t(loaded.textures.size());
				loaded.textures.push_back(std::move(texture));
				decodedImages.push_back({ imageIndex, srgb, decodedIndex });
				material.textures[static_cast<size_t>(slot)] = decodedIndex;
				return true;
			};
			for (size_t materialIndex = 0; materialIndex < usedMaterials.size(); ++materialIndex)
			{
				if (!usedMaterials[materialIndex]) continue;
				const Shared::GlbMaterial& source = document.Materials()[materialIndex];
				MaterialData& material = loaded.materials[materialIndex];
				if (!loadTexture(source.baseColorTexture, TextureSlot::BaseColor, true, material) ||
					!loadTexture(source.emissiveTexture, TextureSlot::Emissive, true, material)) return false;
			}

			bool hasBounds = false;
			// 숨긴 프록시도 빈 부모 노드로 남겨 실제 노드의 계층/로컬 변환을 유지한다.
			for (const Shared::GlbNode& source : document.Nodes())
			{
				ModelNodeData node;
				node.name = source.name;
				node.parent = source.parent;
				node.local = Matrix(source.local);
				if (!HiddenNode(source.name) && source.mesh != Shared::kInvalidGlbIndex)
				{
					node.meshes = meshReferences[source.mesh];
					const XMFLOAT4X4 world = Matrix(source.world);
					const XMMATRIX transform = XMLoadFloat4x4(&world);
					for (uint32_t meshIndex : node.meshes)
					{
						const MeshBounds& bounds = meshBounds[meshIndex];
						// 아핀 변환한 AABB의 여덟 모서리는 모든 실제 정점을 감싸는 범위다.
						// 회전·비균등 배율·반전에서도 반복 메시의 경계를 보수적으로 유지한다.
						for (uint32_t corner = 0; corner < 8; ++corner)
						{
							const XMFLOAT3 localPoint{
								(corner & 1) ? bounds.hi.x : bounds.lo.x,
								(corner & 2) ? bounds.hi.y : bounds.lo.y,
								(corner & 4) ? bounds.hi.z : bounds.lo.z };
							XMFLOAT3 point;
							XMStoreFloat3(&point, XMVector3TransformCoord(XMLoadFloat3(&localPoint), transform));
							if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
							{
								error = L"GLB 노드 변환의 좌표 범위가 올바르지 않습니다.";
								return false;
							}
							if (!hasBounds)
							{
								loaded.boundsMin = loaded.boundsMax = point;
								hasBounds = true;
							}
							else
							{
								loaded.boundsMin.x = std::min(loaded.boundsMin.x, point.x);
								loaded.boundsMin.y = std::min(loaded.boundsMin.y, point.y);
								loaded.boundsMin.z = std::min(loaded.boundsMin.z, point.z);
								loaded.boundsMax.x = std::max(loaded.boundsMax.x, point.x);
								loaded.boundsMax.y = std::max(loaded.boundsMax.y, point.y);
								loaded.boundsMax.z = std::max(loaded.boundsMax.z, point.z);
							}
						}
					}
				}
				loaded.nodes.push_back(std::move(node));
			}
			if (!hasBounds || loaded.meshes.empty())
			{
				error = L"GLB 모델에 표시할 메시가 없습니다.";
				return false;
			}
			out = std::move(loaded);
			return true;
		}
		catch (const std::bad_alloc&) { error = L"GLB 모델을 읽을 메모리가 부족합니다."; }
		catch (const std::length_error&) { error = L"GLB 모델 데이터가 지원 범위를 넘었습니다."; }
		return false;
	}
}
