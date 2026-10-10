#include "Engine.h"
#include <memory>

#include <shellapi.h>
#include <objbase.h>
#include <cstdio>
#include <DirectXMath.h>
#include "Client/DummyMesh.h"
#include "Client/RayTracingParams.h"
#include "Client/Log.h"
#include "Shared/Model/ModelSource.h"   // 행성 접지면을 파싱 결과(collected)로 만든다

using namespace DirectX;

namespace
{
	swc::InputManager* g_input = nullptr;

	constexpr uint32_t kWidth = 1280;
	constexpr uint32_t kHeight = 720;

	constexpr float kMouseSensitivity = 0.0022f;   // Raw 카운트 -> 라디안

	// 콘솔 로그 창 (Client/Log.h). 로드 단계·모델 캐시·파싱 시간을 찍는다.
	constexpr bool kShowLogConsole = true;

	// 캐릭터 모델. exe 옆 assets\ 기준이다.
	// ★ 파일명이 Client.vcxproj 의 CopyModelAssets 검사에도 적혀 있다 — 바꿀 때 두 곳.
	constexpr const wchar_t* kCharacterModelAsset =
		L"model\\character\\Meshy_AI_01_Arc_Sentinel_1005152429_texture.obj";

	// ── 로딩 ────────────────────────────────────────────────
	//  로딩 화면 이미지. exe 옆 assets\ 기준 (Client.vcxproj 빌드 후 복사).
	constexpr const wchar_t* kLoadingBackgroundAsset = L"ui\\loading_background.png";
	constexpr const wchar_t* kLoadingSpinnerAsset = L"ui\\loading_spinner.png";

	// 프레임당 메인에서 마무리할 로딩 완료분 수 (「멀티스레딩 분류 명세서」 ⑥ 업로드 예산).
	// 행성 모델 GPU 업로드처럼 무거운 것이 한 프레임에 겹치지 않게 1 로 둔다.
	constexpr size_t kLoadFinishesPerFrame = 1;

	// ── 사격 효과 (임시) ───────────────────────────────────
	//  판정과 무관한 «보이는 것» 이다. 어디에 맞았는지는 서버만 아므로 길이는 고정이다.
	//  맞은 표시·총구 화염·소리는 없다.
	constexpr float  kTracerLife = 0.06f;     // 보이는 시간 (s)
	constexpr float  kTracerStart = 1.5f;     // 몸통 밖에서 시작 (m)
	constexpr float  kTracerLength = 120.0f;  // 길이 (m) — 지평선이 125m 다
	constexpr size_t kTracerPool = 4;         // 동시에 보일 수 있는 수

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

