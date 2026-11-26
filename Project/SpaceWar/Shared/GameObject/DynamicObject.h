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

		float health = 0.0f;           // 명세 6.4.2 Health — 지금 체력
		// 2026-11-26 최대 체력 (사용자 «다이나믹 클래스에 체력 만들고 UI에 체력바 연동 시켜줘. 다른건 아직 건들지 마»)
		//  체력바 비율(health / maxHealth)의 분모. 0 = 아직 정하지 않음. 명세 6.4.2 에는 Health 하나뿐이라 명세 밖 칸이다.
		//  채우는 곳은 생성자 — Shared::Player 100(Shared::kMaxHealth, 2026-11-26 «플레이어 기본 100») ·
		//  Shared::NPC 200(Shared::kNpcMaxHealth, 2026-11-27 «몬스터는 200정도로»). OtherPlayer 는 아직 0, 서버 플레이어(Session)는 이 클래스를 쓰지 않는다.
		float maxHealth = 0.0f;
		float speed = 0.0f;            // m/s
	};

} // namespace Shared
