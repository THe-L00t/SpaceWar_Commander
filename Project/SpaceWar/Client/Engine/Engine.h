#pragma once

// ★ Network.h 를 맨 앞에 둘 것
//   <winsock2.h> 가 windows.h 보다 먼저 와야 winsock 1.1 과 구조체가 충돌하지 않는다.
//   (자세한 이유는 Network.h 주석)
#include "Client/Net/Network.h"

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include "Client/GRenderer.h"
#include "Client/Sound/SoundManager.h"
#include "Client/Resource/ResourceManager.h"
#include "Client/Scene/SceneManager.h"
#include "Client/Bridge/ClassBridge.h"
#include "Client/State/StateManager.h"
#include "Client/InputManager.h"
#include "Shared/Physics/PhysicsCalculator.h"
#include "Client/Animation/Animator.h"
#include "Client/GameTimer.h"
#include "Client/LegacyScene.h"
#include "Client/Model.h"
#include "Client/SkyPlanet.h"
#include "Client/Camera.h"
#include "Client/PlayerController.h"
#include "Client/Planet.h"
#include "Shared/Terrain/TerrainSampler.h"
#include "Shared/Terrain/PlanetSurface.h"
#include "Shared/Physics/CollisionWorld.h"
#include "Client/DebugView/ColliderDebugView.h"
#include "Client/Weapon/WeaponModels.h"
#include "Client/Weapon/WeaponScene.h"          // 2026-11-15 줍기 · 떨구기 — 든 총 · 바닥 총 그리기
#include "Shared/GameLogic/WeaponShowcase.h"    // 진열 자리(줍는 출처)
#include "Shared/GameLogic/PlayerMovement.h"    // Shared::MoveWorld
#include "Shared/GameLogic/ShotTrace.h"         // Shared::ShotHit — 2026-11-16 TraceVisibleShot
#include "Client/Effects/ParticleSystem.h"      // 2026-11-17 피격 파티클
#include "Client/Effects/Flipbook.h"            // 2026-11-23 파티클 플립북
#include "Client/Effects/ParticleTexture.h"     // 2026-11-24 파티클 낱장 텍스처
#include "Shared/World/WorldCatalog.h"          // 2026-11-19 — NPC 모델 주인 = 맵 번호(kWorldCount)
#include "Client/UI/Hud.h"                      // 2026-11-26 기본 HUD(체력 · 방탄판 · 장탄수 · 나침반)
#include "Shared/GameObject/Player.h"           // 2026-11-26 내 Player(명세 6.6) — 지금은 체력 · 최대 체력만 채운다

// ============================================================
//  Client/Engine/Engine.h — 아키텍처 명세서 1절 · 2절
//
//  엔진의 최상위 계층. 클라이언트 전체를 감싼다.
//  명세 1절 «전체 구조» 의 하위 시스템을 그 순서대로 소유하고,
//  초기화 · 프레임 실행 순서 · 종료(생명주기)를 관리한다.
//
//  ★ 실행 위치 — 메인 스레드 (「멀티스레딩 분류 명세서」 5장)
//    창 메시지 펌프와 프레임 루프를 여기서 돈다. 시작·종료 순서는 9.1 을 따른다.
//  ★ 명세 2절 — Engine 은 게임 로직을 직접 수행하지 않는다.
//    그 자리를 맡을 Class Bridge · State Manager · Scene 의 GameObject 가 아직 선언만 있어서,
//    main.cpp 에 있던 게임 코드를 일단 그대로 옮겨 왔다.
//    아래 «아직 자리로 옮기지 않은 것» 과 Engine.cpp 의 UpdatePlayer · UpdateNetwork 가 그것이다.
// ============================================================

namespace swc {

	class Engine
	{
	public:
		Engine();
		~Engine();

		bool Initialize(HINSTANCE, int showCommand);
		void Run();
		void Shutdown();

	private:
		bool PumpMessages();