	// 쓰지 않는 노드를 화면 밖으로 치워 두는 변환.
	// 행성 지름이 3.2km 이므로 1만 km 아래는 절대 보이지 않는다.
	DirectX::XMMATRIX ParkedTransform()
	{
		return DirectX::XMMatrixTranslation(0.0f, -1.0e7f, 0.0f);
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

	// ★ 로딩 스레드를 멤버 소멸보다 먼저 멈춘다
	//   로딩 중에 창을 닫으면 로딩 스레드가 planet·terrain 을 읽고 있을 수 있다.
	//   그 멤버들은 resources 보다 뒤에 선언돼 먼저 소멸하므로, 여기서 join 해 둔다.
	Engine::~Engine()
	{
		resources.StopLoader();
	}

	// 시작 순서 — 「멀티스레딩 분류 명세서」 9.1:
	//   파일 I/O 스레드 → Renderer 초기화 → 로딩 씬 → (로딩 스레드에서) 게임 리소스 → 서버 접속 → 게임 루프
	bool Engine::Initialize(HINSTANCE hInstance, int showCommand)
	{
		// WIC(하이트맵 로더)가 COM 객체다. 이게 없으면 CO_E_NOTINITIALIZED 로 조용히 실패한다.
		if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
			return false;

		// 로드·캐시 로그를 볼 콘솔 창. 끄려면 kShowLogConsole 을 false 로.
		if (kShowLogConsole)
			OpenLogConsole();
		Log(L"SpaceWar 시작");

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

		// 창은 맨 먼저 띄운다 — 렌더러·로딩 화면 초기화 동안에도 실행됐다는 것이 보이게.
		ShowWindow(hwnd, showCommand);
		UpdateWindow(hwnd);

		// 9.1 시작 순서 3 — 파일 I/O 스레드를 Renderer 보다 먼저 띄운다.
		if (!resources.StartLoader())
		{
			MessageBox(hwnd, L"로딩 스레드를 만들지 못했습니다.", L"초기화 실패", MB_OK | MB_ICONERROR);
			return false;
		}

		if (!renderer.Initialize(hwnd, kWidth, kHeight))
		{
			MessageBox(hwnd, renderer.StatusText().c_str(), L"렌더러 초기화 실패", MB_OK | MB_ICONERROR);
			return false;
		}

		// 로딩 씬 — 이 이미지가 없으면 띄우지 않는다. 검은 화면으로 조용히 넘어가면
		// «로딩이 멈췄다» 와 «로딩 화면이 안 나온다» 를 구분할 수 없다.
		std::wstring loadingError;
		if (!sceneManager.Initialize(renderer, resources,
			AssetPath(kLoadingBackgroundAsset), AssetPath(kLoadingSpinnerAsset), loadingError))
		{
			MessageBox(hwnd, loadingError.c_str(), L"로딩 화면 초기화 실패", MB_OK | MB_ICONERROR);
			return false;
		}

		g_input = &input;
		input.Initialize(hwnd);

		camera.SetAspect(float(kWidth) / float(kHeight));

		// 게임 씬 리소스는 로딩 스레드로 넘긴다. 끝날 때까지 로딩 씬이 돈다.
		std::unique_ptr<LoadBatch> gameLoad = std::make_unique<LoadBatch>(L"게임");
		BuildGameLoad(*gameLoad);
		loadStart = std::chrono::steady_clock::now();
		sceneManager.ChangeScene(SceneId::Game, std::move(gameLoad), resources);
		return true;
	}

	// ── 게임 씬 로드 묶음 ───────────────────────────────────────
	//  단계 0 (로딩 스레드) 행성 OBJ 파싱 «한 번» → 같은 결과로 접지면(PlanetSurface)과 렌더 메시를 함께 만든다
	//                       → (메인) 지형 연결 · GPU 업로드 · 씬 배치 · RAM 사본 해제
	//         (로딩 스레드) 캐릭터 모델 파싱       → (메인) GPU 업로드 · 애니메이션 · RAM 사본 해제
	//  단계 1 (메인)        예광탄 · 플레이어 노드 · 스폰(지형을 쓰므로 단계 0 뒤) · 서버 접속
	//
	//  ★ GPU 업로드는 메인에서 한다 — 렌더 몫(명세 ⑥). 한 프레임에 하나씩(kLoadFinishesPerFrame).
	//  ★ planetSurface 는 로딩 스레드가 채운다 (onParsed 훅)
	//    로딩 중 메인은 로딩 씬만 그리므로 planetSurface 를 건드리지 않는다. 다 채운 뒤에야
	//    finish(메인)가 지형에 연결한다 — 완료 큐의 락이 그 순서를 보장한다.
	//  ★ 모델이 없으면 게임을 띄우지 않는다 — 큐브로 조용히 돌아가면
	//    «모델이 안 나온다» 를 빌드 문제로 착각한다. 실패하면 묶음이 실패로 끝나고 오류 창이 뜬다.
	void Engine::BuildGameLoad(LoadBatch& batch)
	{
		// ── 행성 맵 (OBJ) ──────────────────────────────────────
		// 지표면 판정은 Shared가 맡고 렌더는 기존 ResourceManager → Model 경로를 사용한다.
		// 전체 경계에는 건물/파편이 들어 있으므로 원점과 Planet_Core의 기준 반경으로 맞춘다.
		// planet — 반지름 1.6km (Planet.h kPlanetRadius), 중심 (0,-R,0), 월드 원점 = 스폰 지점
		const std::wstring planetAsset = AssetPath(Shared::kPlanetModelAsset);

		// ★ 행성 OBJ 는 한 번만 파싱한다 (2026-10-09)
		//   렌더 메시는 재질 기준으로 합쳐 읽으면서(드로우 18개), 같은 파싱에서 접지 오브젝트의 면만
		//   collected 로 따로 모은다(collectObject). 그걸로 로딩 스레드에서 접지면 BVH 를 바로 만든다.
		ModelRequestOptions planetOptions;
		planetOptions.collectObject = &Shared::PlanetSurface::IsGroundObject;
		planetOptions.onParsed = [this](const Shared::ModelSource& source, std::wstring& error)
		{
			return planetSurface.Build(source.collected, error);
		};
		// 디스크 캐시 태그 — 접지면 규칙이 바뀌면 PlanetSurface::kGroundRuleTag 버전이 올라가 캐시를 다시 만든다.
		planetOptions.cacheTag = Shared::PlanetSurface::kGroundRuleTag;

		resources.RequestModelFile(batch, planetAsset,
			[this, &batch](const ModelFileResources& loaded)
			{
				// 접지면 — 로딩 스레드가 파싱 직후 만들어 두었다.
				if (!planetSurface.Valid())
				{
					batch.SetFailureDetail(L"행성 지표면 로드 실패: " + resources.LastError());
					return false;
				}
				terrain.Configure(&planetSurface, planet.radius);
				planet.terrain = &terrain;
				terrainStatus = L"OBJ 행성 지표면 " + std::to_wstring(planetSurface.TriangleCount()) + L" 삼각형";
				// 캐시 여부는 게임에 들어갈 때 콘솔에 찍는다 — 두 번째 실행부터 «캐시» 가 보여야 정상이다.
				planetFromCache = loaded.fromCache;

				const ModelData* planetData = resources.Get(loaded.model);
				if (!planetData)
				{
					batch.SetFailureDetail(L"행성 모델 로드 실패: " + resources.LastError());
					return false;
				}
				const float planetScale = float(planet.radius / Shared::kPlanetModelReferenceRadius);
				XMFLOAT4X4 planetVisual;
				XMStoreFloat4x4(&planetVisual, XMMatrixScaling(planetScale, planetScale, planetScale));
				if (!planetModel.Initialize(*planetData, renderer, planetVisual))
				{
					batch.SetFailureDetail(L"행성 모델 초기화 실패: " + planetModel.LastError());
					return false;
				}
				// Model이 노드/핸들을 보관하므로 GPU 업로드 후 CPU 메시/픽셀은 필요 없다.
				resources.ReleaseModel(loaded.model);
				const NodeHandle planetNode = planetModel.Instantiate(scene);
				if (planetNode == kInvalidNode)
				{
					batch.SetFailureDetail(L"행성 모델을 장면에 등록하지 못했습니다.");
					return false;
				}
				scene.SetLocalTransform(planetNode,
					XMMatrixTranslation(float(planet.center.x), float(planet.center.y), float(planet.center.z)));
				return true;
			},
			std::move(planetOptions));

		// ── 캐릭터 모델 (OBJ) ──────────────────────────────────
		//  ★ 한 번만 읽고 GPU 자원을 만든다. 플레이어·원격 플레이어·NPC 가 같은 메시·재질을 공유한다
		//    (그래서 BLAS 도 하나다).
		//  ★ 파일 하나에서 메시·스켈레톤·클립이 함께 나온다 (명세 §4 — 종류별로 다른 핸들).
		//    지금 에셋은 OBJ 라 메시만 나오고, .glb 로 바꾸면 스켈레톤·클립도 같이 채워진다.
		resources.RequestModelFile(batch, AssetPath(kCharacterModelAsset),
			[this, &batch](const ModelFileResources& loaded)
			{
				const ModelData* modelData = resources.Get(loaded.model);
				if (!modelData)
				{
					batch.SetFailureDetail(L"캐릭터 모델 로드 실패: " + resources.LastError());
					return false;
				}
				if (!characterModel.Initialize(*modelData, renderer))
				{
					batch.SetFailureDetail(L"모델 초기화 실패: " + characterModel.LastError());
					return false;
				}
				// GPU 업로드가 끝났으므로 CPU 메시·픽셀을 버린다. 스켈레톤·클립은 남는다(Animator 가 쓴다).
				resources.ReleaseModel(loaded.model);

				// 스킨이 있는 모델이면 재생 인스턴스를 만들고 첫 클립을 돌린다 (명세 §14).
				// OBJ 캐릭터는 뼈가 없어 여기를 지나가지 않는다 — .glb 로 내보내면 살아난다.
				if (loaded.skeleton.Valid())
				{
					playerAnimation = animator.Create(loaded.skeleton);
					if (!loaded.animations.empty())
						animator.Play(playerAnimation, loaded.animations.front());
				}
				return true;
			});

		// ── 단계 1: 씬 구성 · 스폰 · 서버 접속 (메인) ──
		batch.NextStage();
		batch.Add(L"씬 구성", nullptr,
			[this](bool)
			{
				// 예광탄 — +Z 로 1m 길이. 쏠 때 Z 만 늘려 발사선에 놓는다.
				MeshData tracerData = MakeBox(0.10f, 0.10f, 1.0f, { 1.00f, 0.85f, 0.30f });
				tracerMesh = renderer.CreateMesh(
					tracerData.vertices.data(), tracerData.vertices.size(),
					tracerData.indices.data(), tracerData.indices.size());

				// 모델은 루트(이동) + 표시 보정 + OBJ 오브젝트 노드로 펼쳐진다. 반환값이 이동 루트다.
				// 화면 밖으로 치울 때도 이 루트만 옮기면 자식이 따라온다.
				player = characterModel.Instantiate(scene);

				// 예광탄 노드는 미리 만들어 화면 밖에 치워 둔다. 쏠 때 하나 꺼내 쓰고 되돌린다.
				freeTracerNodes.reserve(kTracerPool);
				for (size_t i = 0; i < kTracerPool; ++i)
				{
					const NodeHandle tracer = scene.AddNode(kInvalidNode, tracerMesh, 0);
					scene.SetLocalTransform(tracer, ParkedTransform());
					freeTracerNodes.push_back(tracer);
				}

				// 스폰 = 월드 원점(구 표면). 몸통 중심을 1m 띄워 발이 땅에 닿게 한다
				// (모델도 중심 기준으로 1m 내려 배치된다 — Model.cpp kGroundOffset).
				controller.SetPlanet(&planet);
				controller.Spawn(planet.PositionAt({ 0.0, 1.0, 0.0 }, 1.0), { 0.0, 0.0, 1.0 });
				camera.SnapTo(controller.Position(), controller.Up(), controller.Facing());
				return true;
			});

		// ★ 접속은 메인에서 한다 — 네트워크 스레드(M4)가 생기기 전까지 Network 는 메인 것이다.
		//   서버가 없으면 대개 바로 거절되지만, 응답 없는 주소면 그동안 스피너가 멈춘다.
		batch.Add(L"서버 접속", nullptr,
			[this](bool)
			{
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
				return true;   // 접속 실패는 오프라인으로 계속한다 — 지금까지와 같은 동작
			});
	}

	void Engine::EnterGame()
	{
		// 마우스는 게임에 들어갈 때 잡는다. 로딩 중에는 창을 옮기거나 다른 창으로 갈 수 있게 둔다.
		input.SetCaptured(true);
		titleTimer = 0.0f;

		// 게임 리소스 로드에 걸린 시간 — 로딩 씬을 띄운 순간부터 게임에 들어온 순간까지.
		const double seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - loadStart).count();
		Log(L"[게임] 입장 — 로드 %.2fs (행성 %s) · %s", seconds,
			planetFromCache ? L"캐시" : L"파싱", terrainStatus.c_str());
	}

