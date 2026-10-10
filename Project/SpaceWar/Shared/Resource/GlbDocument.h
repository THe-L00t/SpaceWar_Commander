#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Shared {

	inline constexpr uint32_t kInvalidGlbIndex = UINT32_MAX;

	struct GlbVertex
	{
		std::array<float, 3> position{};
		std::array<float, 3> normal{ 0.0f, 1.0f, 0.0f };
		std::array<float, 2> uv{};
		std::array<float, 4> color{ 1.0f, 1.0f, 1.0f, 1.0f };
		std::array<float, 4> tangent{ 1.0f, 0.0f, 0.0f, 1.0f };
	};

	struct GlbPrimitive
	{
		std::vector<GlbVertex> vertices;
		std::vector<uint32_t> indices;
		uint32_t material = kInvalidGlbIndex;
	};

	// 접지 BVH는 렌더 정점 속성을 복사하지 않고 위치와 인덱스만 읽는다.
	struct GlbGeometry
	{
		std::vector<std::array<float, 3>> positions;
		std::vector<uint32_t> indices;
	};

	struct GlbPrimitiveInfo
	{
		uint32_t position = kInvalidGlbIndex;
		uint32_t normal = kInvalidGlbIndex;
		uint32_t uv = kInvalidGlbIndex;
		uint32_t color = kInvalidGlbIndex;
		uint32_t tangent = kInvalidGlbIndex;
		uint32_t indices = kInvalidGlbIndex;
		uint32_t material = kInvalidGlbIndex;
		// accessor 검증 후 확정한 개수. 접지 배열을 디코딩 전에 한 번 예약한다.
		size_t vertexCount = 0;
		size_t indexCount = 0;
	};

	struct GlbMesh
	{
		std::string name;
		std::vector<GlbPrimitiveInfo> primitives;
	};

	struct GlbMaterial
	{
		std::string name;
		std::array<float, 4> baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
		std::array<float, 3> emissive{};
		float roughness = 1.0f;
		float metallic = 1.0f;
		bool doubleSided = false;
		uint32_t baseColorTexture = kInvalidGlbIndex;
		uint32_t emissiveTexture = kInvalidGlbIndex;
	};

	struct GlbImage
	{
		std::string name;
		std::string mimeType;
		uint32_t view = kInvalidGlbIndex;
	};

	struct GlbTexture
	{
		uint32_t source = kInvalidGlbIndex;
	};

	struct GlbNode
	{
		std::string name;
		uint32_t mesh = kInvalidGlbIndex;
		uint32_t parent = kInvalidGlbIndex;
		std::array<float, 16> local{
			1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
		std::array<float, 16> world{
			1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	};

	// GLB 2.0의 정적 indexed mesh를 읽는다. 반복 노드는 같은 메시를 공유한다.
	// 좌표·변환은 DirectX 왼손 좌표계, 행 벡터 규약으로 반환한다.
	// JSON은 Load 안에서 해제하고 바이너리만 보관하며, 요청한 primitive만 디코딩한다.
	class GlbDocument
	{
	public:
		bool Load(const wchar_t* path, std::wstring& error);
		const std::vector<GlbNode>& Nodes() const { return nodes; }
		const std::vector<GlbMesh>& Meshes() const { return meshes; }
		const std::vector<GlbMaterial>& Materials() const { return materials; }
		const std::vector<GlbImage>& Images() const { return images; }
		const std::vector<GlbTexture>& Textures() const { return textures; }
		// 반환 포인터는 다음 Load 또는 문서 소멸 전까지 유효한 BIN 내부 데이터다.
		bool ReadImage(uint32_t image, const uint8_t*& bytes, size_t& byteCount,
			std::wstring& error) const;
		bool ReadPrimitive(uint32_t mesh, uint32_t primitive,
			GlbPrimitive& out, std::wstring& error) const;
		// 렌더용 법선·UV·색상·접선 디코딩을 생략하는 접지 전용 경로다.
		bool ReadGeometry(uint32_t mesh, uint32_t primitive,
			GlbGeometry& out, std::wstring& error) const;

	private:
		struct BufferView
		{
			size_t offset = 0;
			size_t length = 0;
			size_t stride = 0;
		};
		struct Accessor
		{
			uint32_t view = kInvalidGlbIndex;
			size_t offset = 0;
			size_t count = 0;
			uint32_t component = 0;
			uint32_t dimensions = 0;
			bool normalized = false;
		};

		std::vector<uint8_t> binary;
		std::vector<BufferView> views;
		std::vector<Accessor> accessors;
		std::vector<GlbMesh> meshes;
		std::vector<GlbMaterial> materials;
		std::vector<GlbImage> images;
		std::vector<GlbTexture> textures;
		std::vector<GlbNode> nodes;
	};
}