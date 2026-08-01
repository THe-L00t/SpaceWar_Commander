#pragma once
#include <cstdint>

// 레이 예산 파라미터. DX 타입 없음 — 게임 로직·프레임 컨트롤러가 다룬다.
// 나중에 60fps 컨트롤러가 프레임시간을 보고 knee 를 올려 레이 수를 깎는다.
namespace swc {
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