	void Engine::Run()
	{
		timer.Reset();

		for (;;)
		{
			input.BeginFrame();
			if (!PumpMessages())
				break;

			timer.Tick();
			const float dt = timer.DeltaTime();

			// ⑥ 로딩 완료분 — 업로드 예산만큼만 메인에서 마무리한다(게임 중에도 매 프레임).
			resources.PumpLoaded(kLoadFinishesPerFrame);
			sceneManager.Update(dt, resources);

			if (sceneManager.LoadFailed())
			{
				MessageBox(hwnd, sceneManager.LoadError().c_str(), L"로딩 실패", MB_OK | MB_ICONERROR);
				break;
			}
			if (sceneManager.TakeEntered() == SceneId::Game)
				EnterGame();

			if (sceneManager.IsLoading())
			{
				RenderLoading();
				UpdateLoadingTitle(dt);
				continue;
			}

			HandleSystemKeys();
			UpdatePlayer(dt);
			UpdateFire(dt);
			UpdateNetwork(dt);

			// 애니메이션 포즈 — 게임 단계의 «출력» 이다 (멀티스레딩 명세 3.2 ③).
			//  총구 위치·히트박스가 포즈를 쓰므로 렌더보다 먼저, 상태가 다 정해진 뒤에 돌린다.
			//  나중에 캐릭터 범위 단위 잡으로 쪼갠다(M5).
			animator.Update(resources, dt);

			RenderFrame();
			UpdateTitle(dt);
		}
	}

