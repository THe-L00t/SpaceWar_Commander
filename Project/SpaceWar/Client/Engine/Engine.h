#pragma once

// ★ Network.h 를 맨 앞에 둘 것
//   <winsock2.h> 가 windows.h 보다 먼저 와야 winsock 1.1 과 구조체가 충돌하지 않는다.
//   (자세한 이유는 Network.h 주석)
#include "Client/Net/Network.h"

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
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
#include "Client/Camera.h"
#include "Client/PlayerController.h"
#include "Client/Planet.h"
#include "Shared/Terrain/TerrainSampler.h"
#include "Shared/Terrain/PlanetSurface.h"

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
		void UpdatePlayer(float);
		void UpdateFire(float);
		void SpawnTracer(const Vec3d& muzzle, const Vec3d& aim);
		void UpdateNetwork(float);
		void RenderFrame();
		void UpdateTitle(float);

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
		float                     sendAccumulator = 0.0f;
		std::vector<RemoteView>   remoteViews;
		std::vector<NpcView>      npcViews;

		StateManager              stateManager;   // State Manager (11절) — 선언만
		InputManager              input;          // Input Manager (12절)
		Shared::PhysicsCalculator physics;        // Physics / Math Calculator (13절) — 선언만
		Animator                  animator;       // Animator (14절) — 선언만
		GameTimer                 timer;          // Game Timer (16절)

		// ── 아직 자리로 옮기지 않은 것 ──────────────────────────
		Planet                 planet;            // 맵 정보 → Scene 이 가진다 (2026-09-18 결정)
		Shared::PlanetSurface  planetSurface;     // OBJ 지표면. 렌더와 서버가 같은 좌표/배율을 사용한다.
		Shared::TerrainSampler terrain;
		std::wstring           terrainStatus;
		LegacyScene            scene;             // → Scene 노드 + Render World 로 나뉜다 (LegacyScene.h)
		Camera                 camera;            // 명세 1절에 자리가 없다
		PlayerController       controller;        // → Player(6.6) · Game Logic(8절). 입력은 Class Bridge 경유(6.6.3)
		NodeHandle             player = kInvalidNode;

		// 캐릭터 모델. 플레이어·원격 플레이어·NPC 가 같은 메시·재질을 공유한다.
		// 명세 1절에 Model 자리가 없다 — 리소스(ModelData)를 GPU 자원 + Scene 노드로 펼치는 어댑터다.
		Model                  characterModel;
		Model                  planetModel;       // 정적 맵 메시/재질. 행성 중심에 한 번 배치한다.

		// 플레이어 캐릭터의 재생 인스턴스 (명세 §14). 스킨이 있는 모델(.glb)일 때만 유효하다.
		// 지금 캐릭터는 OBJ(뼈 없음)라 무효로 남고, Animator 는 아무 일도 하지 않는다.
		AnimationInstance      playerAnimation = kInvalidAnimationInstance;

		// 원격 플레이어 · NPC 노드 → Scene 의 OtherPlayer · NPC (6.10)
		std::unordered_map<uint32_t, NodeHandle> remoteNodes;
		std::vector<NodeHandle>                  freeRemoteNodes;
		std::unordered_map<uint32_t, NodeHandle> npcNodes;
		std::vector<NodeHandle>                  freeNpcNodes;

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

		std::vector<InstanceData> items;
		std::vector<UISprite>     uiSprites;    // 렌더 추출 ④ 의 UI 몫
		float                     titleTimer = 0.0f;

		// 마우스를 다시 잡으려고 누른 클릭이 그대로 사격이 되지 않게 한 프레임 막는다.
		bool                      suppressFire = false;
	};

} // namespace swc
