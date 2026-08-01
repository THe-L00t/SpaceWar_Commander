#include <windows.h>
#include <vector>
#include <string>
#include <cstdio>
#include <DirectXMath.h>
#include "GRenderer.h"
#include "Scene.h"
#include "Camera.h"
#include "DummyMesh.h"
#include "GameTimer.h"
#include "Input.h"
#include "PlayerController.h"
#include "RayTracingParams.h"
#include "Resource/ResourceManager.h"
#include "Terrain/TerrainSampler.h"
#include <objbase.h>

using namespace DirectX;

namespace
{
	swc::Input* g_input = nullptr;

	constexpr float kMouseSensitivity = 0.0022f;   // Raw 카운트 -> 라디안

	// 에셋은 빌드 후 exe 옆 assets\ 로 복사된다. 작업 디렉터리와 무관하게 찾는다.
	// ★ 반드시 와이드로 다룬다. GetModuleFileNameA 는 ANSI(CP949)를 주므로
	//   경로에 한글이 있으면 UTF-8 로 오인해 깨진다.
	std::wstring AssetPath(const wchar_t* relative)
	{
		wchar_t exePath[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);

		std::wstring p(exePath);
		const size_t slash = p.find_last_of(L"\\/");
		p = (slash == std::wstring::npos) ? std::wstring() : p.substr(0, slash + 1);
		return p + L"assets\\" + relative;
	}

	LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (g_input && g_input->HandleMessage(msg, wParam, lParam))
			return 0;

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
	// WIC(하이트맵 로더)가 COM 객체다. 이게 없으면 CO_E_NOTINITIALIZED 로 조용히 실패한다.
	if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
		return 1;

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
	{
		MessageBox(hwnd, renderer.StatusText().c_str(), L"렌더러 초기화 실패", MB_OK | MB_ICONERROR);
		return 1;
	}

	swc::Planet planet;   // 반지름 120km, 중심 (0,-R,0), 월드 원점 = 스폰 지점

	// ── 하이트맵 1장을 스폰 위치에 적용 ──
	swc::ResourceManager resources;
	swc::TerrainSampler terrain;

	std::wstring terrainStatus;
	const swc::HeightmapHandle tile = resources.LoadHeightmap(
		AssetPath(L"terrain\\Realistic_Mountain_v00__Realistic_Mountain_v00_Out.png").c_str());
	if (const Shared::HeightmapData* hm = resources.Get(tile))
	{
		terrain.Configure(hm, planet.radius, {});   // 1km / 150m / 25% 감쇠
		planet.terrain = &terrain;

		wchar_t buf[96];
		swprintf_s(buf, L"지형 %ux%u mean %.3f", hm->size, hm->size, hm->mean);
		terrainStatus = buf;
	}
	else
	{
		terrainStatus = L"지형 실패: " + resources.LastError();
	}

	// 더미 메쉬 (파일 없이 코드로 생성) — 테스트용
	// 2.4km 패치 / 513격자 = 정점 간격 4.7m.
	// 변 중앙까지 1,200m 라 지평선(1,084m)을 넘어 패치 끝이 안 보인다.
	swc::MeshData groundData = swc::MakeSpherePatch(planet, 2400.0, 513,
		{ 0.15f, 0.30f, 0.18f });
	swc::MeshData cubeData = swc::MakeCube(2.0f, { 0.90f, 0.45f, 0.15f });
	swc::MeshData noseData = swc::MakeBox(0.5f, 0.5f, 1.0f, { 1.00f, 0.92f, 0.35f });

	swc::MeshHandle groundMesh = renderer.CreateMesh(
		groundData.vertices.data(), groundData.vertices.size(),
		groundData.indices.data(), groundData.indices.size());
	swc::MeshHandle cubeMesh = renderer.CreateMesh(
		cubeData.vertices.data(), cubeData.vertices.size(),
		cubeData.indices.data(), cubeData.indices.size());
	swc::MeshHandle noseMesh = renderer.CreateMesh(
		noseData.vertices.data(), noseData.vertices.size(),
		noseData.indices.data(), noseData.indices.size());

	swc::Scene scene;
	swc::NodeHandle ground = scene.AddNode(swc::kInvalidNode, groundMesh, 0);
	swc::NodeHandle player = scene.AddNode(swc::kInvalidNode, cubeMesh, 0);
	(void)ground;

	// 몸통이 어디를 보는지 눈으로 확인하려고 앞쪽에 자식 노드로 붙인다.
	swc::NodeHandle nose = scene.AddNode(player, noseMesh, 0);
	scene.SetLocalTransform(nose, XMMatrixTranslation(0.0f, 0.0f, 1.3f));

