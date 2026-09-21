#include "Engine.h"

#include <shellapi.h>
#include <objbase.h>
#include <cstdio>
#include <DirectXMath.h>
#include "Client/DummyMesh.h"
#include "Client/RayTracingParams.h"

using namespace DirectX;

namespace
{
	swc::InputManager* g_input = nullptr;

	constexpr uint32_t kWidth = 1280;
	constexpr uint32_t kHeight = 720;

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

namespace swc {

	Engine::Engine() = default;
	Engine::~Engine() = default;

	// 시작 순서 — 「멀티스레딩 분류 명세서」 9.1: Renderer 초기화 → 서버 접속 → 게임 루프
	bool Engine::Initialize(HINSTANCE hInstance, int showCommand)
	{
		// WIC(하이트맵 로더)가 COM 객체다. 이게 없으면 CO_E_NOTINITIALIZED 로 조용히 실패한다.
		if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
			return false;

		WNDCLASSEX wc = {};
		wc.cbSize = sizeof(wc);
		wc.style = CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = WndProc;
		wc.hInstance = hInstance;
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wc.lpszClassName = L"SpaceWarWindow";
		RegisterClassEx(&wc);

		RECT rc = { 0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight) };
		AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

		hwnd = CreateWindow(wc.lpszClassName, L"SpaceWar", WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
			nullptr, nullptr, hInstance, nullptr);

		if (!renderer.Initialize(hwnd, kWidth, kHeight))
		{
			MessageBox(hwnd, renderer.StatusText().c_str(), L"렌더러 초기화 실패", MB_OK | MB_ICONERROR);
			return false;
		}

		// planet — 반지름 1.6km (Planet.h kPlanetRadius), 중심 (0,-R,0), 월드 원점 = 스폰 지점

		// ── 하이트맵 1장을 스폰 위치에 적용 ──
		//  ★ 서버도 같은 파일(Shared::kTerrainTileAsset)을 같은 설정으로 읽는다. 여기만 바꾸면 안 된다.
		const HeightmapHandle tile = resources.LoadHeightmap(
			AssetPath(Shared::kTerrainTileAsset).c_str());
		if (const Shared::HeightmapData* hm = resources.Get(tile))
		{
			terrain.Configure(hm, planet.radius, {});   // 1km / 60m / 10% 감쇠 (TerrainConfig 기본값)
			planet.terrain = &terrain;

			wchar_t buf[96];
			swprintf_s(buf, L"지형 %ux%u mean %.3f", hm->size, hm->size, hm->mean);
			terrainStatus = buf;
		}
		else
		{
			terrainStatus = L"지형 실패: " + resources.LastError();
		}

		// 지면 = 큐브 구 6면 전체 메시 (파일 없이 코드로 생성)
		// 면당 321 격자 → 정점 간격 약 7.9m, 정점 61.8만 / 삼각형 123만.
		// 513 이면 4.9m 간격이지만 삼각형 315만이라 BLAS 부담이 크다.
		constexpr int kPlanetFaceGrid = 321;
		MeshData groundData = MakeCubeSphere(planet, kPlanetFaceGrid,
			{ 0.15f, 0.30f, 0.18f });
		MeshData cubeData = MakeCube(2.0f, { 0.90f, 0.45f, 0.15f });
		// NPC 는 붉게 칠해 플레이어(주황)와 눈으로 구분한다.
		MeshData npcData = MakeCube(2.0f, { 0.85f, 0.15f, 0.15f });
		MeshData noseData = MakeBox(0.5f, 0.5f, 1.0f, { 1.00f, 0.92f, 0.35f });

		const MeshHandle groundMesh = renderer.CreateMesh(
			groundData.vertices.data(), groundData.vertices.size(),
			groundData.indices.data(), groundData.indices.size());
		cubeMesh = renderer.CreateMesh(
			cubeData.vertices.data(), cubeData.vertices.size(),
			cubeData.indices.data(), cubeData.indices.size());
		const MeshHandle noseMesh = renderer.CreateMesh(
			noseData.vertices.data(), noseData.vertices.size(),
			noseData.indices.data(), noseData.indices.size());
		npcMesh = renderer.CreateMesh(
			npcData.vertices.data(), npcData.vertices.size(),
			npcData.indices.data(), npcData.indices.size());

		scene.AddNode(kInvalidNode, groundMesh, 0);
		player = scene.AddNode(kInvalidNode, cubeMesh, 0);

		// 몸통이 어디를 보는지 눈으로 확인하려고 앞쪽에 자식 노드로 붙인다.
		const NodeHandle nose = scene.AddNode(player, noseMesh, 0);
		scene.SetLocalTransform(nose, XMMatrixTranslation(0.0f, 0.0f, 1.3f));

		g_input = &input;
		input.Initialize(hwnd);

		camera.SetAspect(float(kWidth) / float(kHeight));