		// ── 로딩 (2026-10-09) ──────────────────────────────────
		//  게임 씬에 필요한 것을 LoadBatch 로 쌓는다. SceneManager 가 이 묶음이
		//  끝날 때까지 로딩 씬을 띄우고, 끝나면 게임 씬으로 넘긴다.
		//  ★ 게임 씬이 아직 Engine 안에 있어서 묶음도 여기서 만든다.
		//    GameScene 으로 옮길 때 이 함수도 같이 옮긴다.
		void BuildGameLoad(LoadBatch&);
		void EnterGame();
		void RenderLoading();
		void UpdateLoadingTitle(float);

		void HandleSystemKeys();

		// ── 예시 행성 디버그 씬 (2026-10-28, PlanetDebugScene.h) ──
		//  숫자키 1~5 로 행성을 고른다. 처음이면 로드 묶음을 만들어 로딩 씬을 거치고,
		//  씬에 들어가면 보이는 맵 · 접지면 · 스폰을 그 행성으로 바꿔 끼운다.
		//  ★ 맵 번호 (2026-11-01, 사용자 «맵번호 적용해줘») — 행성을 고르면 서버에 JoinWorld 를 보내고,
		//    서버가 그 행성을 판정한다(Shared/World/WorldCatalog.h · Network.h). 답이 오기 전 · 서버가 못 읽었으면 혼자 움직인다.
		//  ★ 0 키 = 처음 씬(게임 맵, 맵 번호 0)으로 돌아간다 (2026-11-14 사용자 «처음 씬을 0번 키에 할당해주고»).
		//    게임 맵은 처음에 이미 읽었으므로 묶음 없이 넘어가고, 들어가면 ReturnToGameMap 이 보이는 맵 · 접지면 · 스폰을 되돌린다.
		void HandleDebugPlanetKeys();
		void BuildDebugPlanetLoad(LoadBatch&, size_t index);
		void EnterDebugPlanet(size_t index);
		void ReturnToGameMap();
		Vec3d GameMapSpawnPoint() const;   // 게임 맵 스폰(착륙 지점 L1 방향, 지면 위 kGroundOffset)
		void UpdateDebugPlanetObjects();
		size_t RasterObjectBudget() const;   // 2026-11-17 — 래스터 오브젝트 메시 예산(렌더러 상한 − 다른 몫 − 여유)
		void ParkNetworkNodes();
		bool InDebugPlanet() const { return activeDebugPlanet != kNoDebugPlanet; }
		// 지금 보이는 맵의 번호 — 게임 맵 0, 예시 행성 1 ~ 5(Shared/World/WorldCatalog.h).
		uint32_t CurrentWorld() const;

		void UpdatePlayer(float);
		void UpdateWeapons(float);     // 2026-11-15 — 바닥 총 소식 · 든 총 그리기 · 줍기 대상 · E 줍기 · G 떨구기
		void UpdateFire(float);
		// length = 총구 앞 kTracerStart 부터 그릴 길이(m). 2026-11-16 — 처음 닿는 곳까지만(언덕 · 벽을 뚫지 않게).
		void SpawnTracer(const Vec3d& muzzle, const Vec3d& aim, float length);
		// 2026-11-15 크로스헤어 맞추기 — 카메라 눈에서 화면 가운데(조준선)로 그은 선이 처음 닿는 곳(겨눈 점)을 찾아
		//  총구에서 그 점으로 가는 방향을 돌려준다. 겨눌 것이 없으면 사거리 끝 점. range = 든 총의 사거리(m).
		Vec3d AimDirection(const Vec3d& muzzle, double range) const;
		// 2026-11-16 — 클라에서 한 발이 처음 닿는 곳(Shared::TraceShot). 몸 = 연결돼 있을 때 그려진 원격 · NPC 자리,
		//  충돌체 · 지형 = 지금 맵, 지형 허용 0(실제 지면). 겨눈 점(AimDirection) · 예광탄 길이(UpdateFire)가 같이 쓴다.
		Shared::ShotHit TraceVisibleShot(const Vec3d& origin, const Vec3d& direction, double maxDistance, double minDistance) const;
		void UpdateNetwork(float);
		void RenderFrame();
		void UpdateTitle(float);

