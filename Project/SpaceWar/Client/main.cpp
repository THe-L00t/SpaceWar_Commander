#include "Engine/Engine.h"

// 초기화 · 프레임 루프 · 종료는 Engine 이 맡는다 (아키텍처 명세서 1·2절).
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
	swc::Engine engine;
	if (!engine.Initialize(hInstance, nCmdShow))
		return 1;

	engine.Run();
	engine.Shutdown();
	return 0;
}
