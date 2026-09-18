#pragma once

// ============================================================
//  Shared/Physics/PhysicsCalculator.h — 아키텍처 명세서 13절
//
//  수학·물리 계산과 충돌 판정을 담당하는 독립 계산 계층이다.
//      Collision · Distance · Intersection · Ray Test ·
//      Vector Calculation · Transform Calculation
//  게임 객체나 Renderer 에 기대지 않고 값이나 참·거짓만 돌려준다.
//
//  ★ Shared 인 이유 — 클라는 예측에, 서버는 권위 판정에 «같은 계열의 계산» 을 쓴다
//    (명세 13절). 최종 판정은 서버가 한다.
//    지형 높이 계산은 이미 Shared/Terrain/TerrainSampler 에 있다.
//  ★ 지금은 선언만 있다. 충돌 박스 형식과 Ray Test 범위가 아직 미정이다
//    (명세 6.13 의 «가림 판정용 충돌 데이터 범위» 도 미정).
// ============================================================

namespace Shared {

	class PhysicsCalculator
	{
	public:
		PhysicsCalculator();
		~PhysicsCalculator();
	};

} // namespace Shared