		// ── 자동 프레임 시간 측정 (2026-11-17, Engine.cpp kAutoFrameBudget — 평소에는 꺼짐) ──
		//  예시 행성 1 ~ 5 를 차례로 들어가 CPU · GPU 구간 시간을 모아 exe 옆 파일로 쓰고 창을 닫는다.
		void RequestDebugPlanet(size_t index);   // 숫자키 · 자동 측정이 같이 쓰는 «예시 행성 index 로 넘어가기»
		void UpdateFrameBudget(float dt, double updateMs, double renderMs, double endFrameMs);
		void ResetFrameBudgetStage();
		void WriteFrameBudget() const;
		int                       budgetStage = 0;          // 0 = 시작 전, 1 ~ 5 = 예시 행성 측정 중, 6 = 끝
		double                    budgetStageTime = 0.0;    // 그 행성에 들어간 뒤 흐른 시간(s)
		uint64_t                  budgetReportMark = 0;     // 마지막으로 본 GPU 보고 번호(GRenderer::GpuTimeReportCount)
		bool                      budgetSkipFirstReport = true;   // 측정 시작 뒤 첫 GPU 보고는 예열과 겹쳐 버린다
		uint32_t                  budgetFrames = 0;
		double                    budgetDtSum = 0.0, budgetUpdateSum = 0.0, budgetRenderSum = 0.0, budgetEndSum = 0.0;   // ms 합
		double                    budgetUpdateMax = 0.0, budgetRenderMax = 0.0, budgetEndMax = 0.0;                     // ms
		std::vector<std::wstring> budgetGpuReports;         // 측정 구간에 나온 GPU 보고 줄
		std::wstring              budgetText;               // 파일에 쓸 내용
		double                    lastEndFrameMs = 0.0;     // 이번 프레임 renderer.EndFrame() 시간(ms) — RenderFrame 이 잰다
		// 2026-11-17 «움직이면서도 한번 측정» — 행성마다 서 있기 뒤 움직이며 한 번 더
		int                       budgetPhase = 0;          // 0 = 서 있기, 1 = 움직이며
		bool                      budgetMoving = false;     // true 면 W + Shift 를 누른 채(InputManager::SetForcedKey) UpdatePlayer 가 카메라를 돌린다
		double                    budgetDistance = 0.0;     // 측정 구간에 움직인 거리(m)
		Vec3d                     budgetLastPos;            // 거리 재기용 앞 프레임 위치
		uint32_t                  budgetRestreams = 0;      // 측정 구간에 오브젝트를 다시 고른 횟수(UpdateDebugPlanetObjects)
		// 2026-11-17 TLAS 목록 분리 (render/tlas-select) — 켜면 예시 행성의 TLAS 를 래스터 목록과 따로 만든다(Engine.cpp kTlasSeparateDefault)
		bool                      tlasSeparate = true;
		uint32_t                  budgetTlasReselects = 0;  // 측정 구간에 TLAS 목록을 다시 고른 횟수
		double                    budgetTlasSelectMax = 0.0;   // 그 가운데 가장 오래 걸린 고르기(ms)
		double                    budgetTlasSelectSum = 0.0;   // 고르기 시간 합(ms)
		size_t                    budgetTlasExamined = 0;   // 마지막 고르기에서 훑은 배치 수
		double                    budgetTlasWarmupMax = 0.0;   // 예열 중(들어간 직후 첫 고르기 포함) 가장 오래 걸린 고르기(ms)
		double                    budgetStreamMax = 0.0;    // 측정 구간 StreamObjects(래스터 고르기) 최대(ms)
		size_t                    rasterOthers = 512;       // 2026-11-17 예시 행성 장면의 «배치 오브젝트가 아닌» 래스터 인스턴스 수(앞 프레임, 처음은 어림 512)
		bool                      lastRtSeparate = false;   // 마지막 메인 패킷이 TLAS 목록을 따로 넘겼나
		size_t                    lastRtCount = 0;          // 그 TLAS 목록(또는 래스터 목록) 크기

