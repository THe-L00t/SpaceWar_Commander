#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "../MeshData.h"

namespace swc {

	enum class TextureSlot : size_t
	{
		BaseColor, Normal, Roughness, Metallic, Emissive, Count
	};
	inline constexpr size_t kMaterialTextureCount = static_cast<size_t>(TextureSlot::Count);
	inline constexpr uint32_t kInvalidModelIndex = UINT32_MAX;

	// WIC에서 읽은 RGBA8 픽셀. GPU 객체나 FBX SDK 객체를 소유하지 않는다.
	struct TextureData
	{
		std::wstring path;
		uint32_t width = 0;
		uint32_t height = 0;
		bool srgb = false;
		std::vector<uint8_t> pixels;
	};

	struct MaterialData
	{
		std::string name;
		DirectX::XMFLOAT4 baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT3 emissive{ 0.0f, 0.0f, 0.0f };
		float roughness = 0.65f;
		float metallic = 0.0f;
		std::array<uint32_t, kMaterialTextureCount> textures{
			kInvalidModelIndex, kInvalidModelIndex, kInvalidModelIndex,
			kInvalidModelIndex, kInvalidModelIndex };
	};

	// 스키닝 속성. ★ Vertex 에 넣지 않고 따로 둔다 —
	//   같은 Vertex 를 절차적 지형(정점 60만 개)이 쓰므로 24바이트를 더하면 그쪽이 통째로 무거워진다.
	//   GPU 스키닝을 붙일 때 두 번째 정점 스트림으로 올린다(명세 §7 의 연속 배열 방식).
	struct SkinVertex
	{
		uint16_t joints[4]{};
		float    weights[4]{};
	};

	struct ModelMeshData
	{
		MeshData mesh;
		uint32_t material = kInvalidModelIndex;

		// 스킨 메시면 mesh.vertices 와 같은 길이다. 아니면 비어 있다.
		std::vector<SkinVertex> skin;
	};

	struct ModelNodeData
	{
		std::string name;
		uint32_t parent = kInvalidModelIndex;
		DirectX::XMFLOAT4X4 local{
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f };
		std::vector<uint32_t> meshes;
	};

	struct ModelData
	{
		std::vector<ModelMeshData> meshes;
		std::vector<MaterialData> materials;
		std::vector<TextureData> textures;
		// 부모를 먼저 저장한다. Scene 등록과 추후 애니메이션에 계층을 그대로 사용한다.
		std::vector<ModelNodeData> nodes;
		DirectX::XMFLOAT3 boundsMin{};
		DirectX::XMFLOAT3 boundsMax{};
	};
}
