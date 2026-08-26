// ★ 이 include 순서를 바꾸지 말 것
//   SimpleNet.h 가 <winsock2.h> 를 품고 있다. windows.h 보다 먼저 와야
//   winsock 1.1 과 구조체가 충돌하지 않는다. (자세한 이유는 SimpleNet.h 주석)
#include "Net/SimpleNet.h"

#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <string>
#include <unordered_map>
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

	// ── 서버 전송 주기 ──────────────────────────────────────
	//  렌더는 144fps 로 돌아도 좌표는 1/30초에 한 번만 보낸다.
	//  매 프레임 보내면 대역폭만 낭비되고 서버 처리량이 프레임률에 끌려간다.
	constexpr float kSendInterval = 1.0f / 30.0f;

	// ── 실행 인자 ───────────────────────────────────────────
	//   Client.exe                     127.0.0.1:25000 에 접속 (기본값)
	//   Client.exe 192.168.0.5         그 주소의 25000 포트로 접속
	//   Client.exe 192.168.0.5 27000   주소와 포트 지정
	//   Client.exe --offline           접속하지 않고 단독 실행
	//
	//  ★ 기본을 "접속" 으로 둔다
	//    비주얼 스튜디오에서 F5 를 누르면 인자가 안 붙는다.
	//    기본이 오프라인이면 서버를 켜놓고 F5 를 눌러도 아무 일이 안 일어나서
	//    "왜 좌표가 안 뜨지" 로 헤매게 된다. (실제로 그랬다)
	struct NetOptions
	{
		bool           online = true;              // 기본 = 접속
		char           host[64] = "127.0.0.1";
		unsigned short port = 25000;
	};

	NetOptions ParseCommandLine()
	{
		NetOptions o{};
		int argc = 0;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (!argv) return o;

		int nPositional = 0;
		for (int i = 1; i < argc; ++i)
		{
			if (_wcsicmp(argv[i], L"--offline") == 0) { o.online = false; continue; }

			if (nPositional == 0)
				WideCharToMultiByte(CP_ACP, 0, argv[i], -1, o.host, sizeof(o.host), nullptr, nullptr);
			else if (nPositional == 1)
				o.port = static_cast<unsigned short>(_wtoi(argv[i]));

			++nPositional;
		}
		LocalFree(argv);
		return o;
	}

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

	// ── 원격 플레이어 ───────────────────────────────────────
	//  ★ 노드를 지우지 않고 재사용한다
	//    Scene 에 노드 삭제 API 가 없다. 나갈 때마다 새로 만들면
	//    접속·퇴장을 반복하는 동안 노드가 계속 쌓인다.
	//    나간 노드는 화면 밖으로 치워 두었다가 다음 사람에게 다시 쓴다.
	std::unordered_map<uint32_t, swc::NodeHandle> remoteNodes;
	std::vector<swc::NodeHandle>                  freeRemoteNodes;
	std::vector<swc::RemoteView>                  remoteViews;

	// 행성 반지름이 120km 이므로 그보다 훨씬 먼 곳이면 절대 보이지 않는다.
	const XMMATRIX parkedTransform = XMMatrixTranslation(0.0f, -1.0e7f, 0.0f);

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

	// ── 서버 접속 ───────────────────────────────────────────
	const NetOptions netOpt = ParseCommandLine();
	std::wstring netStatus = L"오프라인";
	if (netOpt.online)
	{
		std::wstring err;
		if (swc::net_connect(netOpt.host, netOpt.port, err))
			netStatus = L"접속됨";
		else
			netStatus = L"접속 실패: " + err;
	}
	float sendAccumulator = 0.0f;

	// ── 진단 로그 ───────────────────────────────────────────
	//  먹통이 되면 화면을 못 보므로 파일로 남긴다.
	//  exe 옆에 diag_client.log 로 떨어진다.
	FILE* diagLog = nullptr;
	{
		wchar_t exePath[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);
		std::wstring p(exePath);
		const size_t slash = p.find_last_of(L"\\/");
		p = (slash == std::wstring::npos) ? std::wstring() : p.substr(0, slash + 1);
		p += L"diag_client.log";
		_wfopen_s(&diagLog, p.c_str(), L"w, ccs=UTF-8");
		if (diagLog)
		{
			fwprintf(diagLog, L"# %s\n", renderer.StatusText().c_str());
			fwprintf(diagLog, L"# 경과  fps  TLAS누적  커밋  상주  VRAM  공유\n");
			fflush(diagLog);
		}
	}
	float elapsed = 0.0f;

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
		elapsed += dt;

		// ESC 로 마우스 놓기 / 다시 클릭하면 잡기
		if (input.WasPressed(VK_ESCAPE)) input.SetCaptured(false);
		else if (!input.Captured() && input.MouseDown(0)) input.SetCaptured(true);

		// V = 디버그 뷰 순환, R = RT 토글, [ ] = 룰렛 무릎점(레이 예산)
		if (input.WasPressed('V'))
			renderer.SetDebugMode((renderer.DebugMode() + 1) % 10);
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

		// ── 네트워크 ────────────────────────────────────────
		//  ★ 좌표 송신은 1/30초마다. 렌더 프레임률과 분리한다.
		if (swc::net_connected())
		{
			sendAccumulator += dt;
			if (sendAccumulator >= kSendInterval)
			{
				sendAccumulator -= kSendInterval;

				const swc::Vec3d& pos = controller.Position();
				swc::send_to_server(float(pos.x), float(pos.y), float(pos.z));
			}

			//서버가 뿌린 다른 플레이어의 좌표를 받는다. 논블로킹이라 즉시 돌아온다.
			swc::net_poll();

			// ── 원격 플레이어 노드 갱신 ─────────────────────
			swc::net_remote_players(remoteViews);

			// 이번 프레임 목록에 없는 = 나간 플레이어의 노드를 회수한다.
			for (std::unordered_map<uint32_t, swc::NodeHandle>::iterator it = remoteNodes.begin();
				it != remoteNodes.end(); )
			{
				bool alive = false;
				for (size_t i = 0; i < remoteViews.size(); ++i)
				{
					if (remoteViews[i].playerId == it->first) { alive = true; break; }
				}

				if (alive) { ++it; continue; }

				scene.SetLocalTransform(it->second, parkedTransform);
				freeRemoteNodes.push_back(it->second);
				it = remoteNodes.erase(it);
			}

			// 보이는 플레이어를 그 자리에 놓는다. 처음 보는 번호면 노드를 하나 붙인다.
			for (size_t i = 0; i < remoteViews.size(); ++i)
			{
				const swc::RemoteView& v = remoteViews[i];

				std::unordered_map<uint32_t, swc::NodeHandle>::iterator found =
					remoteNodes.find(v.playerId);

				if (found == remoteNodes.end())
				{
					swc::NodeHandle handle;
					if (!freeRemoteNodes.empty())
					{
						handle = freeRemoteNodes.back();
						freeRemoteNodes.pop_back();
					}
					else
					{
						handle = scene.AddNode(swc::kInvalidNode, cubeMesh, 0);
					}
					found = remoteNodes.emplace(v.playerId, handle).first;
				}

				scene.SetLocalTransform(found->second,
					XMMatrixTranslation(v.pos[0], v.pos[1], v.pos[2]));
			}
		}

		// ★ 디바이스가 죽었으면 렌더를 멈춘다.
		//
		//   여기서 멈추지 않으면 교수님이 본 증상이 그대로 재현된다:
		//   Present 가 즉시 실패해 v-sync 대기가 사라지므로 루프가 최대 속도로 돌고,
		//   매 프레임 커맨드만 쌓여 메모리가 폭주한다. 화면은 새까맣다.
		//   창은 살려둔다 — 제목표시줄에서 제거 사유를 읽을 수 있어야 하기 때문이다.
		if (renderer.IsDeviceLost())
		{
			Sleep(100);              // CPU 를 태우지 않는다
			titleTimer += 0.5f;      // 제목은 계속 갱신되게 둔다
		}
		else
		{

		scene.SetLocalTransform(player, controller.WorldMatrix());
		scene.UpdateWorldTransforms();
		scene.Extract(items);

		renderer.BeginFrame();
		swc::RenderView view{ };
		view.viewProj = camera.ViewProj();
		view.eyePosition = camera.EyePosition();
		renderer.Render(view, items, scene.WorldData());
		renderer.EndFrame();

		}   // 디바이스 정상일 때만 렌더

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

			// 네트워크 상태 — 보낸 수 / 에코 받은 수 / 마지막 에코 좌표
			wchar_t netText[200];
			if (swc::net_connected())
			{
				swprintf_s(netText,
					L"나=%u  송신 %u  수신 %u  다른플레이어 %u명",
					swc::net_my_id(), swc::net_sent_count(),
					swc::net_echo_count(), swc::net_remote_count());
			}
			else
			{
				swprintf_s(netText, L"%s", netStatus.c_str());
			}

			// ── 진단: 메모리가 어디서 늘어나는지 ──────────────
			const swc::DiagInfo& dg = renderer.Diagnostics();
			const double MB = 1024.0 * 1024.0;

			wchar_t title[700];
			swprintf_s(title,
				L"SpaceWar  FPS %.0f  |  RT %s  TLAS %llu회  |  "
				L"커밋 %.0fMB  상주 %.0fMB  VRAM %.0fMB  공유 %.0fMB  |  %s  |  고도 %.1fm",
				timer.Fps(), rtState,
				static_cast<unsigned long long>(dg.tlasBuilds),
				dg.privateBytes / MB, dg.workingSet / MB,
				dg.vramUsed / MB, dg.sharedUsed / MB,
				dg.deviceRemoved ? dg.removedReason.c_str() : L"정상",
				controller.Altitude());
			SetWindowText(hwnd, title);

			// 로그 파일 — 화면을 못 보는 상황(먹통)에도 기록이 남는다
			if (diagLog)
			{
				fwprintf(diagLog,
					L"%7.1fs  fps=%6.1f  tlas=%9llu  commit=%9.1fMB  ws=%9.1fMB  "
					L"vram=%8.1fMB  shared=%8.1fMB  %s\n",
					elapsed, timer.Fps(),
					static_cast<unsigned long long>(dg.tlasBuilds),
					dg.privateBytes / MB, dg.workingSet / MB,
					dg.vramUsed / MB, dg.sharedUsed / MB,
					dg.deviceRemoved ? dg.removedReason.c_str() : L"");
				fflush(diagLog);
			}
		}
	}

	if (diagLog) { fwprintf(diagLog, L"# 정상 종료 (%.1f초)\n", elapsed); fclose(diagLog); }
	swc::net_disconnect();
	g_input = nullptr;
	input.SetCaptured(false);
	CoUninitialize();
	return 0;
}