		HWND hwnd = nullptr;

		// ── 명세 1절 «전체 구조» ────────────────────────────────
		//  선언 순서 = 문서 순서. Renderer 가 맨 앞이라 소멸은 맨 마지막이다(GPU 자원을 마지막에 푼다).
		GRenderer                 renderer;       // Renderer (3절)
		SoundManager              soundManager;   // Sound Manager (15절) — 선언만
		ResourceManager           resources;      // Resource Manager (4절). 로딩 스레드(파일 I/O)를 소유한다
		SceneManager              sceneManager;   // Scene Manager (5절). 로딩 씬 + 전환. 게임 씬은 아직 아래 LegacyScene
		ClassBridge               classBridge;    // Game Logic └ Class Bridge (8·9절) — 선언만
		                                          //   Game Logic 본체(Shared::GameLogic)는 지금 서버만 쓴다 (2026-09-18 결정)

		Network                   network;        // Network (10절). 스레드 분리는 멀티스레딩 10장 M4
		std::wstring              netStatus;
		std::vector<RemoteView>   remoteViews;
		std::vector<NpcView>      npcViews;

		StateManager              stateManager;   // State Manager (11절) — 선언만
		InputManager              input;          // Input Manager (12절)
		Shared::PhysicsCalculator physics;        // Physics / Math Calculator (13절) — 선언만
		Animator                  animator;       // Animator (14절) — 선언만
		GameTimer                 timer;          // Game Timer (16절)

		// ── 아직 자리로 옮기지 않은 것 ──────────────────────────
		Planet                 planet;            // 맵 정보 → Scene 이 가진다 (2026-09-18 결정)
		Shared::PlanetSurface  planetSurface;     // GLB 지형. 렌더와 서버가 같은 구면 좌표를 사용한다.
		Shared::TerrainSampler terrain;
		std::wstring           terrainStatus;

		// ── 정적 오브젝트 충돌체 (2026-10-29, Shared/Physics/CollisionWorld.h) ──
		//  게임 맵 몫. 예시 행성 몫은 PlanetDebugScene::Collision(). 이동 예측(PlayerController)과 C 키 와이어프레임이 쓴다.
		//  서버도 같은 파일로 판정한다(게임 맵 2026-10-29 B안, 예시 행성 2026-11-01 맵 번호).
		Shared::CollisionWorld gameMapCollision;
		ColliderDebugView      colliderView;
		bool                   showColliders = false;   // C 키
		const Shared::CollisionWorld* ActiveCollision() const;

		// 로드 확인용 — 게임에 들어갈 때 콘솔에 «로드 N초 (행성 파싱/캐시)» 를 찍는다(Client/Log.h).
		std::chrono::steady_clock::time_point loadStart{};
		bool                   planetFromCache = false;
		LegacyScene            scene;            // → Scene 노드 + Render World 로 나뉜다 (LegacyScene.h)
		Camera                 camera;            // 명세 1절에 자리가 없다
		PlayerController       controller;        // → Player(6.6) · Game Logic(8절). 입력은 Class Bridge 경유(6.6.3)
		NodeHandle             player = kInvalidNode;
		// 2026-11-26 내 Player(명세 6.6 · DynamicObject 6.4.2) — 사용자 «다이나믹 클래스에 체력 만들고 UI에 체력바 연동 시켜줘».
		//  지금은 health · maxHealth 만 쓴다: health = 서버가 판정한 값(Network::MyHealth, 6.13 · 18절 원칙 4), maxHealth = Shared::Player 기본값 100(Shared::kMaxHealth — Player.cpp).
		//  HUD 체력바는 이 값을 읽는다(UpdateHud). 위치 · 이동은 아직 controller · player 노드가 든다(다른 것은 옮기지 않았다).
		Shared::Player         localPlayer;
		Vec3d                  spawnPosition;     // 접지 보정한 시작 위치. 표시 시 재조회하지 않는다.

