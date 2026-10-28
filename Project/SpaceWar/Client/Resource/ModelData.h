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

	// 텍스처 픽셀 형식 (2026-10-28 지형 레이어 — 밉맵 DDS 의 높이 · 눈밭은 1채널이다)
	enum class TextureFormat : uint8_t
	{
		Rgba8,   // 픽셀 4바이트. srgb 면 색(R8G8B8A8_UNORM_SRGB), 아니면 데이터(R8G8B8A8_UNORM)
		R8,      // 픽셀 1바이트 (R8_UNORM). srgb 는 보지 않는다
	};

	// WIC · DDS 에서 읽은 픽셀. GPU 객체나 FBX SDK 객체를 소유하지 않는다.
	struct TextureData
	{
		std::wstring path;
		uint32_t width = 0;
		uint32_t height = 0;
		bool srgb = false;
		// ── 밉 · 배열 · 1채널 (2026-10-28 지형 레이어) ──
		//  pixels = 배열 원소마다 «밉 0, 1, … mipLevels − 1» 을 차례로, 각 밉의 행은 빈틈 없이(밉 m 의 크기 = max(1, 크기 >> m)).
		//  D3D12 서브리소스 번호 순서(원소 × 밉)와 같고 DDS 파일의 데이터 순서와도 같다.
		//  기본값(Rgba8 · 밉 1 · 원소 1)이면 예전과 똑같은 RGBA8 한 장이다.
		TextureFormat format = TextureFormat::Rgba8;
		uint32_t mipLevels = 1;
		uint32_t arraySize = 1;   // 1 = Texture2D, 2 이상 = Texture2DArray
		std::vector<uint8_t> pixels;
	};

	// ── 지형 레이어 재질 (2026-10-28) — 레이어 4종 + LayerMask(RGBA) ──
	//  리소스 규격: 노션 «예시 행성 제작 기술» 2.6, 각 행성 Placement.json 의 terrain_layers.
	//  섞는 식은 블렌더 시안(Resource_Work/<행성>/scripts/build_map.py draft_terrain_material)의 높이 섞기 그대로:
	//    점수 s = Height × heightBlend + 마스크,  w = max(s − (max(s) − blendDepth), 0) / Σ
	//  텍스처는 GRenderer::CreateTerrainMaterial 이 따로 받는다(같은 종류 4장을 묶은 배열 4개 + 마스크).
	struct TerrainMaterialData
	{
		std::string name;
		std::array<float, 4> heightBlend{ 0.5f, 0.5f, 0.5f, 0.5f };   // 마스크 R G B A 레이어마다 Height 에 곱하는 값
		float blendDepth = 0.2f;                                        // 경계 폭 — 가장 큰 점수에서 이만큼 아래까지 섞인다
		float normalStrength = 1.0f;                                    // 섞은 노멀맵의 세기(시안 Normal Map 노드의 Strength)
		// ── 3단계 반복 숨김 (2026-10-28) — 시안의 «바위 레이어 세 방향 투영» ──
		//  이 레이어들은 UV 대신 메시 좌표를 세 면(블렌더 BOX 투영, 경계 섞기 0.35)으로 읽고, 노멀맵 대신 평평한 노멀을 쓴다.
		//  두 크기(tileA · tileB)를 «두 크기 UV» 와 같은 노이즈로 섞는다.
		uint32_t triplanarLayers = 0;    // 비트 = 레이어(마스크 R 1 · G 2 · B 4 · A 8)
		float triplanarTileA = 9.0f;     // 텍스처 한 장이 덮는 길이(m) — 크기 A
		float triplanarTileB = 2.7f;     //                                크기 B
		bool  triplanarOffsetB = false;  // 크기 B 좌표에 (3.7, 1.9, 5.3) 을 더한다(빙하 시안)
		float warpScale = 0.0f;          // 세 방향 투영 좌표를 비트는 노이즈 규모(m). 0 = 비틀지 않음
		float warpAmount = 0.0f;         // 비트는 최대 거리(m)
		// ── 4단계 색 흔들림 · 범프 (2026-10-28) — 시안의 macro · bump1 · bump2 ──
		//  다섯 행성 공통 값(바위 흔들림 · AO 배율 · 요철 노이즈 크기)은 셰이더 상수(TerrainLayers.hlsli). 기본값 = 숲 · 네온 · 폐허.
		float macroAmount = 0.32f;       // 큰 규모(약 83 m) 색 흔들림: 노이즈 × amount + bias
		float macroBias = 0.84f;
		float bumpDistance = 0.06f;      // 범프 1 — 섞은 높이맵(바위는 4배) Distance(m)
		float reliefDistance = 0.45f;    // 범프 2 — 지면 요철 Distance(m)
		float reliefStrength = 0.8f;     // 범프 2 — Strength(0 ~ 1)
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
