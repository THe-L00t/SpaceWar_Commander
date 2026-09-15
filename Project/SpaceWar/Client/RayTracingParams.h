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
	//
	//  ★ test 브랜치에서는 true 다 (2026-09-14).
	//    아래 DXR 부분 차단 시험(숫자키 1~6)은 RT 경로가 살아 있어야 의미가 있다.
	//    이 값이 false 면 BLAS·TLAS 가 아예 만들어지지 않아 시험이 전부 무효다.
	inline constexpr bool kEnableRaytracing = true;

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

		// ── DXR 부분 차단 (디버깅용) ─────────────────────────
		//  메모리 폭주의 «방아쇠» 를 좁히려면 RT 경로를 한 조각씩 끊어봐야 한다.
		//  아래 세 값은 숫자키 1~6 (kRtTestPresets) 으로 바꾼다.
		//
		//  ★ 런타임에 끌 수 없는 것: BLAS. 메쉬를 만들 때 한 번만 빌드하므로
		//    끄려면 kEnableRaytracing 을 false 로 두고 다시 빌드해야 한다.
		bool     buildTlas = true;     // 매 프레임 TLAS 재빌드
		bool     traceRays = true;     // 셰이더의 레이 발사
		bool     bindTlas = true;      // TLAS 를 루트 SRV 로 바인딩 (끄면 발사도 멈춘다)
		uint32_t tlasInterval = 1;     // 재빌드 주기 (프레임)
	};

	// ── DXR 부분 차단 시험 (숫자키 1~6) ─────────────────────
	//  바인딩을 끄면 발사도 멈추므로 실제로 다른 상태는 이 6가지뿐이다.
	//  조건마다 새로 실행하고, 시작하자마자 번호를 누른다.
	//
	//    번호  재빌드 바인딩 발사   이것만 재현되면
	//     1     O      O     O    셋이 겹칠 때 (기준 — 먼저 재현돼야 한다)
	//     2     O      O     X    재빌드 + 바인딩 조합
	//     3     X      O     O    발사 (4 가 멀쩡할 때)
	//     4     X      O     X    바인딩
	//     5     O      X     X    재빌드
	//     6     X      X     X    키로 못 끄는 쪽 (BLAS·RT 셰이더·디버그 계층)
	struct RtTestPreset
	{
		bool buildTlas;
		bool bindTlas;
		bool traceRays;
	};

	inline constexpr RtTestPreset kRtTestPresets[] =
	{
		{ true,  true,  true  },
		{ true,  true,  false },
		{ false, true,  true  },
		{ false, true,  false },
		{ true,  false, false },
		{ false, false, false },
	};
	inline constexpr int kRtTestCount = 6;
}
