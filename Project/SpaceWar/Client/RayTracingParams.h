#pragma once
#include <cstdint>

// ============================================================
//  RayTracingParams.h — 실행 중 RT 설정. DX 타입 없음 — 게임 로직 · 디버그 키가 다룬다.
//
//  ★ 2026-10-27 렌더 파이프라인 2단계에서 다시 썼다
//    · 임시 스위치 kEnableRaytracing(2026-09-14, «래스터만으로 확인하려고» RT 를 통째로 끔)을 없앴다.
//      이제 장치가 DXR Tier 1.1 + SM 6.5 를 지원하면 RT 단계가 켜진다(GRenderer::SupportsRaytracing).
//    · 옛 «픽셀 셰이더 안 인라인 반사 + 러시안 룰렛» 의 rouletteKnee · fresnelBoost 와
//      쓰지 않던 예약 값(resolutionScale · raysPerPixel)을 없앴다.
//      반사는 구조도 [4b](DispatchRays)에서 새로 짠다.
// ============================================================
namespace swc {

	struct RayTracingParams
	{
		// 실행 중 RT 단계를 끄고 켠다(lab: R 키).
		//  꺼지면 [4a] RT 그림자를 건너뛰고, [5] 라이팅은 그림자 없이(가시도 1) 합성한다.
		bool enabled = true;
	};
}
