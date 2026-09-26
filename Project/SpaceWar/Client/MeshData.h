#pragma once
#include <cstdint>
#include <vector>
#include "Vertex.h"

namespace swc {
	// 파일 모델과 절차적 지형이 함께 사용하는 CPU 메시.
	struct MeshData
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
	};
}
