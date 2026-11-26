#include "NPC.h"
#include "../GameLogic/GameLogic.h"   // kNpcMaxHealth — 2026-11-27 몬스터 기본 체력(전투 수치는 그 파일 한 곳)

namespace Shared {

	// 2026-11-27 사용자 «몬스터는 200정도로 해줘» — 처음엔 가득 찬 체력(kNpcMaxHealth = 200).
	NPC::NPC()
	{
		maxHealth = kNpcMaxHealth;
		health = maxHealth;
	}

	NPC::~NPC() = default;

} // namespace Shared
