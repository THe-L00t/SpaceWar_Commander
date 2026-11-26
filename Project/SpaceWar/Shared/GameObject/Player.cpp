#include "Player.h"
#include "../GameLogic/GameLogic.h"   // kMaxHealth — 2026-11-26 플레이어 기본 체력(전투 수치는 그 파일 한 곳)

namespace Shared {

	// 2026-11-26 사용자 «플레이어 기본 100으로 해줘» — 처음엔 가득 찬 체력(kMaxHealth = 100).
	Player::Player()
	{
		maxHealth = kMaxHealth;
		health = maxHealth;
	}

	Player::~Player() = default;

} // namespace Shared