		// 캐릭터 모델. 플레이어·원격 플레이어·NPC 가 같은 메시·재질을 공유한다.
		// 명세 1절에 Model 자리가 없다 — 리소스(ModelData)를 GPU 자원 + Scene 노드로 펼치는 어댑터다.
		Model                  characterModel;
		Model                  planetModel;       // 정적 맵 메시/재질. GLB 노드는 같은 GPU 메시를 공유한다.
		NodeHandle             gameMapNode = kInvalidNode;   // 게임 맵의 루트 — 디버그 행성에 들어가면 숨긴다
		SkyPlanet              skyPlanet;         // 충돌 맵과 분리한 하늘 행성의 LOD 표시.

		// 지금 보이는 예시 행성 디버그 씬 번호(0 ~ 4). 게임 맵이면 kNoDebugPlanet.
		static constexpr size_t kNoDebugPlanet = static_cast<size_t>(-1);
		size_t                 activeDebugPlanet = kNoDebugPlanet;

		// 플레이어 캐릭터의 재생 인스턴스 (명세 §14). 스킨이 있는 모델(.glb)일 때만 유효하다.
		// 지금 캐릭터는 OBJ(뼈 없음)라 무효로 남고, Animator 는 아무 일도 하지 않는다.
		AnimationInstance      playerAnimation = kInvalidAnimationInstance;

		// 원격 플레이어 · NPC 노드 → Scene 의 OtherPlayer · NPC (6.10)
		std::unordered_map<uint32_t, NodeHandle> remoteNodes;
		std::vector<NodeHandle>                  freeRemoteNodes;
		// NPC 노드 (2026-11-19, 사용자 «NPC모델은 행성에 소속 시켜서 … 행성 해제 하는 경우는 그래픽 메모리에서 빼줘»)
		//  모델 주인 = 맵 번호(Shared::kWorlds) — 0 = 게임 맵(캐릭터 모델), 1 ~ 5 = 예시 행성 i - 1 의 NPC 모델(PlanetDebugScene::NpcModel).
		//  주인마다 빈 노드 목록을 따로 둔다 — 노드는 그 모델의 GPU 메시를 가리키므로 다른 모델 자리에 다시 쓰지 않는다.
		//  행성을 내리기 전에 그 주인의 노드를 장면에서 비운다(ReleaseNpcNodes) — 그 뒤 행성이 모델을 GPU 에서 놓는다.
		struct NpcNode
		{
			NodeHandle node = kInvalidNode;
			uint32_t   owner = 0;   // 모델 주인(맵 번호)
		};
		std::unordered_map<uint32_t, NpcNode> npcNodes;
		std::vector<NodeHandle>               freeNpcNodes[Shared::kWorldCount];
		uint32_t     NpcModelOwner() const;              // 지금 맵의 NPC 모델 주인(행성 NPC 모델이 없으면 0 = 캐릭터 모델)
		const Model& NpcModelOf(uint32_t owner) const;
		// 2026-11-25 — 지금 맵 NPC 의 몸 충돌체(몬스터 COL_). 몬스터 모델로 그리고 있고 충돌체도 읽었을 때만 — 아니면 nullptr(캐릭터 캡슐).
		//  겨눈 점(TraceVisibleShot)과 C 키 와이어가 같은 것을 쓴다(그린 것 = 판정한 것).
		const Shared::CollisionWorld* NpcBodyCollision() const;
		void         ReleaseNpcNodes(uint32_t owner);     // 그 주인의 NPC 노드를 장면에서 비운다(LegacyScene::ClearSubtree — 다시 쓰지 않는다)

