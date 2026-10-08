#pragma once
#include <cstdint>
#include <limits>

namespace swc {
	using MeshHandle = uint32_t;
	using MaterialHandle = uint32_t;
	using NodeHandle = uint32_t;
	using TextureHandle = uint32_t;     // GPU 텍스처 (Renderer 발급). RAM 사본은 TextureDataHandle

	inline constexpr NodeHandle kInvalidNode =
		(std::numeric_limits<NodeHandle>::max)();
	inline constexpr MeshHandle kInvalidMesh =
		(std::numeric_limits<MeshHandle>::max)();
	inline constexpr MaterialHandle kInvalidMaterial =
		(std::numeric_limits<MaterialHandle>::max)();
	inline constexpr TextureHandle kInvalidTexture =
		(std::numeric_limits<TextureHandle>::max)();
}