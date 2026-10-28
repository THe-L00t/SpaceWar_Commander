#include "BehaviorTree.h"

// 소멸자를 여기서 정의해 vtable 이 이 번역 단위에만 생기게 한다.
namespace Shared {

	BehaviorTree::~BehaviorTree() = default;

} // namespace Shared
