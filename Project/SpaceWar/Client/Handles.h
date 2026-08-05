#pragma once
#include <cstdint>
#include <limits>

namespace swc {
	using MeshHandle = uint32_t;
	using MaterialHandle = uint32_t;
	using NodeHandle = uint32_t;

	inline constexpr NodeHandle kInvalidNode =
		(std::numeric_limits<NodeHandle>::max)();
	inline constexpr MeshHandle kInvalidMesh =
		(std::numeric_limits<MeshHandle>::max)();
	inline constexpr MaterialHandle kInvalidMaterial =
		(std::numeric_limits<MaterialHandle>::max)();
}