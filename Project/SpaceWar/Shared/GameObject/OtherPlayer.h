#pragma once
#include "DynamicObject.h"

// 명세 6.7 — 현재 클라이언트에서 자신을 제외한 다른 플레이어.
// 상태의 갱신 원천은 로컬 입력이 아니라 서버가 보내는 네트워크 데이터다.
// 명세의 Weapon 은 이번 범위에서 제외했다.
namespace Shared {

	class OtherPlayer : public DynamicObject
	{
	public:
		~OtherPlayer() override;
	};

} // namespace Shared
