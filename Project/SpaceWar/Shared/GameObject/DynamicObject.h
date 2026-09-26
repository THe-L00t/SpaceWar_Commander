#pragma once
#include <cstdint>
#include "Object.h"

// 명세 6.4 — 이동할 수 있고 화면에 표시되는 객체.
// 리소스와 렌더 데이터의 실물은 들지 않고 식별자만 들고 있다 (명세 6.11·6.12).
namespace Shared {

	class DynamicObject : public Object
	{
	public:
		~DynamicObject() override;

		// 식별자 0 의 유효/무효 규약은 Resource Manager 를 붙일 때 정한다.
		uint32_t meshIndex = 0;        // Resource Manager 의 Mesh
		uint32_t animationIndex = 0;   // Resource Manager 의 Animation
		uint32_t renderObjectId = 0;   // Render World 의 렌더 데이터

		float health = 0.0f;
		float speed = 0.0f;            // m/s
	};

} // namespace Shared