	// 종료 순서 — 9.1: 네트워크를 먼저 끊고, 파일 I/O 스레드를 멈춘다(대기 요청 취소 + join).
	//   GPU 자원은 멤버 소멸 때 Renderer 가 맨 마지막에 푼다.
	void Engine::Shutdown()
	{
		network.Disconnect();
		resources.StopLoader();
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
		suppressFire = false;
		if (input.WasPressed(VK_ESCAPE)) input.SetCaptured(false);
		else if (!input.Captured() && input.MouseDown(0))
		{
			input.SetCaptured(true);
			suppressFire = true;   // 이 클릭은 «잡기» 였다. 총알이 나가면 안 된다
		}

		// V = 디버그 뷰 순환, R = RT 토글, [ ] = 룰렛 무릎점(레이 예산)
		// 11번째(10) 는 재질 확인용 — 거칠기·금속성
		if (input.WasPressed('V'))
			renderer.SetDebugMode((renderer.DebugMode() + 1) % 11);
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
	//   입력은 Class Bridge 를 거쳐야 하고(6.6.3), 이펙트는 표시 계층으로 가야 한다.
	//
	//  ★ 누른 «순간» 에만 한 발 나간다. 누르고 있어도 연사되지 않는다.
	//  ★ 클라는 «어디서 어디로» 만 보낸다. 맞았는지는 서버가 정한다 (명세 18절 원칙 4).
	//    그래서 예광탄은 «맞은 곳» 이 아니라 고정 길이로 그린다. 서버 판정과 무관하다.
	//    총구는 지금 몸통 중심이다. 1인칭으로 바꾸면 이 원점만 눈 위치로 옮기면 된다.
	void Engine::UpdateFire(float dt)
	{
		// 1) 보이는 예광탄의 수명을 줄이고, 끝난 것은 치운다.
		for (size_t i = 0; i < tracers.size(); )
		{
			tracers[i].life -= dt;
			if (tracers[i].life > 0.0f) { ++i; continue; }

			scene.SetLocalTransform(tracers[i].node, ParkedTransform());
			freeTracerNodes.push_back(tracers[i].node);

			tracers[i] = tracers.back();
			tracers.pop_back();
		}

		// 2) 이번 프레임에 쐈는가.
		if (!input.Captured() || suppressFire || !input.MouseWasPressed(0))
			return;

		const Vec3d muzzle = controller.Position();
		const Vec3d aim = camera.LookDirection();

		// 효과는 접속 여부와 무관하게 보인다. 판정만 서버 몫이다.
		if (network.Connected())
		{
			const float origin[3] = { float(muzzle.x), float(muzzle.y), float(muzzle.z) };
			const float dir[3] = { float(aim.x), float(aim.y), float(aim.z) };
			network.SendFire(origin, dir);
		}

		SpawnTracer(muzzle, aim);
	}

	// 발사선 위에 상자를 놓는다. +Z 를 조준 방향에 맞추고 Z 만 길이만큼 늘린다.
	void Engine::SpawnTracer(const Vec3d& muzzle, const Vec3d& aim)
	{
		if (tracerMesh == kInvalidMesh) return;

		NodeHandle node = kInvalidNode;

		if (!freeTracerNodes.empty())
		{
			node = freeTracerNodes.back();
			freeTracerNodes.pop_back();
		}
		else if (!tracers.empty())
		{
			// 풀이 다 차 있다 = 가장 오래된 것을 빼서 다시 쓴다.
			node = tracers.front().node;
			tracers.erase(tracers.begin());
		}
		else
		{
			return;
		}

		// 조준 방향을 Z 축으로 하는 기저. up 과 나란해질 일은 없다(pitch 가 66도로 묶여 있다).
		const Vec3d zAxis = aim;
		const Vec3d xAxis = Normalize(Cross(controller.Up(), zAxis));
		const Vec3d yAxis = Cross(zAxis, xAxis);

		const Vec3d center = muzzle + aim * (double(kTracerStart) + double(kTracerLength) * 0.5);

		XMMATRIX m;
		m.r[0] = XMVectorSet(float(xAxis.x), float(xAxis.y), float(xAxis.z), 0.0f);
		m.r[1] = XMVectorSet(float(yAxis.x), float(yAxis.y), float(yAxis.z), 0.0f);
		m.r[2] = XMVectorSet(float(zAxis.x) * kTracerLength,
			float(zAxis.y) * kTracerLength,
			float(zAxis.z) * kTracerLength, 0.0f);
		m.r[3] = XMVectorSet(float(center.x), float(center.y), float(center.z), 1.0f);

		scene.SetLocalTransform(node, m);

		Tracer t;
		t.node = node;
		t.life = kTracerLife;
		tracers.push_back(t);
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

		// ── 리스폰 ──────────────────────────────────────────
		//  ★ 클라가 스스로 되살아나지 않는다. 체력이 0 인지도 서버가 판정하고,
		//    돌아갈 좌표도 서버가 보낸 것을 그대로 쓴다 (명세 18절 원칙 4).
		float respawn[3];
		if (network.TakeRespawn(respawn))
		{
			controller.Spawn(Vec3d(double(respawn[0]), double(respawn[1]), double(respawn[2])),
				controller.Facing());
			camera.SnapTo(controller.Position(), controller.Up(), controller.Facing());
		}

		const XMMATRIX parkedTransform = ParkedTransform();

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
					handle = characterModel.Instantiate(scene);
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
					// NPC 도 같은 모델·재질을 쓴다(client2 이식 방침). 눈으로 가리는 것은
					// 머리 위 HP BAR 로 한다 — 교수님 09-15 ToDo.
					handle = characterModel.Instantiate(scene);
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

		uiSprites.clear();
		sceneManager.ExtractUI(uiSprites);

		renderer.BeginFrame();
		RenderView view{ };
		view.viewProj = camera.ViewProj();
		view.eyePosition = camera.EyePosition();
		renderer.Render(view, items, scene.WorldData());
		renderer.DrawSprites(uiSprites);
		renderer.EndFrame();
	}

	// 로딩 씬 — 3D 는 그리지 않고 UI(배경 + 스피너)만 그린다.
	void Engine::RenderLoading()
	{
		uiSprites.clear();
		sceneManager.ExtractUI(uiSprites);

		renderer.BeginFrame();
		renderer.DrawSprites(uiSprites);
		renderer.EndFrame();
	}

	// 로딩 진행을 창 제목으로 확인 — 화면에는 글자를 그릴 수단이 아직 없다.
	void Engine::UpdateLoadingTitle(float dt)
	{
		titleTimer += dt;
		if (titleTimer < 0.25f)
			return;
		titleTimer = 0.0f;

		const LoadBatch* batch = sceneManager.CurrentBatch();
		if (!batch)
			return;

		wchar_t title[256];
		swprintf_s(title, L"SpaceWar   로딩 중  %zu/%zu  (%.0f%%)  %s",
			batch->FinishedCount(), batch->TaskCount(), batch->Progress() * 100.0f,
			batch->CurrentLabel().c_str());
		SetWindowText(hwnd, title);
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
		wchar_t netText[240];
		if (network.Connected())
		{
			// 체력은 서버가 보내준 값만 쓴다. 아직 못 받았으면 물음표.
			wchar_t healthText[24];
			if (network.MyHealth() >= 0.0f)
				swprintf_s(healthText, L"%.0f", network.MyHealth());
			else
				swprintf_s(healthText, L"?");

			swprintf_s(netText,
				L"나=%u  체력 %s  피격 %u  송신 %u  수신 %u  다른플레이어 %u명  NPC %u마리",
				network.MyId(), healthText, network.HitCount(),
				network.SentCount(),
				network.EchoCount(), network.RemoteCount(),
				network.NpcCount());
		}
		else
		{
			swprintf_s(netText, L"%s", netStatus.c_str());
		}

		// 가속 구조 계측 — scratch 는 빌드 뒤 해제되므로 로드가 끝나면 0 이어야 정상이다.
		wchar_t blasText[128];
		swprintf_s(blasText,
			L"BLAS %zu개 %.0fMB(압축 -%.0fMB, scratch %.0fMB)  TLAS %u/%u%s",
			renderer.BlasCount(),
			double(renderer.BlasResultBytes())     / (1024.0 * 1024.0),
			double(renderer.BlasCompactionSaved()) / (1024.0 * 1024.0),
			double(renderer.BlasScratchBytes())    / (1024.0 * 1024.0),
			renderer.TlasInstanceCount(), renderer.TlasMaxInstances(),
			renderer.TlasDroppedInstances() ? L" ★유실" : L"");

		wchar_t title[1024];
		swprintf_s(title,
			L"SpaceWar   FPS %.0f  dt %.1fms  |  고도 %.2fm  %s  스폰거리 %.0fm  속도 %.1f  "
			L"|  %s  |  %s  |  RT %s knee %.2f view %u  |  %s",
			timer.Fps(), dt * 1000.0f,
			controller.Altitude(), controller.IsGrounded() ? L"접지" : L"공중",
			distFromSpawn, controller.Speed(),
			netText,
			terrainStatus.c_str(),
			rtState, rt.rouletteKnee, renderer.DebugMode(),
			blasText);
		SetWindowText(hwnd, title);
	}

} // namespace swc