		// ── 사격 효과 (임시) ────────────────────────────────
		//  얇고 긴 상자를 발사선에 잠깐 놓는 것뿐이다. 명세에는 이펙트 자리가 없다 —
		//  나중에 Scene 의 표시 요소나 파티클(설계 v1)로 옮긴다.
		//  노드는 원격 플레이어와 같은 방식으로 재사용한다(Scene 에 삭제 API 가 없다).
		struct Tracer
		{
			NodeHandle node = kInvalidNode;
			float      life = 0.0f;      // 남은 시간 (s)
		};

		MeshHandle              tracerMesh = kInvalidMesh;
		std::vector<Tracer>     tracers;
		std::vector<NodeHandle> freeTracerNodes;

		// ── 피격 파티클 (2026-11-17, Client/Effects/ParticleSystem.h · 명세 21절) ──
		//  UpdateFire 가 내 발이 처음 닿는 곳에 효과를 만들고, Run 이 매 프레임 움직이고(UpdateNetwork 뒤), RenderFrame 이 패킷에 담는다.
		ParticleSystem          particles;
		// 파티클 플립북 (2026-11-23 사용자 «리소스 매니저에서 읽어 넘겨주면 플립북으로 다룰 수 있도록»)
		//  BuildGameLoad 단계 0 이 ResourceManager::RequestTexture 로 색 · 노멀을 읽어 Flipbook::Create 로 올리고 층에 붙인다.
		//  파일이 없으면 그 그림만 빠진다(로그만). 표 = Engine.cpp kParticleFlipbooks — 칸 수 = 표 줄 수(static_assert).
		//  2026-11-24 — 피 1 + 먼지 3 + 연기 3(사용자 «먼지 전부, 연기 전부»). ParticleSystem::AddFlipbook 으로 층에 여러 장.
		//  2026-11-25 — 피 1 → 8(유체 시뮬 4판 × 정면 · 측면, 판마다 한 묶음 — 사용자 «정면 측면 둘다 출력»).
		std::array<Flipbook, 14> particleFlipbooks;
		// 파티클 낱장 텍스처 (2026-11-24 사용자 «일반 텍스처용 클래스 하나 만들어줘. 회전이 가능하게»)
		//  플립북과 같은 길로 읽어 ParticleTexture::Create → ParticleSystem::AddTexture. 표 = Engine.cpp kParticleTextures.
		//  2026-11-24 — 먼지 13 + 연기 50(«먼지 전부, 연기 전부»).
		std::array<ParticleTexture, 63> particleTextures;

		// ── 총 모델 (2026-11-14, Shared/GameObject/Weapon.h · Client/Weapon/WeaponModels.h) ──
		//  총 종류 × 1인칭 · 3인칭을 게임 로드 때 한 번 올린다. 명세에는 21절 «작업 중 추가된 클래스» 에 있다.
		//  진열(weaponShowcase) — 사용자 «씬 처음 허공에 1인칭 모델과 3인칭 모델 총 6개 띄우는거까지만»:
		//  게임 맵 스폰 앞 허공에 6개를 띄워 모델을 확인한다. 예시 행성에 들어가면 숨기고, 0 키로 돌아오면 다시 보인다.
		WeaponModels            weaponModels;
		NodeHandle              weaponShowcase = kInvalidNode;
		void BuildWeaponShowcase();
		// 2026-11-15 사용자 «충돌체도 같이 적용은 해줘봐» — BuildWeaponShowcase 가 진열 6개에 총마다 상자 묶음 충돌체
		//  (WeaponSpec::colliderBoxes)를 gameMapCollision 에 넣는다. 자리는 Shared/GameLogic/WeaponShowcase.h —
		//  서버도 같은 자리에 같은 충돌체를 넣는다(이동 판정이 서버 권위라서).
		Shared::WeaponShowcaseSlot showcaseSlots[Shared::kWeaponShowcaseSlotCount];   // 진열 자리 = 줍는 출처(2026-11-15)
		bool                       hasShowcaseSlots = false;