		// 스폰 = 월드 원점(구 표면). 큐브 반지름 1 만큼 띄워 발이 땅에 닿게 한다.
		controller.SetPlanet(&planet);
		controller.Spawn(planet.PositionAt({ 0.0, 1.0, 0.0 }, 1.0), { 0.0, 0.0, 1.0 });
		camera.SnapTo(controller.Position(), controller.Up(), controller.Facing());

		// ── 서버 접속 ───────────────────────────────────────────
		const NetOptions netOpt = ParseCommandLine();
		netStatus = L"오프라인";
		if (netOpt.online)
		{
			std::wstring err;
			if (network.Connect(netOpt.host, netOpt.port, err))
				netStatus = L"접속됨";
			else
				netStatus = L"접속 실패: " + err;
		}

		ShowWindow(hwnd, showCommand);
		return true;
	}

	void Engine::Run()
	{
		input.SetCaptured(true);
		timer.Reset();

		for (;;)
		{
			input.BeginFrame();
			if (!PumpMessages())
				break;

			timer.Tick();
			const float dt = timer.DeltaTime();

			HandleSystemKeys();
			UpdatePlayer(dt);
			UpdateNetwork(dt);
			RenderFrame();
			UpdateTitle(dt);
		}
	}

	// 종료 순서 — 9.1: 네트워크를 먼저 끊는다. GPU 자원은 멤버 소멸 때 Renderer 가 맨 마지막에 푼다.
	void Engine::Shutdown()
	{
		network.Disconnect();
		g_input = nullptr;
		input.SetCaptured(false);
		CoUninitialize();
	}

	bool Engine::PumpMessages()
	{
		MSG msg = {};
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
				return false;

			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		return true;
	}

	void Engine::HandleSystemKeys()
	{
		// ESC 로 마우스 놓기 / 다시 클릭하면 잡기
		if (input.WasPressed(VK_ESCAPE)) input.SetCaptured(false);
		else if (!input.Captured() && input.MouseDown(0)) input.SetCaptured(true);

		// V = 디버그 뷰 순환, R = RT 토글, [ ] = 룰렛 무릎점(레이 예산)
		if (input.WasPressed('V'))
			renderer.SetDebugMode((renderer.DebugMode() + 1) % 10);
		if (input.WasPressed('R'))
		{
			RayTracingParams p = renderer.GetRayTracingParams();
			p.enabled = !p.enabled;
			renderer.SetRayTracingParams(p);
		}
		if (input.WasPressed(VK_OEM_4) || input.WasPressed(VK_OEM_6))
		{
			RayTracingParams p = renderer.GetRayTracingParams();
			p.rouletteKnee += input.WasPressed(VK_OEM_6) ? 0.05f : -0.05f;
			if (p.rouletteKnee < 0.01f) p.rouletteKnee = 0.01f;
			if (p.rouletteKnee > 2.0f) p.rouletteKnee = 2.0f;
			renderer.SetRayTracingParams(p);
		}
	}

	// ★ 게임 로직 — 명세 2절상 Engine 몫이 아니다.
	//   Input Manager → Class Bridge → Game Logic → Player (6.6.3) 로 옮길 대상이다.
	void Engine::UpdatePlayer(float dt)
	{
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
	}

