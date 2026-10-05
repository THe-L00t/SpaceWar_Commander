#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../Vec3.h"
#include "ModelFormat.h"

// ============================================================
//  Shared/Model/ModelSource.h — 포맷 중립 중간 표현
//
//  세 포맷(.swm · glTF · OBJ)의 리더가 모두 이 구조를 채운다. 그 뒤 단계는 포맷을 모른다.
//
//  ★ 왜 Shared 인가 — 서버가 읽어야 한다
//    교수님 2026-09-29 지시: «높이 정보와 장애물 정보를 서버에서 가지고 NPC 이동에 활용».
//    판정은 서버 권위(명세 18절 원칙 4)이므로 충돌 데이터는 서버가 직접 읽는다.
//    지형을 TerrainSampler 로 클라·서버가 같이 보는 것과 같은 방식이다.
//
//  ★ DirectXMath 를 쓰지 않는다
//    Shared 가 DirectX 를 물면 서버가 끌려온다(09-18 에 TerrainSampler 를 옮길 때 지킨 원칙).
//    렌더용 변환은 Client/Resource/ModelBuilder 가 한다.
//
//  ★ 텍스처 픽셀을 담지 않는다
//    경로만 들고 있고, 읽기는 Client 의 TextureLoader(WIC) 가 한다.
//    FBX SDK 시절의 «내장 텍스처를 임시폴더로 추출» 단계가 없어진다.
// ============================================================

namespace Shared {

	// Client 의 swc::Vertex 와 필드가 1:1 로 대응한다 (ModelBuilder 가 그대로 올린다).
	struct SourceVertex
	{
		Vec3 position{};
		Vec3 normal{};
		Vec3 color{ 1.0f, 1.0f, 1.0f };
		Vec2 uv{};
		Vec4 tangent{ 1.0f, 0.0f, 0.0f, 1.0f };   // xyz = 접선, w = 종법선 부호
	};

	struct SourceMesh
	{
		std::vector<SourceVertex> vertices;
		std::vector<uint32_t>     indices;        // 삼각형 목록 (후처리에서 보장)
		uint32_t                  material = kInvalidIndex;
	};

	// 텍스처 슬롯 순서는 셰이더의 t1~t5 와 같아야 한다 (Client 의 TextureSlot 과 동일).
	enum class SourceTextureSlot : size_t
	{
		BaseColor, Normal, Roughness, Metallic, Emissive, Count
	};
	inline constexpr size_t kSourceTextureCount = static_cast<size_t>(SourceTextureSlot::Count);

	struct SourceMaterial
	{
		std::string name;
		Vec4  baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
		Vec3  emissive{};
		float roughness = 0.65f;
		float metallic = 0.0f;

		// 모델 파일이 있는 폴더 기준 상대 경로. 빈 문자열이면 그 슬롯은 없다.
		std::array<std::string, kSourceTextureCount> texturePaths;
	};

	struct SourceNode
	{
		std::string name;
		uint32_t    parent = kInvalidIndex;   // ★ 부모가 반드시 자기보다 앞에 온다
		Mat4        local{};
		std::vector<uint32_t> meshes;
	};

	// ★ 서버가 쓰는 부분. 렌더 정보가 전혀 없다.
	//   하이트맵을 쓰지 않는 맵(2026-10-05 방침)에서는 이것이 지면 높이의 출처가 된다.
	struct CollisionData
	{
		std::vector<Vec3>     vertices;
		std::vector<uint32_t> indices;        // 삼각형 목록
		Vec3 boundsMin{};
		Vec3 boundsMax{};

		bool Empty() const { return indices.empty(); }
	};

	struct ModelSource
	{
		std::vector<SourceMesh>     meshes;
		std::vector<SourceMaterial> materials;
		std::vector<SourceNode>     nodes;
		CollisionData               collision;

		Vec3 boundsMin{};
		Vec3 boundsMax{};

		ModelFormat format = ModelFormat::Unknown;
		std::string sourceDirectory;   // 텍스처 상대 경로를 푸는 기준
	};

	struct ReadOptions
	{
		// 서버용. 정점색·UV·탄젠트·재질을 읽지 않고 충돌과 위치만 채운다.
		bool geometryOnly = false;

		// 충돌 섹션이 없는 포맷(glTF·OBJ)이면 렌더 메시에서 만들어 넣는다.
		bool generateCollision = true;

		// 탄젠트가 파일에 없을 때 UV 기준으로 만든다. 노멀맵을 쓰면 필요하다.
		bool generateTangents = true;
	};

} // namespace Shared