	swc::GameTimer timer;
	swc::Input input;
	swc::Camera camera;
	swc::PlayerController controller;

	g_input = &input;
	input.Initialize(hwnd);

	camera.SetAspect(float(width) / float(height));

	// 스폰 = 월드 원점(구 표면). 큐브 반지름 1 만큼 띄워 발이 땅에 닿게 한다.
	controller.SetPlanet(&planet);
	controller.Spawn(planet.PositionAt({ 0.0, 1.0, 0.0 }, 1.0), { 0.0, 0.0, 1.0 });
	camera.SnapTo(controller.Position(), controller.Up(), controller.Facing());

	std::vector<swc::RenderItem> items;

	ShowWindow(hwnd, nCmdShow);
	input.SetCaptured(true);
	timer.Reset();

	float titleTimer = 0.0f;
	bool running = true;
	MSG msg = {};

	while (running)
	{
		input.BeginFrame();

		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				running = false;
				break;
			}
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		if (!running) break;

		timer.Tick();
		const float dt = timer.DeltaTime();

		// ESC 로 마우스 놓기 / 다시 클릭하면 잡기
		if (input.WasPressed(VK_ESCAPE)) input.SetCaptured(false);
		else if (!input.Captured() && input.MouseDown(0)) input.SetCaptured(true);

		// V = 디버그 뷰 순환, R = RT 토글, [ ] = 룰렛 무릎점(레이 예산)
		if (input.WasPressed('V'))
			renderer.SetDebugMode((renderer.DebugMode() + 1) % 8);
		if (input.WasPressed('R'))
		{
			swc::RayTracingParams p = renderer.GetRayTracingParams();
			p.enabled = !p.enabled;
			renderer.SetRayTracingParams(p);
		}
		if (input.WasPressed(VK_OEM_4) || input.WasPressed(VK_OEM_6))
		{
			swc::RayTracingParams p = renderer.GetRayTracingParams();
			p.rouletteKnee += input.WasPressed(VK_OEM_6) ? 0.05f : -0.05f;
			if (p.rouletteKnee < 0.01f) p.rouletteKnee = 0.01f;
			if (p.rouletteKnee > 2.0f) p.rouletteKnee = 2.0f;
			renderer.SetRayTracingParams(p);
		}

		camera.SetAiming(input.Captured() && input.MouseDown(1));
		if (input.Captured())
		{
			const float scale = kMouseSensitivity * camera.LookScale();
			camera.AddLook(input.MouseDeltaX() * scale, input.MouseDeltaY() * scale);
		}

		controller.Update(dt, input, camera);
		camera.SetSprinting(controller.IsSprinting());
		camera.Update(dt, controller.Position(), controller.Up(), planet);

		scene.SetLocalTransform(player, controller.WorldMatrix());
		scene.UpdateWorldTransforms();
		scene.Extract(items);

		renderer.BeginFrame();
		swc::RenderView view{ };
		view.viewProj = camera.ViewProj();
		view.eyePosition = camera.EyePosition();
		renderer.Render(view, items, scene.WorldData());
		renderer.EndFrame();

		// 델타타임 / 하이브리드 상태를 창 제목으로 확인
		titleTimer += dt;
		if (titleTimer >= 0.5f)
		{
			titleTimer = 0.0f;
			const swc::RayTracingParams& rt = renderer.GetRayTracingParams();
			const wchar_t* rtState = !renderer.SupportsRaytracing() ? L"미지원"
				: (rt.enabled ? L"ON" : L"OFF");

			// 구면 이동 검증용: 고도 / 접지 / 스폰에서의 거리
			const swc::Vec3d& p = controller.Position();
			const double distFromSpawn = swc::Length(p);

			wchar_t title[400];
			swprintf_s(title,
				L"SpaceWar   FPS %.0f  dt %.1fms  |  고도 %.2fm  %s  스폰거리 %.0fm  속도 %.1f  "
				L"|  %s  |  RT %s knee %.2f view %u",
				timer.Fps(), dt * 1000.0f,
				controller.Altitude(), controller.IsGrounded() ? L"접지" : L"공중",
				distFromSpawn, controller.Speed(),
				terrainStatus.c_str(),
				rtState, rt.rouletteKnee, renderer.DebugMode());
			SetWindowText(hwnd, title);
		}
	}

	g_input = nullptr;
	input.SetCaptured(false);
	CoUninitialize();
	return 0;
}
