#pragma once
#include <cstdint>

// 레이 예산 파라미터. DX 타입 없음 — 게임 로직·프레임 컨트롤러가 다룬다.
// 나중에 60fps 컨트롤러가 프레임시간을 보고 knee 를 올려 레이 수를 깎는다.
namespace swc {

	// ── 임시 스위치 (2026-09-14) ─────────────────────────────
	//  래스터만으로 확인하려고 DXR 을 통째로 끈다.
	//
	//  false 면 장치가 DXR 을 지원하더라도
	//    · 가속 구조를 아예 만들지 않는다 (BLAS·TLAS 둘 다. AccelStructure::Initialize 도 안 부른다)
	//    · 셰이더를 RT_SUPPORTED=0 · sm 6.0 으로 컴파일한다 (RayQuery 코드가 들어가지 않는다)
	//    · 루트 시그니처에서 TLAS SRV 파라미터를 뺀다
	//
	//  ★ 실행 인자로 만들지 않는다. 되돌리려면 true 로 고치고 다시 빌드한다.
	inline constexpr bool kEnableRaytracing = false;

	struct RayTracingParams
	{
		bool  enabled = true;

		// 러시안 룰렛 무릎점. 반사 가중치가 이 값 이상이면 확정 발사(노이즈 0),
		// 미만이면 확률 발사한다. 올릴수록 레이가 줄고 꼬리 노이즈가 커진다.
		float rouletteKnee = 0.15f;

		// FGPS 의 F(p) 가 발사 확률을 얼마나 밀어올릴지.
		// 그려지는 값이 아니라 "샘플을 어디에 몰아줄지"에만 영향을 준다.
		float fresnelBoost = 2.0f;

		float    resolutionScale = 1.0f;   // 예약 (RT 해상도 분리 시)
		uint32_t raysPerPixel = 1;         // 예약
	};
}
