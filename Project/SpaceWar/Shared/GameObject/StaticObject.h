#pragma once
#include <cstdint>
#include "Object.h"

// 명세 6.5 — 움직이지 않고 화면에 표시되는 객체. DynamicObject 의 형제.
// Animation Index 와 Speed 를 갖지 않는 점만 다르다.
namespace Shared {

	class StaticObject : public Object
	{
	public:
		~StaticObject() override;

		uint32_t meshIndex = 0;        // Resource Manager 의 Mesh
		uint32_t renderObjectId = 0;   // Render World 의 렌더 데이터

		float health = 0.0f;
	};

} // namespace Shared