		// ── 총 줍기 · 떨구기 · 든 총으로 쏘기 (2026-11-15, Shared/GameLogic/WeaponPickup.h · Client/Weapon/WeaponScene.h) ──
		//  사용자: «주우면 애니메이션 시에 적절한 위치에 총을 고정시키고, 쏠 수 있도록 해줘. 줍기, 떨구기 둘 다 구현하면 되는거야.
		//          이때 공중에 있는것을 줍고, 떨구면 땅바닥에 떨어지도록. 공중에 있는 총은 사라지지 않게»
		//  접속 중 = 서버가 정한 것(network.MyWeapon · WeaponOf · 바닥 총 소식), 오프라인 = 아래 offline* 로 혼자 처리.
		WeaponScene               weaponScene;
		Shared::WeaponType        offlineHeld = Shared::WeaponType::None;   // 오프라인일 때 든 총
		uint32_t                  offlineNextGroundId = 1;
		std::vector<GroundWeaponEvent> groundEventScratch;                   // 네트워크에서 꺼낸 바닥 총 소식(매 프레임 재사용)
		struct PickupTarget
		{
			bool               valid = false;
			uint8_t            source = 0;          // Shared::kPickupFrom*
			uint32_t           id = 0;              // 진열 자리 번호 · 바닥 총 번호
			Shared::WeaponType type = Shared::WeaponType::None;
			double             distance = 0.0;
		};
		PickupTarget              pickupTarget;     // 지금 E 로 주울 총(가장 가까운 것, Shared::kWeaponPickupRange 안)
		float                     fireCooldown = 0.0f;   // 다음 발까지 남은 시간(s) — 든 총의 발 간격. 0 이면 «게이트가 열림»
		uint32_t                  shotsFired = 0;        // 예광탄 간격(WeaponSpec::tracerEvery) 세기
		Shared::WeaponType HeldWeapon() const;           // 지금 든 총(접속 = 서버 값, 오프라인 = offlineHeld)
		Shared::MoveWorld  WeaponMoveWorld() const;      // 떨군 자리 · 진열 자리 계산용(planet · 지형)
		void FindPickupTarget();
		void OfflinePickup(const PickupTarget& target);
		void OfflineDrop();

		// 임시 크로스헤어 (2026-11-14) — 코드로 만든 UI 텍스처. 화면 한가운데(조준선)에 그린다(Engine.cpp kCrosshair*).
		TextureHandle           crosshairTexture = kInvalidTexture;
		// NPC 명중 표시 (2026-11-16) — 크로스헤어와 같은 방식의 X(네 분면 대각선, Engine.cpp kHitMarker*). 크로스헤어 위에 겹쳐 그린다.
		//  서버가 «내 발이 NPC 에 맞았다»(Network::TakeNpcHits)고 알리면 hitMarkerTimer 를 kHitMarkerSeconds 로 채운다.
		TextureHandle           hitMarkerTexture = kInvalidTexture;
		float                   hitMarkerTimer = 0.0f;   // 남은 표시 시간(s). 0 이면 그리지 않는다
		// 기본 HUD (2026-11-26) — 체력 · 방탄판 · 장탄수 · 나침반(Client/UI/Hud.h). 게임 씬이 아직 Engine 에 있어 크로스헤어처럼 여기 둔다.
		//  만들기 = 게임 리소스 로드 끝(크로스헤어와 같은 자리), 값 채우기 = UpdateHud(메인), 그리기 = RenderFrame 의 UI 몫.
		Hud                     hud;
		void UpdateHud();

		FramePacket               packet;       // 렌더 추출 ④ — 매 프레임 다시 채우고 메모리는 재사용한다
		std::vector<UISprite>     uiSprites;    // 렌더 추출 ④ 의 UI 몫
		float                     titleTimer = 0.0f;
		float                     memoryLogTimer = 0.0f;

		// 마우스를 다시 잡으려고 누른 클릭이 그대로 사격이 되지 않게 한 프레임 막는다.
		bool                      suppressFire = false;
	};

} // namespace swc
