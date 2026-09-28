#pragma once
#include <DirectXMath.h>

namespace swc {
	struct Vertex {
		DirectX::XMFLOAT3 position{};
		DirectX::XMFLOAT3 normal{};
		DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT2 uv{};
		// xyz = 접선, w = 종법선 부호. 무텍스처 지형도 항상 초기화한다.
		DirectX::XMFLOAT4 tangent{ 1.0f, 0.0f, 0.0f, 1.0f };
	};
}
