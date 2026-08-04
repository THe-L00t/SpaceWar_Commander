#pragma once
#include <cstdint>
#include <DirectXMath.h>
#include "Handles.h"
namespace swc {
	struct RenderItem {
		uint32_t sortKey = 0;
		NodeHandle node = kInvalidNode;
		MeshHandle mesh = kInvalidMesh;
		MaterialHandle material = kInvalidMaterial;
	};

	struct RenderView {
		DirectX::XMFLOAT4X4 viewProj{ };
		DirectX::XMFLOAT3   eyePosition{ };   // 프레넬의 시선 벡터(V) 계산용
	};
}
