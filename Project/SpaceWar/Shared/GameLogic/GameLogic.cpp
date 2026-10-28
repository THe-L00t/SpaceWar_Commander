#include "GameLogic.h"

namespace Shared {

	GameLogic::GameLogic() = default;
	GameLogic::~GameLogic() = default;

	float GameLogic::ApplyDamage(float health, float damage)
	{
		const float left = health - damage;
		return left > 0.0f ? left : 0.0f;
	}

	bool GameLogic::IsDown(float health)
	{
		return health <= 0.0f;
	}

} // namespace Shared
