#pragma once
#include <cmath>

// ============================================================
//  Shared/Units.h — 단위 규약
//
//  ★ 내부 계산은 예외 없이 아래 정규 단위를 쓴다.
//        길이 = 미터(m) / 시간 = 초(s) / 각도 = 라디안(rad)
//     변환은 경계에서만 한다 — 상수 작성, UI 표시, 파일 입출력.
//
//  ★ 상수를 쓸 때는 리터럴로 단위를 드러낸다.
//        constexpr float kPlanetRadius = 120.0_km;   // O
//        constexpr float kPlanetRadius = 120000.0f;  // X (자릿수 세야 함)
//
//  매크로가 아니라 constexpr 사용자 정의 리터럴을 쓰는 이유:
//    - 매크로는 네임스페이스를 무시해 다른 헤더와 충돌한다
//    - 매크로는 타입이 없어 정수 연산이 조용히 생긴다
//    - 리터럴은 컴파일 시간에 접히므로 런타임 비용이 0이다
//
//  Shared 에 두는 이유:
//    서버가 권위를 가지므로 이동 속도·중력·행성 반지름을 클라와 서버가
//    똑같이 알아야 한다. 둘이 다른 값을 쓰면 위치 검증이 어긋난다.
// ============================================================

namespace Shared {

	inline constexpr double kPiD = 3.14159265358979323846;

	inline constexpr float kPi     = static_cast<float>(kPiD);
	inline constexpr float kTwoPi  = static_cast<float>(kPiD * 2.0);
	inline constexpr float kHalfPi = static_cast<float>(kPiD * 0.5);

	constexpr float Deg2Rad(float deg) { return deg * static_cast<float>(kPiD / 180.0); }
	constexpr float Rad2Deg(float rad) { return rad * static_cast<float>(180.0 / kPiD); }

	constexpr double Deg2Rad(double deg) { return deg * (kPiD / 180.0); }
	constexpr double Rad2Deg(double rad) { return rad * (180.0 / kPiD); }

	namespace units {

		// ── 길이 → 미터 ──────────────────────────────────
		constexpr float operator""_mm(long double v) { return static_cast<float>(v * 0.001L); }
		constexpr float operator""_cm(long double v) { return static_cast<float>(v * 0.01L); }
		constexpr float operator""_m (long double v) { return static_cast<float>(v); }
		constexpr float operator""_km(long double v) { return static_cast<float>(v * 1000.0L); }

		constexpr float operator""_mm(unsigned long long v) { return static_cast<float>(v) * 0.001f; }
		constexpr float operator""_cm(unsigned long long v) { return static_cast<float>(v) * 0.01f; }
		constexpr float operator""_m (unsigned long long v) { return static_cast<float>(v); }
		constexpr float operator""_km(unsigned long long v) { return static_cast<float>(v) * 1000.0f; }

		// ── 시간 → 초 ────────────────────────────────────
		constexpr float operator""_ms (long double v) { return static_cast<float>(v * 0.001L); }
		constexpr float operator""_s  (long double v) { return static_cast<float>(v); }
		constexpr float operator""_min(long double v) { return static_cast<float>(v * 60.0L); }

		constexpr float operator""_ms (unsigned long long v) { return static_cast<float>(v) * 0.001f; }
		constexpr float operator""_s  (unsigned long long v) { return static_cast<float>(v); }
		constexpr float operator""_min(unsigned long long v) { return static_cast<float>(v) * 60.0f; }

		// ── 각도 → 라디안 ────────────────────────────────
		constexpr float operator""_deg(long double v) { return Deg2Rad(static_cast<float>(v)); }
		constexpr float operator""_rad(long double v) { return static_cast<float>(v); }

		constexpr float operator""_deg(unsigned long long v) { return Deg2Rad(static_cast<float>(v)); }
		constexpr float operator""_rad(unsigned long long v) { return static_cast<float>(v); }

		// ── 속도 → m/s ───────────────────────────────────
		constexpr float operator""_mps (long double v) { return static_cast<float>(v); }
		constexpr float operator""_kmph(long double v) { return static_cast<float>(v * (1000.0L / 3600.0L)); }

		constexpr float operator""_mps (unsigned long long v) { return static_cast<float>(v); }
		constexpr float operator""_kmph(unsigned long long v) { return static_cast<float>(v) * (1000.0f / 3600.0f); }

		// ── 가속도 → m/s^2 ───────────────────────────────
		constexpr float operator""_mps2(long double v) { return static_cast<float>(v); }
		constexpr float operator""_mps2(unsigned long long v) { return static_cast<float>(v); }

		// ── 각속도 → rad/s ───────────────────────────────
		constexpr float operator""_degps(long double v) { return Deg2Rad(static_cast<float>(v)); }
		constexpr float operator""_radps(long double v) { return static_cast<float>(v); }

		constexpr float operator""_degps(unsigned long long v) { return Deg2Rad(static_cast<float>(v)); }
		constexpr float operator""_radps(unsigned long long v) { return static_cast<float>(v); }

	} // namespace units

	// ── 표시용 역변환 (UI·로그 경계에서만) ───────────────
	constexpr float MetersToKm(float m) { return m * 0.001f; }
	constexpr float MpsToKmph(float mps) { return mps * 3.6f; }

} // namespace Shared
