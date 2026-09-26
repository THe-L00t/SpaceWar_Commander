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

	struct ModelMeshData
	{
		MeshData mesh;
		uint32_t material = kInvalidModelIndex;
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