	// ★ 게임 로직 — 명세 2절상 Engine 몫이 아니다.
	//   송신은 Class Bridge → Network(9절), 수신은 Network → State Manager → OtherPlayer · NPC(6.13) 로 옮길 대상이다.
	void Engine::UpdateNetwork(float dt)
	{
		if (!network.Connected())
			return;

		// ★ 좌표 송신은 1/30초마다. 렌더 프레임률과 분리한다.
		sendAccumulator += dt;
		if (sendAccumulator >= kSendInterval)
		{
			sendAccumulator -= kSendInterval;

			const Vec3d& pos = controller.Position();
			network.SendToServer(float(pos.x), float(pos.y), float(pos.z));
		}

		//서버가 뿌린 다른 플레이어의 좌표를 받는다. 논블로킹이라 즉시 돌아온다.
		network.Poll();

		// 행성 지름이 3.2km 이므로 1만 km 아래는 절대 보이지 않는다.
		const XMMATRIX parkedTransform = XMMatrixTranslation(0.0f, -1.0e7f, 0.0f);

		// ── 원격 플레이어 노드 갱신 ─────────────────────
		//  ★ 노드를 지우지 않고 재사용한다
		//    Scene 에 노드 삭제 API 가 없다. 나갈 때마다 새로 만들면
		//    접속·퇴장을 반복하는 동안 노드가 계속 쌓인다.
		//    나간 노드는 화면 밖으로 치워 두었다가 다음 사람에게 다시 쓴다.
		network.RemotePlayers(remoteViews);

		// 이번 프레임 목록에 없는 = 나간 플레이어의 노드를 회수한다.
		for (std::unordered_map<uint32_t, NodeHandle>::iterator it = remoteNodes.begin();
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
			const RemoteView& v = remoteViews[i];

			std::unordered_map<uint32_t, NodeHandle>::iterator found =
				remoteNodes.find(v.playerId);

			if (found == remoteNodes.end())
			{
				NodeHandle handle;
				if (!freeRemoteNodes.empty())
				{
					handle = freeRemoteNodes.back();
					freeRemoteNodes.pop_back();
				}
				else
				{
					handle = scene.AddNode(kInvalidNode, cubeMesh, 0);
				}
				found = remoteNodes.emplace(v.playerId, handle).first;
			}

			scene.SetLocalTransform(found->second,
				XMMatrixTranslation(v.pos[0], v.pos[1], v.pos[2]));
		}

		// ── NPC 노드 갱신 ───────────────────────────────
		//  행동 계산은 전부 서버가 한다. 클라는 좌표를 받아 그리기만 하므로
		//  위 원격 플레이어 갱신과 같은 절차다. 다른 것은 메시 색뿐이다.
		network.Npcs(npcViews);

		// 이번 프레임 목록에 없는 NPC 의 노드를 회수한다.
		for (std::unordered_map<uint32_t, NodeHandle>::iterator it = npcNodes.begin();
			it != npcNodes.end(); )
		{
			bool alive = false;
			for (size_t i = 0; i < npcViews.size(); ++i)
			{
				if (npcViews[i].npcId == it->first) { alive = true; break; }
			}

			if (alive) { ++it; continue; }

			scene.SetLocalTransform(it->second, parkedTransform);
			freeNpcNodes.push_back(it->second);
			it = npcNodes.erase(it);
		}

		for (size_t i = 0; i < npcViews.size(); ++i)
		{
			const NpcView& v = npcViews[i];

			std::unordered_map<uint32_t, NodeHandle>::iterator found =
				npcNodes.find(v.npcId);

			if (found == npcNodes.end())
			{
				NodeHandle handle;
				if (!freeNpcNodes.empty())
				{
					handle = freeNpcNodes.back();
					freeNpcNodes.pop_back();
				}
				else
				{
					handle = scene.AddNode(kInvalidNode, npcMesh, 0);
				}
				found = npcNodes.emplace(v.npcId, handle).first;
			}

			// 고도까지 서버가 지형으로 정해 보낸다. 받은 좌표를 그대로 그린다.
			scene.SetLocalTransform(found->second,
				XMMatrixTranslation(v.pos[0], v.pos[1], v.pos[2]));
		}
	}

	// 렌더 추출(멀티스레딩 3.2 ④) → Renderer
	void Engine::RenderFrame()
	{
		scene.UpdateWorldTransforms();
		scene.Extract(items);

		renderer.BeginFrame();
		RenderView view{ };
		view.viewProj = camera.ViewProj();
		view.eyePosition = camera.EyePosition();
		renderer.Render(view, items, scene.WorldData());
		renderer.EndFrame();
	}

	// 델타타임 / 하이브리드 상태를 창 제목으로 확인
	void Engine::UpdateTitle(float dt)
	{
		titleTimer += dt;
		if (titleTimer < 0.5f)
			return;
		titleTimer = 0.0f;

		const RayTracingParams& rt = renderer.GetRayTracingParams();
		// 임시 스위치로 끈 것과 장치가 못 하는 것을 구분해서 보여준다.
		// 둘을 「미지원」 하나로 뭉치면 RTX 장비에서 «왜 미지원이지» 로 헤맨다.
		const wchar_t* rtState = !kEnableRaytracing ? L"임시끔(래스터만)"
			: !renderer.SupportsRaytracing() ? L"미지원"
			: (rt.enabled ? L"ON" : L"OFF");

		// 구면 이동 검증용: 고도 / 접지 / 스폰에서의 거리
		const Vec3d& p = controller.Position();
		const double distFromSpawn = Length(p);

		// 네트워크 상태 — 보낸 수 / 에코 받은 수 / 마지막 에코 좌표
		wchar_t netText[200];
		if (network.Connected())
		{
			swprintf_s(netText,
				L"나=%u  송신 %u  수신 %u  다른플레이어 %u명  NPC %u마리",
				network.MyId(), network.SentCount(),
				network.EchoCount(), network.RemoteCount(),
				network.NpcCount());
		}
		else
		{
			swprintf_s(netText, L"%s", netStatus.c_str());
		}

		wchar_t title[600];
		swprintf_s(title,
			L"SpaceWar   FPS %.0f  dt %.1fms  |  고도 %.2fm  %s  스폰거리 %.0fm  속도 %.1f  "
			L"|  %s  |  %s  |  RT %s knee %.2f view %u",
			timer.Fps(), dt * 1000.0f,
			controller.Altitude(), controller.IsGrounded() ? L"접지" : L"공중",
			distFromSpawn, controller.Speed(),
			netText,
			terrainStatus.c_str(),
			rtState, rt.rouletteKnee, renderer.DebugMode());
		SetWindowText(hwnd, title);
	}

} // namespace swc
