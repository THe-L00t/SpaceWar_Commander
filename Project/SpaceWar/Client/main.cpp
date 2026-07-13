#include <windows.h>
#include <vector>
#include <DirectXMath.h>
#include "GRenderer.h"
#include "Scene.h"
#include "Camera.h"
#include "DummyMesh.h"

using namespace DirectX;

namespace
{
	LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (msg == WM_DESTROY)
		{
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
	const uint32_t width = 1280;
	const uint32_t height = 720;

	WNDCLASSEX wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = L"SpaceWarWindow";
	RegisterClassEx(&wc);

	RECT rc = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

	HWND hwnd = CreateWindow(wc.lpszClassName, L"SpaceWar", WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
		nullptr, nullptr, hInstance, nullptr);

	swc::GRenderer renderer;
	if (!renderer.Initialize(hwnd, width, height))
		return 1;

	// 더미 메쉬 (파일 없이 코드로 생성) — 테스트용
	swc::MeshData groundData = swc::MakeGround(100.0f, { 0.15f, 0.30f, 0.18f });
	swc::MeshData cubeData = swc::MakeCube(2.0f, { 0.90f, 0.45f, 0.15f });

	swc::MeshHandle groundMesh = renderer.CreateMesh(
		groundData.vertices.data(), groundData.vertices.size(),
		groundData.indices.data(), groundData.indices.size());
	swc::MeshHandle cubeMesh = renderer.CreateMesh(
		cubeData.vertices.data(), cubeData.vertices.size(),
		cubeData.indices.data(), cubeData.indices.size());

	swc::Scene scene;
	swc::NodeHandle ground = scene.AddNode(swc::kInvalidNode, groundMesh, 0);
	swc::NodeHandle player = scene.AddNode(swc::kInvalidNode, cubeMesh, 0);
	(void)ground;

	swc::Camera camera;
	camera.SetAspect(float(width) / float(height));

	XMFLOAT3 playerPos = { 0.0f, 1.0f, 0.0f };
	const float moveSpeed = 0.25f;
	const float mapLimit = 49.0f;   // 100x100 땅(±50) 안, 큐브 반지름 1 고려

	std::vector<swc::RenderItem> items;

	ShowWindow(hwnd, nCmdShow);

	MSG msg = {};
	while (msg.message != WM_QUIT)
	{
		if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else
		{
			// WASD 로 XZ 평면 이동 (지금은 프레임 단위 고정 속도)
			if (GetAsyncKeyState('W') & 0x8000) playerPos.z += moveSpeed;
			if (GetAsyncKeyState('S') & 0x8000) playerPos.z -= moveSpeed;
			if (GetAsyncKeyState('A') & 0x8000) playerPos.x -= moveSpeed;
			if (GetAsyncKeyState('D') & 0x8000) playerPos.x += moveSpeed;

			// 땅(100x100) 밖으로 못 나가게 좌표 제한
			if (playerPos.x >  mapLimit) playerPos.x =  mapLimit;
			if (playerPos.x < -mapLimit) playerPos.x = -mapLimit;
			if (playerPos.z >  mapLimit) playerPos.z =  mapLimit;
			if (playerPos.z < -mapLimit) playerPos.z = -mapLimit;

			scene.SetLocalTransform(player, XMMatrixTranslation(playerPos.x, playerPos.y, playerPos.z));
			scene.UpdateWorldTransforms();
			scene.Extract(items);

			camera.FollowTarget(playerPos);

			renderer.BeginFrame();
			swc::RenderView view{ };
			view.viewProj = camera.ViewProj();
			renderer.Render(view, items, scene.WorldData());
			renderer.EndFrame();
		}
	}
	return 0;
}
