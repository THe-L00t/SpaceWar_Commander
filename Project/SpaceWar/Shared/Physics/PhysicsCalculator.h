#pragma once
// ★ Shared 안에서는 상대 경로로 include 한다
//   Shared.vcxproj 에는 추가 include 디렉터리가 없다. 클라·서버처럼 "Shared/..." 로 쓰면
//   Shared 자신을 빌드할 때 파일을 못 찾는다 (TerrainSampler.h 도 "../HeightmapData.h" 를 쓴다).
#include "../Vec3.h"
#include "../Vec3d.h"

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
//  ★ 클라에서의 실행 위치 — 메인 스레드, 넓은 판정·좁은 판정은 측정 뒤 잡 (멀티스레딩 5장)
//    계산 계층이 객체에 의존하지 않으므로 결과 배열을 ③ 상태 커밋으로 넘긴다.
//  ★ 2026-09-24 — 히트스캔에 필요한 것만 채웠다
//    레이 대 구(히트박스)와 레이 대 지형(가림)이다. 충돌 박스 형식과
//    명세 6.13 의 «가림 판정용 충돌 데이터 범위» 는 여전히 미정이다.
// ============================================================

namespace Shared {

	class TerrainSampler;

	class PhysicsCalculator
	{
	public:
		PhysicsCalculator();
		~PhysicsCalculator();

		// ── 레이 대 구 ──────────────────────────────────────
		//  히트박스 하나와 레이가 만나는가. 만나면 true 와 원점에서의 거리.
		//  레이 뒤쪽(음수 거리)은 맞은 것으로 치지 않는다.
		static bool RaySphere(const Vec3& origin, const Vec3& direction,
			const Vec3& center, float radius, float maxDistance, float& outDistance);

		// ── 레이 대 캡슐 (2026-11-25) ──────────────────────────
		//  캐릭터 몸(플레이어 · 게임 맵 NPC)의 충돌체 = 이동 캡슐(PlayerMovement kCapsuleRadius · kCapsuleHeight)과 레이.
		//  a · b = 두 끝 구의 중심, direction = 단위. [tMin, tMax] 안에서 처음 들어가는 거리 — 원점이 캡슐 안이면 tMin.
		//  캡슐 = 막힌 원기둥 ∪ 두 끝 구 — 볼록이라 레이가 안에 있는 구간이 하나다(세 구간을 합친다).
		static bool RayCapsule(const Vec3d& origin, const Vec3d& direction, const Vec3d& a, const Vec3d& b,
			double radius, double tMin, double tMax, double& outDistance);

		// ── 레이 대 지형 ────────────────────────────────────
		//  원점에서 distance 만큼 가는 동안 땅에 막히는가.
		//
		//  ★ 행성 자체(지평선 너머)도 여기서 같이 걸러진다
		//    표본 지점의 «행성 중심에서의 거리» 를 «기준구 + 지형 높이» 와 비교하므로,
		//    지형이 없어도 구 반대편은 막힌 것으로 나온다.
		//  ★ terrain 이 null 이면 평평한 기준구로 검사한다.
		//  ★ 2026-11-15 — 서버 사격 판정은 Shared::TraceShot(GameLogic/ShotTrace — 몸 · 충돌체 · 지형)을 쓴다. 지금 부르는 곳 없음.
		static bool TerrainBlocks(const TerrainSampler* terrain,
			const Vec3& origin, const Vec3& direction, float distance);
	};

} // namespace Shared
