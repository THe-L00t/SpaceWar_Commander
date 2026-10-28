#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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
		bool doubleSided = false;

		// ── 투과 (2026-10-27 렌더 2단계) ──
		//  glTF KHR_materials_transmission · KHR_materials_volume 과 이름 · 뜻 · 기본값을 맞췄다(로더가 그대로 옮겨 담게).
		//  지금은 [4a] RT 그림자만 읽는다 — 그림자 레이가 이 재질의 물체 하나를 지나면 빛이 이만큼 남는다:
		//    남는 빛 = transmission × baseColor.rgb × attenuationColor ^ (thickness / attenuationDistance)
		//  기본값(transmission 0)이면 예전처럼 빛을 완전히 막는다.
		float transmission = 0.0f;                                              // transmissionFactor — 0 불투명 ~ 1 다 통과
		float thickness = 0.0f;                                                 // thicknessFactor — 부피 두께(메시 좌표 단위)
		float attenuationDistance = std::numeric_limits<float>::infinity();     // 빛이 attenuationColor 가 되는 거리
		DirectX::XMFLOAT3 attenuationColor{ 1.0f, 1.0f, 1.0f };                // 흰빛이 그 거리를 지나 남는 색
		// ── 굴절률 (2026-10-27 렌더 9단계) — glTF KHR_materials_ior 의 ior, 기본값 1.5 도 같다. 얼음 1.31.
		//  [7] 반투명 패스가 굴절 방향(HDRP 상자 모델)과 표면 반사율 F0 = ((ior−1)/(ior+1))² 에 쓴다.
		float ior = 1.5f;
		// ── 알파 마스크 (2026-10-28) — glTF alphaMode MASK · alphaCutoff ──
		//  참이면 «baseColor 알파(텍스처 a × baseColor.a) < alphaCutoff» 인 곳은 없는 것으로 그린다:
		//  G-buffer · 배경 패스는 그 픽셀을 버리고, RT 레이(그림자 · AO · GI · 반사 · 반투명)는 그 자리를 지나간다.
		bool  alphaMask = false;
		float alphaCutoff = 0.5f;
		std::array<uint32_t, kMaterialTextureCount> textures{
			kInvalidModelIndex, kInvalidModelIndex, kInvalidModelIndex,
			kInvalidModelIndex, kInvalidModelIndex };
	};

	// 스키닝 속성. ★ Vertex 에 넣지 않고 따로 둔다 —
	//   같은 Vertex 를 절차적 지형(정점 60만 개)이 쓰므로 24바이트를 더하면 그쪽이 통째로 무거워진다.
	//   GPU 스키닝을 붙일 때 두 번째 정점 스트림으로 올린다(명세 §7 의 연속 배열 방식).
	//   ★ 파서가 만든 배열을 그대로 받는다(Shared/Model/Vertex.h, 2026-10-09).
	using SkinVertex = Shared::SkinVertex;

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
