#include "GameTimer.h"
#include <windows.h>

namespace swc {
	GameTimer::GameTimer()
	{
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		secondsPerCount = 1.0 / double(freq.QuadPart);
		Reset();
	}

	void GameTimer::Reset()
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		baseTime = now.QuadPart;
		prevTime = baseTime;
		currTime = baseTime;

		deltaTime = 0.0f;
		frameCount = 0;
		fpsAccumulator = 0.0f;
		fps = 0.0f;
	}

	void GameTimer::Tick()
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		currTime = now.QuadPart;

		double dt = double(currTime - prevTime) * secondsPerCount;
		prevTime = currTime;

		if (dt < 0.0) dt = 0.0;                  // 멀티코어 카운터 점프 방어
		deltaTime = float(dt);
		if (deltaTime > kMaxDelta) deltaTime = kMaxDelta;

		// FPS — 누적이 1초를 넘으면 갱신
		++frameCount;
		fpsAccumulator += deltaTime;
		if (fpsAccumulator >= 1.0f)
		{
			fps = float(frameCount) / fpsAccumulator;
			frameCount = 0;
			fpsAccumulator = 0.0f;
		}
	}

	float GameTimer::TotalTime() const
	{
		return float(double(currTime - baseTime) * secondsPerCount);
	}
}
