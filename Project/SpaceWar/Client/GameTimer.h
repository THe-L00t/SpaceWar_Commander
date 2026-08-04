#pragma once
#include <cstdint>

// 고해상도 카운터 기반 프레임 타이머. 매 프레임 Tick() 1회.
namespace swc {
	class GameTimer
	{
	public:
		GameTimer();

		void Reset();
		void Tick();

		float DeltaTime() const { return deltaTime; }
		float TotalTime() const;
		float Fps() const { return fps; }

	private:
		double  secondsPerCount = 0.0;
		int64_t baseTime = 0;
		int64_t prevTime = 0;
		int64_t currTime = 0;

		float deltaTime = 0.0f;

		int   frameCount = 0;
		float fpsAccumulator = 0.0f;
		float fps = 0.0f;

		// 디버거 중단 등으로 생기는 비정상 delta 클램프
		static constexpr float kMaxDelta = 0.10f;
	};
}
