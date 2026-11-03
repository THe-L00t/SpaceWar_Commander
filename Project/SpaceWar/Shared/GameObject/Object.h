#pragma once
#include "../Vec3.h"

// ============================================================
//  Shared/GameObject/Object.h
//  아키텍처 명세서 6.3 — 게임 월드에 존재하는 가장 기본적인 객체.
//  화면에 보이지 않는 객체(트리거 등)도 여기에 해당한다.
//
//  계층 (명세 6.2)
//      Object
//      ├── DynamicObject ── Player / OtherPlayer / NPC
//      └── StaticObject
//
//  ★ 지금은 구조뿐이다. 이동·판정·네트워크 동기화 로직은 없다.
//    명세의 Collision Box / State / Weapon / Behavior Tree 는 아직 넣지 않았다.
//  ★ Rotation (2026-11-02 명세 6.3 에 추가, 사용자 «오브젝트 명세에 회전도 넣어줘»)
//    단위 쿼터니언 (x, y, z, w). 기본값 = 회전 없음. Direction 은 이 회전으로 앞쪽(+Z)을 돌린 방향과 같게 둔다.
//    Direction 만 쓰는 곳(서버 NpcWorld)은 아직 Rotation 을 채우지 않는다.
// ============================================================

namespace Shared {

	class Object
	{
	public:
		Object() = default;
		virtual ~Object();

		Vec3 position{};
		Vec3 direction{ 0.0f, 0.0f, 1.0f };
		Vec4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f };   // 쿼터니언 (x, y, z, w) — 명세 6.3 Rotation
		Vec3 scale{ 1.0f, 1.0f, 1.0f };
	};

} // namespace Shared
